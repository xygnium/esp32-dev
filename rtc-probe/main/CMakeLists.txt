idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_i2c esp_driver_gpio)

# The build machine's UTC offset (for example -0400), so main.c can turn its
#   local build time into UTC. Read when cmake configures.
execute_process(COMMAND date +%z OUTPUT_VARIABLE BUILD_TZ_OFFSET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BUILD_TZ_OFFSET="${BUILD_TZ_OFFSET}")
