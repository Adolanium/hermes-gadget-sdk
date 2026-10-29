/**
 * @file ml_at_socket.c
 * @brief AT Socket Bridge — BSD socket API over SIM7600 internal TCP/IP stack
 *
 * Maps standard BSD socket calls to SIM7600 AT commands:
 *   socket()     → allocate link_num (0-9)
 *   connect()    → AT+CIPOPEN=N,"TCP",IP,port
 *   send()       → AT+CIPSEND=N,len then data
 *   recv()       → AT+CIPRXGET=2,N,len (active polling)
 *   sendto()     → AT+CIPSEND=N,len,"IP",port
 *   recvfrom()   → AT+CIPRXGET=2,N,len
 *   close()      → AT+CIPCLOSE=N
 *   getaddrinfo()→ AT+CDNSGIP="hostname"
 *   select()     → AT+CIPRXGET=4,N (query pending bytes)
 *
 * Architecture: Single-reader model. Only at_cmd() reads from UART.
 * No background URC handler task — URCs are processed inline when they
 * appear in AT command responses, and recv/select actively poll the modem.
 *
 * The UART is shared with ml_cellular.c (AT command mode, no PPP).
 * A mutex protects AT command sequences from concurrent access.
 */

#include "ml_at_socket.h"
#include "ml_cellular.h"
#include "tailtiny_internal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>    /* sockaddr_in6, INET6_ADDRSTRLEN */

#ifdef CONFIG_ML_ENABLE_CELLULAR

static const char *TAG = "ml_at_sock";

/* ============================================================================
 * Constants
 * ========================================================================== */

#define AT_SOCK_MAX         ML_AT_SOCK_MAX  /* SIM7600 supports link_num 0-9 */
#define AT_SOCK_FD_BASE     ML_AT_SOCK_FD_BASE  /* Virtual FDs: 32-41 (within FD_SETSIZE=64) */
#define AT_MAX_SEND_CHUNK   1460    /* Max bytes per AT+CIPSEND */
#define AT_RESP_BUF_SIZE    512     /* AT response buffer */
#define AT_DATA_BUF_SIZE    4096    /* Data response buffer — room for 1460 data + headers */
#define UART_NUM            UART_NUM_1
#define AT_TIMEOUT_MS       5000
#define AT_LONG_TIMEOUT_MS  15000
#define DNS_TIMEOUT_MS      20000   /* DNS can be slow over cellular */
#define RX_RING_SIZE        8192    /* Per-socket RX ring buffer — 8KB for throughput */

/* ============================================================================
 * Internal Socket State
 * ========================================================================== */

typedef struct {
    int link_num;               /* SIM7600 socket ID (0-9), -1 = free */
    int type;                   /* SOCK_STREAM or SOCK_DGRAM */
    bool connected;
    bool bound;
    bool nonblocking;
    bool closed_by_remote;      /* Remote side closed the connection */
    uint32_t recv_timeout_ms;
    uint32_t send_timeout_ms;
    uint16_t local_port;        /* For UDP bind */

    /* RX ring buffer — filled by active polling via AT+CIPRXGET=2 */
    uint8_t *rx_buf;
    volatile size_t rx_head;    /* Write position */
    volatile size_t rx_tail;    /* Read position */
    size_t rx_size;             /* Ring buffer capacity */

    /* Last UDP source (from +RECV FROM URC) */
    uint32_t last_src_ip;
    uint16_t last_src_port;

    /* Async connect result — set by process_urcs() */
    volatile bool connect_done;
    volatile bool connect_ok;
} ml_at_sock_t;

static struct {
    bool initialized;
    bool net_open;
    ml_at_sock_t socks[AT_SOCK_MAX];
    SemaphoreHandle_t at_mutex;     /* Protects AT command sequences */
} s_at = {0};

/* ============================================================================
 * Ring Buffer Helpers
 * ========================================================================== */

static inline size_t ring_used(const ml_at_sock_t *s) {
    return (s->rx_head - s->rx_tail) % s->rx_size;
}

static inline size_t ring_free(const ml_at_sock_t *s) {
    return s->rx_size - 1 - ring_used(s);
}

static size_t ring_write(ml_at_sock_t *s, const uint8_t *data, size_t len) {
    size_t written = 0;
    while (written < len && ring_free(s) > 0) {
        s->rx_buf[s->rx_head] = data[written++];
        s->rx_head = (s->rx_head + 1) % s->rx_size;
    }
    return written;
}

static size_t ring_read(ml_at_sock_t *s, uint8_t *data, size_t len) {
    size_t read_count = 0;
    while (read_count < len && ring_used(s) > 0) {
        data[read_count++] = s->rx_buf[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % s->rx_size;
    }
    return read_count;
}

/* ============================================================================
 * URC Processing (inline — called from at_cmd response parsing)
 *
 * When at_cmd() reads a response, it may contain interleaved URCs.
 * This function scans the response buffer for URC lines and dispatches them.
 * ========================================================================== */

static void process_urc_line(const char *line)
{
    /* +IPCLOSE: <link_num>,<reason> — remote closed */
    if (strncmp(line, "+IPCLOSE:", 9) == 0) {
        int link_num = atoi(line + 9);
        if (link_num >= 0 && link_num < AT_SOCK_MAX) {
            ESP_LOGI(TAG, "URC: remote closed link %d", link_num);
            s_at.socks[link_num].closed_by_remote = true;
        }
        return;
    }

    /* +CIPOPEN: <link_num>,<err> — async connect result */
    if (strncmp(line, "+CIPOPEN:", 9) == 0) {
        int link_num, err;
        if (sscanf(line + 9, " %d,%d", &link_num, &err) == 2) {
            if (link_num >= 0 && link_num < AT_SOCK_MAX) {
                s_at.socks[link_num].connect_done = true;
                s_at.socks[link_num].connect_ok = (err == 0);
                if (err == 0) {
                    s_at.socks[link_num].connected = true;
                    ESP_LOGI(TAG, "URC: link %d connected", link_num);
                } else {
                    ESP_LOGW(TAG, "URC: link %d connect failed err=%d", link_num, err);
                }
            }
        }
        return;
    }

    /* +CIPRXGET: 1,<link_num> — data notification (we ignore; we poll actively) */
    if (strncmp(line, "+CIPRXGET: 1,", 13) == 0) {
        int link_num = atoi(line + 13);
        ESP_LOGD(TAG, "URC: data notification on link %d (ignored, will poll)", link_num);
        return;
    }
}

/* Scan response buffer for URC lines and process them.
 * This is called after at_cmd() finishes reading a response.
 * URCs can be interleaved with the expected response. */
static void process_urcs_in_response(const char *resp)
{
    const char *p = resp;
    while (p && *p) {
        /* Skip whitespace/newlines */
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;

        /* Find end of line */
        const char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);

        /* Extract line (without trailing \r\n) */
        int line_len = (int)(eol - p);
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        if (line_len > 0 && line_len < 256) {
            char line[256];
            memcpy(line, p, line_len);
            line[line_len] = '\0';

            /* Check if it's a URC */
            if (line[0] == '+') {
                process_urc_line(line);
            }
        }

        p = (*eol) ? eol + 1 : eol;
    }
}

/* ============================================================================
 * AT Command Helpers (single-reader model — sole UART reader)
 * ========================================================================== */

/* Binary-safe search: find needle in haystack of given length.
 * Like memmem() but portable. */
static void *bin_search(const void *haystack, size_t haystack_len,
                         const char *needle, size_t needle_len)
{
    if (needle_len > haystack_len) return NULL;
    const uint8_t *h = (const uint8_t *)haystack;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(h + i, needle, needle_len) == 0) {
            return (void *)(h + i);
        }
    }
    return NULL;
}

/* Send an AT command and wait for response.
 * Caller must hold at_mutex.
 * Processes any URCs found in the response.
 * Uses binary-safe search (memmem) so null bytes in data don't hang. */
static int at_cmd(const char *cmd, char *resp, size_t resp_size, int timeout_ms)
{
    uart_flush_input(UART_NUM);

    char cmd_buf[512];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmd);
    uart_write_bytes(UART_NUM, cmd_buf, strlen(cmd_buf));

    ESP_LOGD(TAG, ">> %s", cmd);

    int total = 0;
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    memset(resp, 0, resp_size);

    while ((esp_timer_get_time() - start) < timeout_us) {
        /* Short UART read timeout (20ms) for fast AT round-trips.
         * At 921600 baud, 1460 bytes take ~16ms to transfer. */
        int len = uart_read_bytes(UART_NUM,
                                   (uint8_t *)(resp + total),
                                   resp_size - total - 1,
                                   pdMS_TO_TICKS(20));
        if (len > 0) {
            total += len;
            resp[total] = '\0';
            /* Binary-safe: scan full buffer including past null bytes */
            if (bin_search(resp, total, "\r\nOK\r\n", 6) ||
                bin_search(resp, total, "\nOK\r\n", 5) ||
                bin_search(resp, total, "\r\nERROR\r\n", 9) ||
                bin_search(resp, total, "\r\n+CME ERROR", 12) ||
                bin_search(resp, total, "\r\n+CMS ERROR", 12)) {
                break;
            }
            /* Also check if buffer starts with OK (no preceding \r\n) */
            if (total >= 4 && memcmp(resp, "OK\r\n", 4) == 0) {
                break;
            }
        }
    }

    if (total > 0) {
        ESP_LOGD(TAG, "<< [%d bytes]", total);
        /* Process any URCs that appeared in the response */
        process_urcs_in_response(resp);
    }
    return total;
}

static bool at_cmd_ok(const char *cmd, int timeout_ms)
{
    char resp[AT_RESP_BUF_SIZE];
    int len = at_cmd(cmd, resp, sizeof(resp), timeout_ms);
    return (len > 0 && strstr(resp, "OK") != NULL);
}

/* Send an AT command and wait for a specific URC pattern (e.g. +CDNSGIP).
 * After getting OK, continues reading for the async URC response.
 * Caller must hold at_mutex. */
static int at_cmd_wait_urc(const char *cmd, const char *urc_prefix,
                            char *resp, size_t resp_size, int timeout_ms)
{
    uart_flush_input(UART_NUM);

    char cmd_buf[512];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmd);
    uart_write_bytes(UART_NUM, cmd_buf, strlen(cmd_buf));

    ESP_LOGD(TAG, ">> %s (wait for %s)", cmd, urc_prefix);

    int total = 0;
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    memset(resp, 0, resp_size);
    bool got_ok = false;
    bool got_urc = false;

    while ((esp_timer_get_time() - start) < timeout_us) {
        int len = uart_read_bytes(UART_NUM,
                                   (uint8_t *)(resp + total),
                                   resp_size - total - 1,
                                   pdMS_TO_TICKS(100));
        if (len > 0) {
            total += len;
            resp[total] = '\0';

            if (!got_ok && (strstr(resp, "OK") || strstr(resp, "ERROR"))) {
                got_ok = true;
                /* If ERROR, stop immediately */
                if (strstr(resp, "ERROR")) break;
            }

            /* Check if the URC we're waiting for has arrived */
            if (strstr(resp, urc_prefix)) {
                got_urc = true;
                /* Give a tiny bit more time for trailing data */
                vTaskDelay(pdMS_TO_TICKS(100));
                len = uart_read_bytes(UART_NUM,
                                       (uint8_t *)(resp + total),
                                       resp_size - total - 1,
                                       pdMS_TO_TICKS(200));
                if (len > 0) {
                    total += len;
                    resp[total] = '\0';
                }
                break;
            }
        }
    }

    if (total > 0) {
        ESP_LOGD(TAG, "<< [%d bytes] %s", total, resp);
        process_urcs_in_response(resp);
    }

    if (!got_urc) {
        ESP_LOGW(TAG, "Timed out waiting for %s", urc_prefix);
    }
    return total;
}

/* Send raw data (after AT+CIPSEND prompt).
 * Caller must hold at_mutex. */
static int at_send_raw(const void *data, size_t len, int timeout_ms)
{
    uart_write_bytes(UART_NUM, data, len);

    /* Wait for response (OK or ERROR) */
    char resp[256];
    int total = 0;
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    memset(resp, 0, sizeof(resp));

    while ((esp_timer_get_time() - start) < timeout_us) {
        int n = uart_read_bytes(UART_NUM,
                                 (uint8_t *)(resp + total),
                                 sizeof(resp) - total - 1,
                                 pdMS_TO_TICKS(100));
        if (n > 0) {
            total += n;
            resp[total] = '\0';
            if (strstr(resp, "OK") || strstr(resp, "ERROR")) {
                break;
            }
        }
    }

    return (strstr(resp, "OK") != NULL) ? (int)len : -1;
}

/* Format IP from sockaddr to string (supports both IPv4 and IPv6) */
static void sockaddr_to_str(const struct sockaddr *addr, char *buf, size_t buflen, uint16_t *port)
{
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        inet_ntoa_r(sin->sin_addr, buf, buflen);
        if (port) *port = ntohs(sin->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf, buflen);
        if (port) *port = ntohs(sin6->sin6_port);
    }
}

/* ============================================================================
 * Active Polling for RX Data
 *
 * Instead of relying on a URC handler task, we actively poll the modem
 * for pending data using AT+CIPRXGET commands.
 * ========================================================================== */

/* Query how many bytes are pending for a given link.
 * Uses AT+CIPRXGET=4,N → +CIPRXGET: 4,<link_num>,<pending_len>
 * Caller must hold at_mutex. */
static int at_query_pending(int link_num)
{
    char cmd[32];
    char resp[AT_RESP_BUF_SIZE];

    int64_t t0 = esp_timer_get_time();
    snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=4,%d", link_num);
    int len = at_cmd(cmd, resp, sizeof(resp), AT_TIMEOUT_MS);
    if (len <= 0) return 0;

    /* Parse +CIPRXGET: 4,<link_num>,<pending_len> */
    char *p = strstr(resp, "+CIPRXGET: 4,");
    if (!p) return 0;

    int ln, pending;
    if (sscanf(p, "+CIPRXGET: 4,%d,%d", &ln, &pending) >= 2) {
        int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        if (pending > 0) {
            ESP_LOGI(TAG, "QUERY: link=%d pending=%d %lldms", link_num, pending, elapsed_ms);
        }
        return pending;
    }
    return 0;
}

/* Read one chunk of data from modem into socket's ring buffer.
 * Binary-safe: reads response in two phases:
 *   1. Text header: +CIPRXGET: 2,<link>,<actual>,<remaining>\r\n
 *   2. Binary data: exactly <actual> bytes
 *   3. Trailing: \r\nOK\r\n
 * Caller must hold at_mutex.
 * Returns: actual bytes written to ring, sets *out_remaining if not NULL. */
static int at_read_data_chunk(int link_num, int max_bytes, int *out_remaining)
{
    if (link_num < 0 || link_num >= AT_SOCK_MAX) return 0;
    ml_at_sock_t *s = &s_at.socks[link_num];
    if (s->link_num < 0) return 0;

    int req_len = max_bytes;
    size_t free_space = ring_free(s);
    if ((size_t)req_len > free_space) req_len = (int)free_space;
    if (req_len <= 0) return 0;
    if (req_len > 1460) req_len = 1460;

    /* Send command */
    uart_flush_input(UART_NUM);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CIPRXGET=2,%d,%d\r\n", link_num, req_len);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));

    /* Phase 1: Read text header line.
     * Looking for: +CIPRXGET: 2,<link>,<actual>,<remaining>\r\n
     * The header is pure ASCII text, so strstr is safe here. */
    char hdr[128];
    int hdr_total = 0;
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)AT_TIMEOUT_MS * 1000;
    int actual_len = 0, remaining = 0;
    bool got_header = false;

    while ((esp_timer_get_time() - start) < timeout_us) {
        int n = uart_read_bytes(UART_NUM,
                                 (uint8_t *)(hdr + hdr_total),
                                 sizeof(hdr) - hdr_total - 1,
                                 pdMS_TO_TICKS(20));
        if (n > 0) {
            hdr_total += n;
            hdr[hdr_total] = '\0';

            /* Check for ERROR response */
            if (strstr(hdr, "ERROR")) {
                return 0;
            }

            /* Parse the header — look for the line ending with \n after +CIPRXGET */
            char *p = strstr(hdr, "+CIPRXGET: 2,");
            if (p) {
                char *nl = strchr(p, '\n');
                if (nl) {
                    int ln;
                    if (sscanf(p, "+CIPRXGET: 2,%d,%d,%d", &ln, &actual_len, &remaining) >= 2) {
                        got_header = true;
                        /* Calculate how many bytes of data we already consumed
                         * past the header \n */
                        int hdr_end_offset = (int)(nl + 1 - hdr);
                        int data_already = hdr_total - hdr_end_offset;

                        /* Write any data bytes that came with the header read */
                        if (data_already > 0 && actual_len > 0) {
                            int to_write = (data_already > actual_len) ? actual_len : data_already;
                            ring_write(s, (const uint8_t *)(nl + 1), to_write);
                            actual_len -= to_write;
                        }
                        break;
                    }
                }
            }
        }
    }

    if (!got_header) {
        if (out_remaining) *out_remaining = 0;
        return 0;
    }

    /* Track total bytes written to ring.
     * Phase 1 may have piggybacked some data — actual_len was reduced. */
    int orig_actual = actual_len;
    {
        int ln_dummy, parsed_actual = 0;
        char *p2 = strstr(hdr, "+CIPRXGET: 2,");
        if (p2) sscanf(p2, "+CIPRXGET: 2,%d,%d", &ln_dummy, &parsed_actual);
        orig_actual = parsed_actual; /* original data length from modem */
    }
    int total_written = orig_actual - actual_len; /* Phase 1 piggyback */

    /* Phase 2: Read remaining binary data bytes directly into ring buffer.
     * We know exactly how many bytes to expect — no string parsing needed.
     * Use a large buffer to minimize uart_read_bytes() calls (each call has
     * timeout overhead even when data is available). */
    uint8_t chunk[1460];
    while (actual_len > 0 && (esp_timer_get_time() - start) < timeout_us) {
        int want = (actual_len > (int)sizeof(chunk)) ? (int)sizeof(chunk) : actual_len;
        int n = uart_read_bytes(UART_NUM, chunk, want, pdMS_TO_TICKS(20));
        if (n > 0) {
            size_t w = ring_write(s, chunk, n);
            total_written += (int)w;
            actual_len -= n;
        }
    }

    /* Phase 3: Consume trailing \r\nOK\r\n (don't care about exact content).
     * Short timeout — data is already flowing, just need to clear the tail. */
    uint8_t trail[32];
    uart_read_bytes(UART_NUM, trail, sizeof(trail), pdMS_TO_TICKS(5));

    if (out_remaining) *out_remaining = remaining;

    /* Throughput diagnostic: log every chunk at INFO level to see actual sizes.
     * Key question: are chunks actually 1460 bytes or much smaller? */
    int64_t chunk_elapsed_ms = (esp_timer_get_time() - start) / 1000;
    ESP_LOGI(TAG, "CHUNK: link=%d req=%d got=%d remaining=%d %lldms",
             link_num, req_len, total_written, remaining, chunk_elapsed_ms);

    return total_written;
}

/* Read ALL available data from modem into socket's ring buffer.
 * Keeps reading in a loop while remaining > 0.
 * Caller must hold at_mutex.
 * Returns total bytes read. Sets *out_remaining to bytes still in modem. */
static int at_read_data(int link_num, int max_bytes, int *out_remaining)
{
    int total = 0;
    int remaining = 0;

    /* Read first chunk */
    int got = at_read_data_chunk(link_num, max_bytes, &remaining);
    total += got;

    /* Keep reading while there's more data and ring buffer has space */
    while (remaining > 0 && got > 0 && ring_free(&s_at.socks[link_num]) > 0) {
        got = at_read_data_chunk(link_num, remaining, &remaining);
        total += got;
    }

    if (out_remaining) *out_remaining = remaining;
    return total;
}

/* ============================================================================
 * Initialization
 * ========================================================================== */

esp_err_t ml_at_socket_init(void)
{
    if (s_at.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Initialize socket array */
    for (int i = 0; i < AT_SOCK_MAX; i++) {
        s_at.socks[i].link_num = -1; /* Free */
    }

    /* Create AT mutex */
    s_at.at_mutex = xSemaphoreCreateMutex();
    if (!s_at.at_mutex) {
        ESP_LOGE(TAG, "Failed to create AT mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Close any existing network session */
    xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
    at_cmd_ok("AT+NETCLOSE", AT_TIMEOUT_MS);
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Set manual RX mode (we poll with AT+CIPRXGET=2) */
    at_cmd_ok("AT+CIPRXGET=1", AT_TIMEOUT_MS);

    /* Open network */
    char resp[AT_RESP_BUF_SIZE];
    at_cmd("AT+NETOPEN", resp, sizeof(resp), AT_LONG_TIMEOUT_MS);
    xSemaphoreGive(s_at.at_mutex);

    if (!strstr(resp, "OK") && !strstr(resp, "+NETOPEN: 0") && !strstr(resp, "already opened")) {
        ESP_LOGE(TAG, "AT+NETOPEN failed: %s", resp);
        return ESP_FAIL;
    }

    /* Wait for network to be ready */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Verify IP address */
    xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
    at_cmd("AT+IPADDR", resp, sizeof(resp), AT_TIMEOUT_MS);
    xSemaphoreGive(s_at.at_mutex);

    char *ip_start = strstr(resp, "+IPADDR:");
    if (ip_start) {
        ESP_LOGI(TAG, "Modem IP: %s", ip_start);
    } else {
        ESP_LOGW(TAG, "Could not get modem IP address");
    }

    s_at.net_open = true;
    s_at.initialized = true;
    ESP_LOGI(TAG, "AT socket bridge initialized (single-reader mode)");
    return ESP_OK;
}

void ml_at_socket_deinit(void)
{
    if (!s_at.initialized) return;

    /* Close all sockets */
    for (int i = 0; i < AT_SOCK_MAX; i++) {
        if (s_at.socks[i].link_num >= 0) {
            ml_at_close(AT_SOCK_FD_BASE + i);
        }
    }

    /* Close network */
    if (s_at.net_open) {
        xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
        at_cmd_ok("AT+NETCLOSE", AT_TIMEOUT_MS);
        xSemaphoreGive(s_at.at_mutex);
        s_at.net_open = false;
    }

    if (s_at.at_mutex) {
        vSemaphoreDelete(s_at.at_mutex);
        s_at.at_mutex = NULL;
    }

    s_at.initialized = false;
    ESP_LOGI(TAG, "AT socket bridge deinitialized");
}

bool ml_at_socket_is_ready(void)
{
    return s_at.initialized && s_at.net_open;
}

/* ============================================================================
 * Socket API
 * ========================================================================== */

bool ml_at_socket_is_at_fd(int fd)
{
    return fd >= AT_SOCK_FD_BASE && fd < AT_SOCK_FD_BASE + AT_SOCK_MAX;
}

static ml_at_sock_t *fd_to_sock(int fd)
{
    if (!ml_at_socket_is_at_fd(fd)) return NULL;
    int idx = fd - AT_SOCK_FD_BASE;
    ml_at_sock_t *s = &s_at.socks[idx];
    return (s->link_num >= 0) ? s : NULL;
}

int ml_at_socket(int domain, int type, int protocol)
{
    if (!s_at.initialized) {
        errno = ENETDOWN;
        return -1;
    }

    /* Accept AF_INET and AF_INET6 — the modem handles IPv6 transparently
     * via AT+CIPOPEN with IPv6 address strings. The domain is stored but
     * the AT bridge doesn't differentiate at the link level. */
    if (domain != AF_INET && domain != AF_INET6) {
        ESP_LOGW(TAG, "Unsupported domain %d (need AF_INET or AF_INET6)", domain);
        errno = EAFNOSUPPORT;
        return -1;
    }

    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        errno = EPROTOTYPE;
        return -1;
    }

    /* Find free slot */
    for (int i = 0; i < AT_SOCK_MAX; i++) {
        if (s_at.socks[i].link_num < 0) {
            ml_at_sock_t *s = &s_at.socks[i];
            memset(s, 0, sizeof(*s));
            s->link_num = i;
            s->type = type;
            s->recv_timeout_ms = 10000; /* Default 10s */
            s->send_timeout_ms = 10000;

            /* Allocate RX ring buffer */
            s->rx_size = RX_RING_SIZE;
            s->rx_buf = ml_psram_malloc(RX_RING_SIZE);
            if (!s->rx_buf) {
                s->rx_buf = malloc(RX_RING_SIZE);
            }
            if (!s->rx_buf) {
                s->link_num = -1;
                errno = ENOMEM;
                return -1;
            }

            int fd = AT_SOCK_FD_BASE + i;
            ESP_LOGI(TAG, "socket(%s) -> fd=%d link=%d",
                     type == SOCK_STREAM ? "TCP" : "UDP", fd, i);
            return fd;
        }
    }

    ESP_LOGE(TAG, "No free AT sockets (max %d)", AT_SOCK_MAX);
    errno = EMFILE;
    return -1;
}

int ml_at_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    char ip_str[INET6_ADDRSTRLEN];  /* 46 bytes — fits IPv4 and IPv6 */
    uint16_t port;
    sockaddr_to_str(addr, ip_str, sizeof(ip_str), &port);

    const char *proto = (s->type == SOCK_STREAM) ? "TCP" : "UDP";
    char cmd[192];  /* Larger for IPv6 addresses */

    if (s->type == SOCK_DGRAM && s->local_port > 0) {
        /* UDP with local port binding */
        snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=%d,\"%s\",\"%s\",%u,%u",
                 s->link_num, proto, ip_str, port, s->local_port);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=%d,\"%s\",\"%s\",%u",
                 s->link_num, proto, ip_str, port);
    }

    ESP_LOGI(TAG, "connect: %s", cmd);

    s->connect_done = false;
    s->connect_ok = false;

    /* For TCP, we need to wait for the async +CIPOPEN: N,0 URC.
     * Use at_cmd_wait_urc to keep reading until we see it. */
    char resp[AT_RESP_BUF_SIZE];
    xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);

    if (s->type == SOCK_STREAM) {
        /* TCP: wait for +CIPOPEN URC */
        at_cmd_wait_urc(cmd, "+CIPOPEN:", resp, sizeof(resp), AT_LONG_TIMEOUT_MS);
    } else {
        /* UDP: just need OK */
        at_cmd(cmd, resp, sizeof(resp), AT_LONG_TIMEOUT_MS);
    }
    xSemaphoreGive(s_at.at_mutex);

    /* Check for inline connect result */
    if (strstr(resp, "+CIPOPEN:") && strstr(resp, ",0")) {
        s->connected = true;
        ESP_LOGI(TAG, "connect: link %d connected", s->link_num);
        return 0;
    }

    /* Check async result (processed by process_urcs_in_response) */
    if (s->connect_done && s->connect_ok) {
        ESP_LOGI(TAG, "connect: link %d connected (URC)", s->link_num);
        return 0;
    }

    if (s->type == SOCK_DGRAM) {
        /* UDP "connect" just stores the remote for sendto default */
        if (strstr(resp, "OK")) {
            s->connected = true;
            return 0;
        }
    }

    ESP_LOGW(TAG, "connect: link %d failed: %s", s->link_num, resp);
    errno = ECONNREFUSED;
    return -1;
}

ssize_t ml_at_send(int fd, const void *buf, size_t len, int flags)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }
    if (!s->connected) { errno = ENOTCONN; return -1; }

    const uint8_t *data = (const uint8_t *)buf;
    size_t total_sent = 0;

    while (total_sent < len) {
        size_t chunk = len - total_sent;
        if (chunk > AT_MAX_SEND_CHUNK) chunk = AT_MAX_SEND_CHUNK;

        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d", s->link_num, (int)chunk);

        xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);

        /* Send the command and wait for '>' prompt */
        char resp[256];
        uart_flush_input(UART_NUM);
        char cmd_buf[128];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmd);
        uart_write_bytes(UART_NUM, cmd_buf, strlen(cmd_buf));

        /* Wait for '>' prompt */
        int resp_len = 0;
        int64_t start = esp_timer_get_time();
        memset(resp, 0, sizeof(resp));
        bool got_prompt = false;
        while ((esp_timer_get_time() - start) < (int64_t)AT_TIMEOUT_MS * 1000) {
            int n = uart_read_bytes(UART_NUM,
                                     (uint8_t *)(resp + resp_len),
                                     sizeof(resp) - resp_len - 1,
                                     pdMS_TO_TICKS(50));
            if (n > 0) {
                resp_len += n;
                resp[resp_len] = '\0';
                if (strchr(resp, '>')) {
                    got_prompt = true;
                    break;
                }
                if (strstr(resp, "ERROR")) break;
            }
        }

        if (!got_prompt) {
            xSemaphoreGive(s_at.at_mutex);
            ESP_LOGW(TAG, "send: no '>' prompt for link %d", s->link_num);
            if (total_sent > 0) return total_sent;
            errno = EIO;
            return -1;
        }

        /* Send the raw data */
        int sent = at_send_raw(data + total_sent, chunk, s->send_timeout_ms);
        xSemaphoreGive(s_at.at_mutex);

        if (sent < 0) {
            ESP_LOGW(TAG, "send: data send failed for link %d", s->link_num);
            if (total_sent > 0) return total_sent;
            errno = EIO;
            return -1;
        }

        total_sent += chunk;
    }

    return total_sent;
}

ssize_t ml_at_recv(int fd, void *buf, size_t len, int flags)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    /* Check ring buffer first — fast path, no AT commands */
    size_t avail = ring_used(s);
    if (avail > 0) {
        size_t to_read = (avail < len) ? avail : len;
        return ring_read(s, (uint8_t *)buf, to_read);
    }

    /* Check if remote closed */
    if (s->closed_by_remote) {
        return 0; /* EOF */
    }

    /* Non-blocking mode */
    if (s->nonblocking) {
        xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
        int pending = at_query_pending(s->link_num);
        if (pending > 0) {
            at_read_data(s->link_num, pending, NULL);
        }
        xSemaphoreGive(s_at.at_mutex);

        avail = ring_used(s);
        if (avail > 0) {
            size_t to_read = (avail < len) ? avail : len;
            return ring_read(s, (uint8_t *)buf, to_read);
        }
        errno = EAGAIN;
        return -1;
    }

    /* Blocking mode with drain optimization:
     * - Query modem immediately (no initial sleep)
     * - When data found: drain ALL pending data before returning
     * - When no data: backoff with short intervals
     * - Return as soon as we have ANY data in the ring buffer */
    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)s->recv_timeout_ms * 1000;
    bool first_poll = true;  /* No delay before first poll */

    do {
        if (!first_poll) {
            /* Short fixed delay between polls — 10ms balances throughput
             * with CPU usage. Lower than 10ms starves other tasks. */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        first_poll = false;

        /* Drain mode: query once, then keep reading via at_read_data()'s
         * internal remaining tracking. Only re-query if at_read_data()
         * reports remaining > 0 but couldn't read (ring full). */
        xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
        int total_drained = 0;
        int remaining = 0;
        int pending = at_query_pending(s->link_num);
        if (pending > 0) {
            /* at_read_data drains ALL pending data in a loop internally */
            int got = at_read_data(s->link_num, pending, &remaining);
            total_drained += got;

            /* If modem still has data (ring was full), try again after
             * reading from ring — but this is rare with 8KB ring */
        }
        xSemaphoreGive(s_at.at_mutex);

        if (total_drained > 0) {
            ESP_LOGI(TAG, "DRAIN: link=%d drained=%d remaining=%d", s->link_num, total_drained, remaining);
        }

        /* Return data if available */
        avail = ring_used(s);
        if (avail > 0) {
            size_t to_read = (avail < len) ? avail : len;
            return ring_read(s, (uint8_t *)buf, to_read);
        }

        /* Check remote close */
        if (s->closed_by_remote) {
            return 0;
        }

        /* No data — yield to let lower-priority tasks (DERP connect) run */
        vTaskDelay(1);

    } while ((esp_timer_get_time() - start) < timeout_us);

    /* Timeout */
    errno = EAGAIN;
    return -1;
}

ssize_t ml_at_sendto(int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *dest_addr, socklen_t addrlen)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    if (s->type != SOCK_DGRAM) {
        /* For TCP, just use send */
        return ml_at_send(fd, buf, len, flags);
    }

    char ip_str[INET6_ADDRSTRLEN];  /* 46 bytes — fits IPv4 and IPv6 */
    uint16_t port;
    sockaddr_to_str(dest_addr, ip_str, sizeof(ip_str), &port);

    /* For UDP, if not yet "connected" (CIPOPEN), open it first */
    if (!s->connected) {
        char cmd[192];
        if (s->local_port > 0) {
            snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=%d,\"UDP\",,,%u",
                     s->link_num, s->local_port);
        } else {
            snprintf(cmd, sizeof(cmd), "AT+CIPOPEN=%d,\"UDP\",,,0",
                     s->link_num);
        }

        xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
        bool ok = at_cmd_ok(cmd, AT_LONG_TIMEOUT_MS);
        xSemaphoreGive(s_at.at_mutex);

        if (!ok) {
            ESP_LOGW(TAG, "sendto: CIPOPEN UDP failed for link %d", s->link_num);
            errno = ENETUNREACH;
            return -1;
        }
        s->connected = true;
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    size_t chunk = (len > AT_MAX_SEND_CHUNK) ? AT_MAX_SEND_CHUNK : len;
    char cmd[192];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%d,\"%s\",%u",
             s->link_num, (int)chunk, ip_str, port);

    xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);

    /* Send command and wait for '>' */
    char resp[256];
    uart_flush_input(UART_NUM);
    char cmd_buf[256];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmd);
    uart_write_bytes(UART_NUM, cmd_buf, strlen(cmd_buf));

    int resp_len = 0;
    int64_t start = esp_timer_get_time();
    memset(resp, 0, sizeof(resp));
    bool got_prompt = false;
    while ((esp_timer_get_time() - start) < (int64_t)AT_TIMEOUT_MS * 1000) {
        int n = uart_read_bytes(UART_NUM,
                                 (uint8_t *)(resp + resp_len),
                                 sizeof(resp) - resp_len - 1,
                                 pdMS_TO_TICKS(50));
        if (n > 0) {
            resp_len += n;
            resp[resp_len] = '\0';
            if (strchr(resp, '>')) { got_prompt = true; break; }
            if (strstr(resp, "ERROR")) break;
        }
    }

    if (!got_prompt) {
        xSemaphoreGive(s_at.at_mutex);
        ESP_LOGW(TAG, "sendto: no '>' prompt for link %d", s->link_num);
        errno = EIO;
        return -1;
    }

    int sent = at_send_raw(buf, chunk, s->send_timeout_ms);
    xSemaphoreGive(s_at.at_mutex);

    if (sent < 0) {
        errno = EIO;
        return -1;
    }

    return chunk;
}

ssize_t ml_at_recvfrom(int fd, void *buf, size_t len, int flags,
                        struct sockaddr *src_addr, socklen_t *addrlen)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    ssize_t n = ml_at_recv(fd, buf, len, flags);

    /* Fill in source address from last URC if available */
    if (n > 0 && src_addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = s->last_src_ip;
        sin->sin_port = htons(s->last_src_port);
        *addrlen = sizeof(struct sockaddr_in);
    }

    return n;
}

int ml_at_close(int fd)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    ESP_LOGI(TAG, "close: fd=%d link=%d", fd, s->link_num);

    /* Send AT+CIPCLOSE */
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", s->link_num);

    xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
    at_cmd_ok(cmd, AT_TIMEOUT_MS);
    xSemaphoreGive(s_at.at_mutex);

    /* Free resources */
    if (s->rx_buf) {
        free(s->rx_buf);
        s->rx_buf = NULL;
    }

    s->link_num = -1;
    s->connected = false;
    s->closed_by_remote = false;

    return 0;
}

int ml_at_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
        s->local_port = ntohs(sin->sin_port);
        s->bound = true;
        ESP_LOGD(TAG, "bind: fd=%d port=%u", fd, s->local_port);
    }

    return 0;
}

int ml_at_setsockopt(int fd, int level, int optname,
                      const void *optval, socklen_t optlen)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO && optlen >= sizeof(struct timeval)) {
            const struct timeval *tv = (const struct timeval *)optval;
            s->recv_timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
            if (s->recv_timeout_ms == 0) s->recv_timeout_ms = 1; /* Avoid 0 = forever */
            return 0;
        }
        if (optname == SO_SNDTIMEO && optlen >= sizeof(struct timeval)) {
            const struct timeval *tv = (const struct timeval *)optval;
            s->send_timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
            if (s->send_timeout_ms == 0) s->send_timeout_ms = 1;
            return 0;
        }
        if (optname == SO_KEEPALIVE) {
            /* Accepted but can't configure on AT sockets */
            return 0;
        }
    }

    if (level == IPPROTO_TCP) {
        /* TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT — accept but no-op */
        return 0;
    }

    /* Accept unknown options silently */
    return 0;
}

int ml_at_fcntl(int fd, int cmd, int arg)
{
    ml_at_sock_t *s = fd_to_sock(fd);
    if (!s) { errno = EBADF; return -1; }

    if (cmd == F_GETFL) {
        return s->nonblocking ? O_NONBLOCK : 0;
    }

    if (cmd == F_SETFL) {
        s->nonblocking = (arg & O_NONBLOCK) != 0;
        return 0;
    }

    errno = EINVAL;
    return -1;
}

int ml_at_select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout)
{
    uint32_t timeout_ms = 0;
    if (timeout) {
        timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    } else {
        timeout_ms = 30000; /* Default max 30s instead of forever */
    }

    int ready_count = 0;
    fd_set rd_result, wr_result;
    FD_ZERO(&rd_result);
    FD_ZERO(&wr_result);

    int64_t start = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    bool first_poll = true;

    do {
        ready_count = 0;

        /* First pass: check ring buffers and writable status (no AT commands) */
        for (int fd = AT_SOCK_FD_BASE; fd < AT_SOCK_FD_BASE + AT_SOCK_MAX && fd < nfds; fd++) {
            ml_at_sock_t *s = fd_to_sock(fd);
            if (!s) continue;

            if (readfds && FD_ISSET(fd, readfds)) {
                if (ring_used(s) > 0 || s->closed_by_remote) {
                    FD_SET(fd, &rd_result);
                    ready_count++;
                }
            }

            if (writefds && FD_ISSET(fd, writefds)) {
                if (s->connected) {
                    FD_SET(fd, &wr_result);
                    ready_count++;
                }
            }
        }

        if (ready_count > 0) break;

        /* Second pass: query+drain for each socket in one mutex hold */
        xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
        for (int fd = AT_SOCK_FD_BASE; fd < AT_SOCK_FD_BASE + AT_SOCK_MAX && fd < nfds; fd++) {
            ml_at_sock_t *s = fd_to_sock(fd);
            if (!s) continue;

            if (readfds && FD_ISSET(fd, readfds) && ring_used(s) == 0) {
                int pending = at_query_pending(s->link_num);
                if (pending > 0) {
                    at_read_data(s->link_num, pending, NULL);
                }

                if (ring_used(s) > 0) {
                    FD_SET(fd, &rd_result);
                    ready_count++;
                }
            }
        }
        xSemaphoreGive(s_at.at_mutex);

        if (ready_count > 0) break;

        /* No data — yield to let lower-priority tasks (DERP connect) run,
         * then wait before polling again */
        vTaskDelay(1);

        if (!first_poll) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        first_poll = false;

    } while ((esp_timer_get_time() - start) < timeout_us);

    /* Copy results back */
    if (readfds) *readfds = rd_result;
    if (writefds) *writefds = wr_result;
    if (exceptfds) FD_ZERO(exceptfds);

    return ready_count;
}

/* ============================================================================
 * DNS Resolution
 * ========================================================================== */

int ml_at_getaddrinfo(const char *hostname, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res)
{
    if (!s_at.initialized) return EAI_FAIL;
    if (!hostname) return EAI_NONAME;

    *res = NULL;

    /* Check if hostname is already an IP address */
    struct in_addr test_addr;
    if (inet_aton(hostname, &test_addr)) {
        /* It's already an IP — build result directly */
        struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
        if (!ai) return EAI_MEMORY;

        struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
        sin->sin_family = AF_INET;
        sin->sin_addr = test_addr;
        if (service) sin->sin_port = htons(atoi(service));

        ai->ai_family = AF_INET;
        ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
        ai->ai_protocol = hints ? hints->ai_protocol : 0;
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = (struct sockaddr *)sin;
        ai->ai_next = NULL;

        *res = ai;
        return 0;
    }

    /* Resolve via AT+CDNSGIP — this is an ASYNC command.
     * The modem replies "OK" immediately, then sends "+CDNSGIP: 1,..." as a URC.
     * We use at_cmd_wait_urc() to keep reading until we see the result. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+CDNSGIP=\"%s\"", hostname);

    ESP_LOGI(TAG, "DNS: resolving %s", hostname);

    char resp[512];
    xSemaphoreTake(s_at.at_mutex, portMAX_DELAY);
    at_cmd_wait_urc(cmd, "+CDNSGIP:", resp, sizeof(resp), DNS_TIMEOUT_MS);
    xSemaphoreGive(s_at.at_mutex);

    /* Parse +CDNSGIP: 1,"hostname","IP1"[,"IP2"[,"IP3"]]
     * Also handle error: +CDNSGIP: 0,8 (DNS error code)
     *
     * Some carriers return only IPv6 addresses (e.g. 2001:19F0:...).
     * We iterate through ALL returned IPs, prefer IPv4, fall back to IPv6. */
    char *p = strstr(resp, "+CDNSGIP: 1,");
    if (!p) {
        /* Check for DNS error response */
        char *err_p = strstr(resp, "+CDNSGIP: 0,");
        if (err_p) {
            ESP_LOGW(TAG, "DNS error for %s: %s", hostname, err_p);
        } else {
            ESP_LOGW(TAG, "DNS failed for %s (no +CDNSGIP response): %.*s",
                     hostname, (int)strlen(resp) > 200 ? 200 : (int)strlen(resp), resp);
        }
        return EAI_NONAME;
    }

    /* Extract all quoted IP strings from the response.
     * Format: +CDNSGIP: 1,"hostname","IP1"[,"IP2"[,"IP3"]]
     * Skip the first quoted string (hostname), collect the rest. */
    char *ips[4] = {NULL};
    int ip_count = 0;
    int quote_count = 0;
    char *cur_start = NULL;

    for (char *q = p; *q; q++) {
        if (*q == '"') {
            quote_count++;
            if (quote_count >= 3 && (quote_count % 2) == 1) {
                /* Odd quote >= 3 = start of an IP string */
                cur_start = q + 1;
            } else if (quote_count >= 4 && (quote_count % 2) == 0 && cur_start) {
                /* Even quote >= 4 = end of an IP string */
                *q = '\0';
                if (ip_count < 4) {
                    ips[ip_count++] = cur_start;
                }
                cur_start = NULL;
            }
        }
    }

    if (ip_count == 0) {
        ESP_LOGW(TAG, "DNS parse failed for %s (no IPs found)", hostname);
        return EAI_NONAME;
    }

    /* First pass: look for an IPv4 address (preferred — works with our full stack) */
    for (int i = 0; i < ip_count; i++) {
        if (inet_aton(ips[i], &test_addr)) {
            ESP_LOGI(TAG, "DNS: %s -> %s (IPv4)", hostname, ips[i]);

            struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
            if (!ai) return EAI_MEMORY;

            struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
            sin->sin_family = AF_INET;
            sin->sin_addr = test_addr;
            if (service) sin->sin_port = htons(atoi(service));

            ai->ai_family = AF_INET;
            ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
            ai->ai_protocol = hints ? hints->ai_protocol : 0;
            ai->ai_addrlen = sizeof(struct sockaddr_in);
            ai->ai_addr = (struct sockaddr *)sin;
            ai->ai_next = NULL;

            *res = ai;
            return 0;
        }
    }

    /* Second pass: no IPv4 found — try IPv6 */
    struct in6_addr addr6;
    for (int i = 0; i < ip_count; i++) {
        if (inet_pton(AF_INET6, ips[i], &addr6) == 1) {
            ESP_LOGI(TAG, "DNS: %s -> %s (IPv6)", hostname, ips[i]);

            struct addrinfo *ai = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in6));
            if (!ai) return EAI_MEMORY;

            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)(ai + 1);
            sin6->sin6_family = AF_INET6;
            sin6->sin6_addr = addr6;
            if (service) sin6->sin6_port = htons(atoi(service));

            ai->ai_family = AF_INET6;
            ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
            ai->ai_protocol = hints ? hints->ai_protocol : 0;
            ai->ai_addrlen = sizeof(struct sockaddr_in6);
            ai->ai_addr = (struct sockaddr *)sin6;
            ai->ai_next = NULL;

            *res = ai;
            return 0;
        }
    }

    /* Neither IPv4 nor IPv6 parsed successfully */
    ESP_LOGW(TAG, "DNS parse failed for %s (no valid IPs: %s)", hostname, ips[0]);
    return EAI_NONAME;
}

void ml_at_freeaddrinfo(struct addrinfo *res)
{
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res);
        res = next;
    }
}

/* ============================================================================
 * Convenience Wrappers (write/read for mbedTLS BIO callbacks)
 * ========================================================================== */

ssize_t ml_at_write(int fd, const void *buf, size_t len)
{
    return ml_at_send(fd, buf, len, 0);
}

ssize_t ml_at_read(int fd, void *buf, size_t len)
{
    return ml_at_recv(fd, buf, len, 0);
}

#endif /* CONFIG_ML_ENABLE_CELLULAR */
