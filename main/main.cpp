#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_dev.h"

#include "Chip8.h"

// ---------------------------------------------------------------------------
// Display wiring / geometry
// ---------------------------------------------------------------------------
#define LCD_SCLK 12
#define LCD_MOSI 11
#define LCD_CS 10
#define LCD_DC 9
#define LCD_RST 8

#define LCD_WIDTH 320   // after swap_xy (landscape)
#define LCD_HEIGHT 240

#define LCD_HOST SPI2_HOST

// CHIP-8 physical buttons
#define BUTTON_Q GPIO_NUM_1
#define BUTTON_1 GPIO_NUM_2

// CHIP-8 is 64x32. At SCALE 5 it becomes 320x160, centred vertically.
constexpr int CHIP8_WIDTH = 64;
constexpr int CHIP8_HEIGHT = 32;
constexpr int SCALE = 5;
constexpr int BAND_WIDTH = CHIP8_WIDTH * SCALE;    // 320
constexpr int BAND_HEIGHT = CHIP8_HEIGHT * SCALE;  // 160
constexpr int Y_OFFSET = (LCD_HEIGHT - BAND_HEIGHT) / 2;  // 40

constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t GREEN = 0x07E0;

constexpr int CYCLES_PER_FRAME = 20;  // ~600 instructions/second at 60 fps

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static const char *TAG = "SD_CARD";
static const char *LCD_TAG = "LCD";

static esp_lcd_panel_io_handle_t lcd_io = nullptr;
static esp_lcd_panel_handle_t lcd_panel = nullptr;

// Given back by the LCD driver when a color transfer has finished.
// Whoever wants to touch `frame` must take it first.
static SemaphoreHandle_t lcd_done = nullptr;

// Only the CHIP-8 band is stored/sent (320 x 160), not the full screen.
static uint16_t frame[BAND_WIDTH * BAND_HEIGHT];

Chip8 chip8;

int initSDCard();
void initScreen();
void clearBars();
void drawScreen();
void runEmulator();
void initButtons();

// ---------------------------------------------------------------------------
// LCD "transfer finished" callback (runs in an ISR)
// ---------------------------------------------------------------------------
static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t *edata,
                                  void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(lcd_done, &woken);
    return woken == pdTRUE;
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    if (initSDCard() != 1)
    {
        ESP_LOGE(TAG, "Failed to initialize SD card.");
        return;
    }
    ESP_LOGI(TAG, "SD card initialized successfully!");

    if (chip8.LoadRom("/sd/PONG.ch8") != 1)
    {
        ESP_LOGE(TAG, "Failed to load ROM.");
        return;
    }
    ESP_LOGI(TAG, "ROM loaded successfully!");

    initScreen();
    initButtons();
    clearBars();
    runEmulator();  // never returns
}

// ---------------------------------------------------------------------------
// SD card
// ---------------------------------------------------------------------------
int initSDCard()
{
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = 40;
    bus_cfg.miso_io_num = 41;
    bus_cfg.sclk_io_num = 39;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SDSPI_DEFAULT_DMA));

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_42;
    slot_config.host_id = SPI3_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sd", &host, &slot_config,
                                            &mount_config, &card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return 0;
    }

    ESP_LOGI(TAG, "SD card mounted successfully!");
    sdmmc_card_print_info(stdout, card);
    return 1;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
void initScreen()
{
    // 1. SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = LCD_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.sclk_io_num = LCD_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // 2. Semaphore: starts "free" so the first frame can be built right away
    lcd_done = xSemaphoreCreateBinary();
    xSemaphoreGive(lcd_done);

    // 3. Panel IO
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = LCD_CS;
    io_cfg.dc_gpio_num = LCD_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = 80 * 1000 * 1000;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.on_color_trans_done = onColorDone;
    io_cfg.user_ctx = nullptr;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &lcd_io));

    // 4. ST7789 panel
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(lcd_io, &panel_cfg, &lcd_panel));

    // 5. Reset, init, orientation, on
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    ESP_LOGI(LCD_TAG, "Display initialized");
}

// Paint the unused top and bottom bars black once. Only the middle band is
// redrawn after this.
void clearBars()
{
    memset(frame, 0, sizeof(frame));

    xSemaphoreTake(lcd_done, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, LCD_WIDTH, Y_OFFSET, frame);

    xSemaphoreTake(lcd_done, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, Y_OFFSET + BAND_HEIGHT,
                              LCD_WIDTH, LCD_HEIGHT, frame);
}

// Redraws the CHIP-8 band, but only if the emulator's video changed.
void drawScreen()
{
    // How much brightness a pixel loses per frame once it turns off.
    // 255 = no persistence (instant off). Lower = longer trail. Try 64-100.
    constexpr int DECAY = 80;

    static uint8_t level[CHIP8_WIDTH * CHIP8_HEIGHT] = {};
    static bool first = true;

    // Update each pixel's brightness; note whether anything changed.
    bool changed = first;
    for (int i = 0; i < CHIP8_WIDTH * CHIP8_HEIGHT; i++)
    {
        uint8_t old = level[i];
        uint8_t now;

        if (chip8.video[i] != 0)
            now = 255;                                  // lit: full brightness
        else
            now = (old > DECAY) ? (old - DECAY) : 0;    // unlit: fade out

        if (now != old)
        {
            level[i] = now;
            changed = true;
        }
    }
    if (!changed)
        return;
    first = false;

    // Wait until the previous transfer has finished reading `frame`.
    xSemaphoreTake(lcd_done, portMAX_DELAY);

    for (int y = 0; y < CHIP8_HEIGHT; y++)
    {
        for (int x = 0; x < CHIP8_WIDTH; x++)
        {
            // Brightness 0-255 -> green channel 0-63 in RGB565 (bits 5-10)
            uint16_t colour = (uint16_t)(level[y * CHIP8_WIDTH + x] >> 2) << 5;

            for (int dy = 0; dy < SCALE; dy++)
            {
                uint16_t *row = &frame[(y * SCALE + dy) * BAND_WIDTH + x * SCALE];
                for (int dx = 0; dx < SCALE; dx++)
                {
                    row[dx] = colour;
                }
            }
        }
    }

    esp_lcd_panel_draw_bitmap(lcd_panel, 0, Y_OFFSET,
                              LCD_WIDTH, Y_OFFSET + BAND_HEIGHT, frame);
}

// ---------------------------------------------------------------------------
// Emulator loop: one frame = N CPU cycles, one timer tick, one draw (~60 Hz)
// Set CONFIG_FREERTOS_HZ=1000 in menuconfig so 16 ms is really 16 ticks.
// ---------------------------------------------------------------------------
void runEmulator()
{
    TickType_t lastWake = xTaskGetTickCount();

    while (true)
    {
        chip8.keypad[4] = (gpio_get_level(BUTTON_Q) == 0);
        chip8.keypad[1] = (gpio_get_level(BUTTON_1) == 0);

        for (int i = 0; i < CYCLES_PER_FRAME; i++)
        {
            chip8.Cycle();
        }

        chip8.UpdateTimers();
        drawScreen();

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(16));
    }
}

void initButtons()
{
    gpio_config_t button_cfg = {};

    button_cfg.pin_bit_mask =
        (1ULL << BUTTON_Q) | (1ULL << BUTTON_1);

    button_cfg.mode = GPIO_MODE_INPUT;
    button_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    button_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    button_cfg.intr_type = GPIO_INTR_DISABLE;

    ESP_ERROR_CHECK(gpio_config(&button_cfg));

    ESP_LOGI(LCD_TAG, "CHIP-8 buttons initialized");
}