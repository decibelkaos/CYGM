idf_component_register(SRCS "main.c"
                            "nvs_config.c"
                            "wifi_manager.c"
                            "dexcom_api.c"
                            "libre_api.c"
                            "json_scan.c"
                            "nightscout_api.c"
                            "sd_logger.c"
                            "hardware/led.c"
                            "hardware/buzzer.c"
                            "hardware/battery.c"
                            "hardware/display.c"
                            "hardware/screenshot.c"
                            "hardware/sd_crc_compat.c"
                            "features/time_system.c"
                            "features/weather_system.c"
                            "features/geocoding_api.c"
                            "features/glucose_history.c"
                            "features/update_checker.c"
                            "features/heartbeat.c"
                            "ui/menu_screen.c"
                            "ui/home_screen.c"
                            "ui/wifi_screens.c"
                            "ui/cgm_screens.c"
                            "ui/alarm_screens.c"
                            "ui/time_weather_screens.c"
                            "ui/glucose_chart.c"
                            "tasks/background_tasks.c"
                            "font_montserrat_120.c"
                       INCLUDE_DIRS "." "hardware" "features" "ui" "tasks")

# Accept SD cards that refuse CMD59. ESP-IDF makes enabling SPI CRC a fatal
# init step, but the SD spec makes CRC16 optional in SPI mode, so cards that
# decline it are in spec and work everywhere that never asks. There is no
# Kconfig to relax it in v5.5.2, so the step is redirected rather than the
# managed component patched, which a toolchain update would undo.
# The wrapper is in hardware/sd_crc_compat.c.
# -u forces the wrapper object out of libmain.a. It defines only the wrap
# symbol, so nothing in our own code references it and the linker would
# otherwise never extract it, leaving sdmmc_init.c pointing at nothing.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=sdmmc_init_spi_crc"
                      "-Wl,-u,__wrap_sdmmc_init_spi_crc")
