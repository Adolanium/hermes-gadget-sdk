# TailTiny, vendored: ../TAILTINY.md has the pinned commit and our patches.
# Compiled only when CONFIG_HG_TAILNET is on. The requirements stay unconditional:
# ESP-IDF resolves them before the sdkconfig exists.
set(SRCS)
if(CONFIG_HG_TAILNET)
list(APPEND SRCS
    "src/tailtiny.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/x25519.c"
    "src/ml_zerocopy.c"
    # src/ml_cellular.c: the 4G modem driver, left out (CONFIG_ML_ENABLE_CELLULAR stays off,
    # and the file includes driver/uart.h outside its #ifdef).
    "src/ml_at_socket.c"
    "src/ml_net_switch.c"
    "src/ml_config_httpd.c"
    "src/ml_json_strip.c"
)
endif()

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    espressif__cjson
    driver
    # ESP-IDF 6 dropped UART from `driver`; ml_at_socket.c includes driver/uart.h.
    esp_driver_uart
    esp_driver_tsens
    esp_http_server
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)

if(CONFIG_HG_TAILNET)
    # GCC 15: protocol constants (HSalsa20's sigma) are byte arrays initialised from
    # strings of exactly their length, deliberately without the NUL.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unterminated-string-initialization)
endif()
