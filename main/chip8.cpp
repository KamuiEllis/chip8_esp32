#include "chip8.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <random>
#include <iostream>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

using namespace std;

static const char *TAG = "SD_CARD";

Chip8::Chip8()
{
    pc = 0x200;
    opcode = 0;
    index = 0;
    sp = 0;

    for (int i = 0; i < 16; i++)
    {
        registers[i] = 0;
        stack[i] = 0;
        keypad[i] = 0;
    }

    for (int i = 0; i < 4096; i++)
    {
        memory[i] = 0;
    }

    for (int i = 0; i < 64 * 32; i++)
    {
        video[i] = 0;
    }

    delayTimer = 0;
    soundTimer = 0;

    LoadFont();
    LoadRom("D:\\Downloads\\PONG.ch8");
}

int Chip8::LoadRom(const char* path)
{
    FILE* file = fopen(path, "rb");

    if (file == nullptr)
    {
        ESP_LOGE(TAG, "Failed to open ROM: %s", path);
        return 0; // Return 0 to indicate failure
    }

    // Find the ROM file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    rewind(file);

    // Check that the ROM fits in CHIP-8 memory
    if (fileSize <= 0 ||
        fileSize > sizeof(memory) - 0x200) // 0x200 is the start address for CHIP-8 programs
    {
        ESP_LOGE(TAG, "Invalid ROM size: %ld bytes", fileSize);
        fclose(file);
        return 0; // Return 0 to indicate failure
        
    }

    // Load the ROM at address 0x200
    size_t bytesRead = fread(
        &memory[0x200],
        1,
        fileSize,
        file
    );

    fclose(file);

    if (bytesRead != static_cast<size_t>(fileSize))
    {
        ESP_LOGE(TAG, "Failed to read the entire ROM");
        return 0; // Return 0 to indicate failure
    }

    ESP_LOGI(TAG, "ROM loaded: %ld bytes", fileSize);
    return 1;
    
}

void Chip8::LoadFont()
{
    const uint8_t fontset[80] =
    {
        0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
        0x20, 0x60, 0x20, 0x20, 0x70, // 1
        0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
        0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
        0x90, 0x90, 0xF0, 0x10, 0x10, // 4
        0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
        0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
        0xF0, 0x10, 0x20, 0x40, 0x40, // 7
        0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
        0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
        0xF0, 0x90, 0xF0, 0x90, 0x90, // A
        0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
        0xF0, 0x80, 0x80, 0x80, 0xF0, // C
        0xE0, 0x90, 0x90, 0x90, 0xE0, // D
        0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
        0xF0, 0x80, 0xF0, 0x80, 0x80  // F
    };

    for (int i = 0; i < 80; i++)
    {
        memory[0x050 + i] = fontset[i];
    }
}

void Chip8::Cycle() {
    opcode =
        (static_cast<uint16_t>(memory[pc]) << 8)
        | memory[pc + 1];
    pc += 2;
    Execute();
}

void Chip8::Execute() {
    // Extract the first hexadecimal digit.
    uint8_t type = (opcode & 0xF000) >> 12;

    switch (type)
    {
    case 0x3:
    {
        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t nn = opcode & 0x00FF;

        if (registers[x] == nn) {
            pc += 2;
        }

        break;
    }

    case 0x4:
    {
        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t nn = opcode & 0x00FF;

        if (registers[x] != nn) {
            pc += 2;
        }

        break;
    }

    case 0x5:
    {
        if ((opcode & 0x000F) != 0) {
            break;
        }

        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t y = (opcode & 0x00F0) >> 4;

        if (registers[x] == registers[y]) {
            pc += 2;
        }

        break;

    }

    case 0x6:
    {
        // Extract the X register index.
        uint8_t x = (opcode & 0x0F00) >> 8;

        // Extract the 8-bit immediate value NN.
        uint8_t nn = opcode & 0x00FF;

        // Set register VX to NN.
        registers[x] = nn;
        break;
    }
    case 0x7:
    {
        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t nn = opcode & 0x00FF;

        registers[x] += nn;
        break;
    }

    case 0x8:
    {
        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t y = (opcode & 0x00F0) >> 4;
        uint8_t subOpcode = opcode & 0x000F;

        switch (subOpcode)
        {
        
		case 0x0:
		{
			registers[x] = registers[y];
			break;
		}

        case 0x4:
        {
            
            uint16_t sum =
                static_cast<uint16_t>(registers[x]) + registers[y];
            
            registers[0xF] = (sum > 0xFF) ? 1 : 0;
            registers[x] = static_cast<uint8_t>(sum);
            break;
        }

        case 0x5:
        {
           
            uint8_t vx = registers[x];
            uint8_t vy = registers[y];

            registers[0xF] = (vx >= vy) ? 1 : 0;
            registers[x] = static_cast<uint8_t>(vx - vy);
            break;
        }
        case 0x6:
        {
            uint8_t vx = registers[x];
            
            registers[0xF] = vx & 0x01;
            registers[x] = vx >> 1;
            break;
        }

        case 0x1: // 8XY1: Bitwise OR
        {
            registers[x] = registers[x] | registers[y];
            break;
        }

        case 0x2: // 8XY2: Bitwise AND
        {
            registers[x] = registers[x] & registers[y];
            break;
        }

        case 0x3: // 8XY3: Bitwise XOR
        {
            registers[x] = registers[x] ^ registers[y];
            break;
        }

        case 0x7: // 8XY7: Set VX = VY - VX
        {
            uint8_t vx = registers[x];
            uint8_t vy = registers[y];

            registers[0xF] = (vy >= vx) ? 1 : 0;
            registers[x] = static_cast<uint8_t>(vy - vx);
            break;
        }

        case 0xE: // 8XYE: Shift VX left
        {
            uint8_t vx = registers[x];

            registers[0xF] = (vx & 0x80) >> 7;
            registers[x] = static_cast<uint8_t>(vx << 1);
            break;
        }


        default:
            break;
        }

        break;
    }

    case 0x9:
    {
        if ((opcode & 0x000F) != 0) {
            break;
        }

        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t y = (opcode & 0x00F0) >> 4;

        if (registers[x] != registers[y]) {
            pc += 2;
        }

        break;
    }

    case 0x1:
    {
        uint16_t address = opcode & 0x0FFF;
        pc = address;
        break;
    }
    case 0x2:
    {
        uint16_t address = opcode & 0x0FFF;
        stack[sp] = pc;
        sp++;
        pc = address;
        break;
    }

    case 0x0:
    {
        if (opcode == 0x00E0) {
            for (int i = 0; i < 64 * 32; i++) {
                video[i] = 0;
            }
        }
        else if (opcode == 0x00EE) {
            if (sp > 0) {
                sp--;
                pc = stack[sp];
            }
        }

        break;
    }

    case 0xA:
    {
        index = opcode & 0x0FFF;
        break;
    }

	case 0xB:
	{
		uint16_t address = opcode & 0x0FFF;
		pc = address + registers[0];
		break;
	}

    case 0xC:
    {
        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t nn = (opcode) & 0x00FF;

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distr(0, 255);

        uint8_t randomByte = distr(gen);

        registers[x] = randomByte & nn;
        break;
    }

    case 0xE:
    {
        uint8_t x = (opcode & 0xF00) >> 8;
        uint8_t lastByte = (opcode) & 0x00FF;

        switch (lastByte) {
        case 0x9E:
            if (registers[x] < 16 && keypad[registers[x]] != 0) {
                pc += 2;
            }

            break;

        case 0xA1:
            if (registers[x] < 16 && keypad[registers[x]] == 0) {
                pc += 2;
            }
            break;
        }

        break;

    }

    case 0xF:
    {
        uint8_t x = (opcode & 0x0F00) >> 8;
        uint8_t lastbyte = opcode & 0x0FF;

        switch (lastbyte) {

		case 0x65: 
        {
			for (uint8_t i = 0; i <= x; i++)
			{
				registers[i] = memory[index + i];
			}
			break;
		}

		case 0x55:
		{
			for (uint8_t i = 0; i <= x; i++)
			{
				memory[index + i] = registers[i];
			}
			break;
		}

		case 0x33:
		{
			uint8_t value = registers[x];
			memory[index] = value / 100;
			memory[index + 1] = (value / 10) % 10;
			memory[index + 2] = value % 10;
			break;
		}

		case 0x1E:
			index += registers[x];
			break;

        case 0x07:
            registers[x] = delayTimer;
            break;

        case 0x15:
            delayTimer = registers[x];
            break;

        case 0x18:
            soundTimer = registers[x];
            break;

        case 0x0A:
        {
            bool keyPressed = false;
            for (uint8_t key = 0; key < 16; key++)
            {
                if (keypad[key] != 0)
                {
                    registers[x] = key;
                    keyPressed = true;
                    break;
                }
            }

            if (!keyPressed)
            {
                pc -= 2;
            }

            break;
        }
        case 0x29:
        {
            uint8_t digit = registers[x];

            if (digit < 16) {
                index = 0x050 + (digit * 5);

            }


            break;
        }
        }

        break;
    }

    case 0xD:
    {
        uint8_t x = registers[(opcode & 0x0F00) >> 8];
        uint8_t y = registers[(opcode & 0x00F0) >> 4];
        uint8_t height = opcode & 0x000F;
        registers[0xF] = 0;
        for (uint8_t row = 0; row < height; row++)
        {
            uint8_t spriteByte = memory[index + row];
            for (uint8_t col = 0; col < 8; col++)
            {
                if ((spriteByte & (0x80 >> col)) != 0)
                {
                    uint16_t pixelIndex = ((y + row) % 32) * 64 + ((x + col) % 64);
                    if (video[pixelIndex] == 1)
                    {
                        registers[0xF] = 1;
                    }
                    video[pixelIndex] ^= 1;
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

void Chip8::UpdateTimers()
{
	if (delayTimer > 0)
	{
		--delayTimer;
	}
	if (soundTimer > 0)
	{
		--soundTimer;
	}
}