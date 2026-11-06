idf_component_register(SRCS "main.c" "settings.c" "console_cmds.c" "supply.c" "fault.c" "sd.c"
                    INCLUDE_DIRS "."
                    REQUIRES wifi esp_timer console nvs_flash esp_app_format esp_adc
                             esp_driver_spi esp_driver_sdspi fatfs sdmmc)

# Test builds only (build-test.sh): adds the `test fault` console command.
if(AMBIENT_TEST_FAULTS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AMBIENT_TEST_FAULTS=1)
endif()
