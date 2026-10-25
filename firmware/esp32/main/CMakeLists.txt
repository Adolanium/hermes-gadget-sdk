if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/wifi_setup.html" HG_WIFI_SETUP_HTML)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/wifi_setup.html")
    configure_file(wifi_setup_page.hpp.in wifi_setup_page.hpp @ONLY)
endif()

set(HG_P4_SOURCES)
set(HG_P4_REQUIRES)
set(HG_RGB_REQUIRES)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND HG_P4_SOURCES
        "port_dsi.cpp"
        "port_audio_p4.cpp"
        "port_power_p4.cpp"
        "../../drivers/stc8_battery.cpp")
    list(APPEND HG_P4_REQUIRES esp_lcd_ek79007)
endif()
# The RGB panel driver (ST7701 over an I/O expander) only builds for the chips
# with an LCD_CAM RGB scanout: port_rgb.cpp is compiled out on every other
# target, and esp_lcd_st7701 does not publish builds for them either.
if(IDF_TARGET STREQUAL "esp32s3" OR IDF_TARGET STREQUAL "esp32p4")
    list(APPEND HG_RGB_REQUIRES esp_lcd_st7701 esp_lcd_panel_io_additions)
endif()
idf_component_register(
    SRCS
        "main.cpp"
        "board.cpp"
        "port_amoled.cpp"
        "port_audio.cpp"
        ${HG_P4_SOURCES}
        "port_ble.cpp"
        "port_buttons.cpp"
        "port_codec.cpp"
        "port_cores3.cpp"
        "port_tca9554.cpp"
        "port_console.cpp"
        "port_dac.cpp"
        "port_diag.cpp"
        "port_display.cpp"
        "port_events.cpp"
        "port_motion.cpp"
        "port_ota.cpp"
        "port_parallel.cpp"
        "port_rgb.cpp"
        "port_power.cpp"
        "port_latch_power.cpp"
        "port_opus.cpp"
        "../../drivers/axp2101.cpp"
        "../../drivers/cores3.cpp"
        "../../drivers/pcf85063.cpp"
        "../../drivers/qmi8658.cpp"
        "port_storage.cpp"
        "port_system.cpp"
        "port_time.cpp"
        "port_touch.cpp"
        "port_wifi.cpp"
        "port_wifi_setup.cpp"
        "port_ws.cpp"
    INCLUDE_DIRS "." "../../drivers" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES core
    # "driver" pulls in GPIO, I2S, LEDC and SPI on every ESP-IDF 5.x release.
    # ESP-IDF 6.x dropped I2S, LEDC and the DAC from that catch-all, so name them
    # too; all three components exist in 5.x as well, where this is a no-op.
    PRIV_REQUIRES
        app_update
        bt
        driver
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_dac
        esp_app_format
        esp_adc
        esp_event
        esp_lcd
        ${HG_RGB_REQUIRES}
        ${HG_P4_REQUIRES}
        esp_netif
        esp_http_server
        esp_psram
        esp_timer
        esp_wifi
        console
        nvs_flash
        mbedtls
        esp_websocket_client
)
