/**
 * @file ml_cellular.h
 * @brief TailTiny Cellular Module — SIM7600 4G Modem Driver
 *
 * Provides AT command interface to SIM7600G-H cellular modem over UART,
 * plus PPP data connection that creates an esp_netif PPP interface for lwIP.
 *
 * Hardware: Seeed Studio XIAO ESP32S3 + Waveshare SIM7600X 4G Module
 * Wiring:
 *   SIM7600 TXD  →  XIAO D7 (GPIO44 / RX)
 *   SIM7600 RXD  →  XIAO D6 (GPIO43 / TX)
 *   GND connected, separate USB-C power supplies
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ========================================================================== */

typedef struct {
    int tx_pin;                 /* ESP32 TX → SIM7600 RXD (default: GPIO43) */
    int rx_pin;                 /* ESP32 RX ← SIM7600 TXD (default: GPIO44) */
    int baud_rate;              /* UART baud rate (default: 115200) */
    const char *apn;            /* APN for data connection (NULL = auto) */
    const char *sim_pin;        /* SIM PIN code (NULL = no PIN required) */
    const char *ppp_user;       /* PPP CHAP username (NULL = empty) */
    const char *ppp_pass;       /* PPP CHAP password (NULL = empty) */
} ml_cellular_config_t;

/* Default pin configuration for XIAO ESP32S3 + Waveshare SIM7600X */
#define ML_CELLULAR_DEFAULT_CONFIG() { \
    .tx_pin = 43,               \
    .rx_pin = 44,               \
    .baud_rate = 115200,        \
    .apn = NULL,                \
    .sim_pin = NULL,            \
    .ppp_user = NULL,           \
    .ppp_pass = NULL,           \
}

/* ============================================================================
 * Cellular state
 * ========================================================================== */

typedef enum {
    ML_CELL_STATE_OFF,          /* Not initialized */
    ML_CELL_STATE_INIT,         /* UART initialized, AT communication pending */
    ML_CELL_STATE_AT_OK,        /* AT communication established */
    ML_CELL_STATE_SIM_READY,    /* SIM card ready */
    ML_CELL_STATE_REGISTERED,   /* Registered on network */
    ML_CELL_STATE_PPP_CONNECTING, /* PPP dial in progress */
    ML_CELL_STATE_PPP_CONNECTED,  /* PPP connected, IP assigned */
    ML_CELL_STATE_DATA_CONNECTED, /* AT socket bridge connected (modem internal TCP/IP) */
    ML_CELL_STATE_ERROR,        /* Error state */
} ml_cellular_state_t;

typedef enum {
    ML_DATA_MODE_NONE,          /* No data connection */
    ML_DATA_MODE_PPP,           /* PPP — standard lwIP sockets */
    ML_DATA_MODE_AT_SOCKET,     /* AT socket bridge — modem internal TCP/IP */
} ml_cellular_data_mode_t;

/* ============================================================================
 * Module info
 * ========================================================================== */

typedef struct {
    char imei[20];              /* IMEI string (15 digits + null) */
    char iccid[24];             /* SIM ICCID */
    char model[32];             /* Module model (e.g. "SIM7600G-H") */
    char firmware[64];          /* Firmware version */
    char operator_name[32];     /* Registered operator */
    int rssi;                   /* Signal strength (0-31, 99=unknown) */
    int rssi_dbm;               /* Signal in dBm */
    bool sim_ready;             /* SIM card is ready */
    bool registered;            /* Registered on network */
    bool data_connected;        /* PPP data connection active */
} ml_cellular_info_t;

/* ============================================================================
 * Public API
 * ========================================================================== */

/**
 * Initialize the SIM7600 cellular modem.
 * Sets up UART, sends initial AT commands, checks SIM status.
 * Does NOT start PPP data connection.
 *
 * @param config  Cellular configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t ml_cellular_init(const ml_cellular_config_t *config);

/**
 * Deinitialize cellular module. Closes PPP if active, releases UART.
 */
void ml_cellular_deinit(void);

/**
 * Start the PPP data connection.
 * Sends AT commands to configure PDP context and enters PPP mode.
 * Creates an esp_netif PPP interface that becomes available to lwIP.
 *
 * This is a blocking call that waits for IP assignment (up to 30s).
 *
 * @return ESP_OK if PPP connected and IP assigned
 */
esp_err_t ml_cellular_ppp_start(void);

/**
 * Stop the PPP data connection and return to AT command mode.
 */
esp_err_t ml_cellular_ppp_stop(void);

/**
 * Start cellular data using AT socket bridge (modem internal TCP/IP).
 * Uses AT+NETOPEN instead of PPP — bypasses CHAP authentication issues.
 * Initializes the AT socket bridge (ml_at_socket_init) which provides
 * BSD socket API wrappers over AT commands.
 *
 * @return ESP_OK if modem internal TCP/IP is active and has IP
 */
esp_err_t ml_cellular_data_start(void);

/**
 * Stop the AT socket bridge data connection.
 * Sends AT+NETCLOSE and deinitializes the AT socket bridge.
 */
esp_err_t ml_cellular_data_stop(void);

/**
 * Connect cellular data with automatic PPP→AT fallback.
 *
 * Attempts PPP connection first (30s timeout). If PPP fails (dial error,
 * CHAP auth failure, or IP assignment timeout), automatically falls back
 * to AT socket bridge (modem internal TCP/IP).
 *
 * PPP gives standard lwIP sockets (TCP+UDP, ~300ms latency).
 * AT socket bridge gives modem-managed TCP only (~3s latency).
 *
 * @return ESP_OK if connected via either PPP or AT
 */
esp_err_t ml_cellular_connect(void);

/**
 * Get the active data transport mode.
 * @return ML_DATA_MODE_PPP, ML_DATA_MODE_AT_SOCKET, or ML_DATA_MODE_NONE
 */
ml_cellular_data_mode_t ml_cellular_get_data_mode(void);

/**
 * Check if cellular is using AT socket bridge mode (not PPP).
 * When true, all TailTiny socket calls route through AT commands.
 */
bool ml_cellular_is_data_mode(void);

/**
 * Get current cellular state.
 */
ml_cellular_state_t ml_cellular_get_state(void);

/**
 * Get cellular module info (IMEI, signal, operator, etc).
 * @param info  Output structure (must not be NULL)
 * @return ESP_OK on success
 */
esp_err_t ml_cellular_get_info(ml_cellular_info_t *info);

/**
 * Get the IMEI string. Returns NULL if not available.
 * The returned pointer is valid until ml_cellular_deinit().
 */
const char *ml_cellular_get_imei(void);

/**
 * Send a raw AT command and get response.
 * Only works when NOT in PPP mode.
 *
 * @param cmd        AT command string (without \r\n)
 * @param response   Buffer for response
 * @param resp_size  Size of response buffer
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received, or -1 on timeout
 */
int ml_cellular_send_at(const char *cmd, char *response, size_t resp_size, int timeout_ms);

#ifdef __cplusplus
}
#endif
