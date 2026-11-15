# TailTiny, vendored: ../TAILTINY.md has the pinned release and our patches.
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
    "src/ml_cellular.c"
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
    driver
    esp_driver_gpio
    esp_driver_uart
    esp_driver_tsens
    esp_http_server
    vfs  # eventfd that wakes the DERP task when TX/control work is queued
)

# ESP-IDF 6 no longer ships cJSON as the `json` component; it comes from the
# component registry instead (idf_component.yml).
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND REQUIRES espressif__cjson)
else()
    list(APPEND REQUIRES json)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)
