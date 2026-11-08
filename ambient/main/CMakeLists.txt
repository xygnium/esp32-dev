idf_component_register(SRCS "main.c" "settings.c" "console_cmds.c" "supply.c" "fault.c" "sd.c"
                         "i2c_bus.c" "ds3231.c" "clock.c"
                         "thermistor.c" "sht45.c" "bmp388.c" "sensors.c" "datalog.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi esp_timer console nvs_flash esp_app_format esp_adc
                             esp_driver_spi esp_driver_sdspi fatfs sdmmc
                             esp_driver_i2c esp_driver_gpio esp_rom)

