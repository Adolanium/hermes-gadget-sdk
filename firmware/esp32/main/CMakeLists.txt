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
        "port_storage.cpp"
        "port_system.cpp"
        "port_touch.cpp"
        "port_wifi.cpp"
        "port_ws.cpp"
    INCLUDE_DIRS "."
    REQUIRES core
    # "driver" pulls in GPIO, I2S, LEDC and SPI on every ESP-IDF 5.x release.
    PRIV_REQUIRES
        driver
        esp_app_format
        esp_event
        esp_lcd
        esp_netif
        esp_timer
        esp_wifi
        console
        nvs_flash
        mbedtls
        esp_websocket_client
)
