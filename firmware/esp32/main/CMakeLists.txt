idf_component_register(
    SRCS
        "main.cpp"
        "board.cpp"
        "port_amoled.cpp"
        "port_audio.cpp"
        "port_buttons.cpp"
        "port_codec.cpp"
        "port_console.cpp"
        "port_diag.cpp"
        "port_display.cpp"
        "port_events.cpp"
        "port_ota.cpp"
        "port_power.cpp"
        "port_latch_power.cpp"
        "../../drivers/axp2101.cpp"
        "port_storage.cpp"
        "port_system.cpp"
        "port_touch.cpp"
        "port_wifi.cpp"
        "port_ws.cpp"
    INCLUDE_DIRS "." "../../drivers"
    REQUIRES core
    # "driver" pulls in GPIO, I2S, LEDC and SPI on every ESP-IDF 5.x release.
    # ESP-IDF 6.x dropped I2S and LEDC from that catch-all, so name them too;
    # both components exist in 5.x as well, where this is a no-op.
    PRIV_REQUIRES
        app_update
        driver
        esp_driver_i2s
        esp_driver_ledc
        esp_app_format
        esp_adc
        esp_event
        esp_lcd
        esp_netif
        esp_psram
        esp_timer
        esp_wifi
        console
        nvs_flash
        mbedtls
        esp_websocket_client
)
