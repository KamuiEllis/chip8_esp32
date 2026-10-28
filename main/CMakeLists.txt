idf_component_register(SRCS "main.cpp" "chip8.cpp"
                       INCLUDE_DIRS "." "headers" 
                       REQUIRES fatfs sdmmc esp_lcd driver )
