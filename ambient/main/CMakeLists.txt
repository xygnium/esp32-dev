idf_component_register(SRCS "main.c" "settings.c" "console_cmds.c" "supply.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi esp_timer console nvs_flash esp_app_format esp_adc)
