/**
 * @file wireguard-platform-esp32.c
 * @brief ESP32 platform implementation for wireguard-lwip
 */

#include "wireguard-platform.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/time.h>

/* 2020-01-01: a wall clock earlier than this hasn't been set. */
#define WG_WALL_CLOCK_VALID_AFTER 1577836800

/* ============================================================================
 * Time Functions
 * ========================================================================== */

/* Unix time minus uptime (us), from wireguard_set_time_hint(); 0 = none.
 * Without SNTP or an RTC the timestamp fell back to uptime, which restarts at
 * 0 on every boot, so after a reboot every initiation we sent was dropped as
 * a replay until the peer initiated itself. */
static volatile int64_t s_time_hint_offset_us = 0;

void wireguard_set_time_hint(int64_t unix_seconds) {
    if (unix_seconds < WG_WALL_CLOCK_VALID_AFTER) return;
    s_time_hint_offset_us = unix_seconds * 1000000LL - esp_timer_get_time();
}

uint32_t wireguard_sys_now() {
    // Use lwIP's built-in time function
    return sys_now();
}

void wireguard_tai64n_now(uint8_t *output) {
    // TAI64N format: 8 bytes seconds + 4 bytes nanoseconds.
    //
    // A WireGuard peer drops a handshake initiation whose timestamp isn't newer
    // than the last one it accepted from us (replay protection; wireguard-go
    // logs "handshake replay"). Uptime starts again at 0 on every boot, so after
    // a reboot every initiation we sent was dropped until the peer forgot us or
    // initiated itself. Use the wall clock once it is set (an RTC, SNTP or the
    // application), which keeps growing across reboots; uptime only until then.
    struct timeval tv;
    uint64_t seconds;
    uint32_t nanoseconds;
    gettimeofday(&tv, NULL);
    int64_t hint = s_time_hint_offset_us;
    if (tv.tv_sec >= WG_WALL_CLOCK_VALID_AFTER) {
        seconds = (uint64_t)tv.tv_sec;
        nanoseconds = (uint32_t)tv.tv_usec * 1000;
    } else if (hint != 0) {
        uint64_t now_us = (uint64_t)(esp_timer_get_time() + hint);
        seconds = now_us / 1000000ULL;
        nanoseconds = (now_us % 1000000ULL) * 1000;
    } else {
        uint64_t now_us = esp_timer_get_time();
        seconds = now_us / 1000000ULL;
        nanoseconds = (now_us % 1000000ULL) * 1000;
    }

    // Never go backwards within a boot: SNTP may step the clock back a little,
    // and the peer would drop that initiation too.
    static uint64_t last_seconds = 0;
    static uint32_t last_nanoseconds = 0;
    if (seconds < last_seconds || (seconds == last_seconds && nanoseconds <= last_nanoseconds)) {
        seconds = last_seconds;
        nanoseconds = last_nanoseconds + 1;
        if (nanoseconds >= 1000000000U) {
            seconds++;
            nanoseconds = 0;
        }
    }
    last_seconds = seconds;
    last_nanoseconds = nanoseconds;

    // TAI64 starts at 1970-01-01 00:00:10 TAI (Unix epoch + 10 seconds)
    // Add TAI offset: 2^62 + Unix time
    seconds += 0x400000000000000AULL;

    // Write in big-endian format
    output[0] = (seconds >> 56) & 0xFF;
    output[1] = (seconds >> 48) & 0xFF;
    output[2] = (seconds >> 40) & 0xFF;
    output[3] = (seconds >> 32) & 0xFF;
    output[4] = (seconds >> 24) & 0xFF;
    output[5] = (seconds >> 16) & 0xFF;
    output[6] = (seconds >> 8) & 0xFF;
    output[7] = seconds & 0xFF;

    output[8] = (nanoseconds >> 24) & 0xFF;
    output[9] = (nanoseconds >> 16) & 0xFF;
    output[10] = (nanoseconds >> 8) & 0xFF;
    output[11] = nanoseconds & 0xFF;
}

/* ============================================================================
 * Random Number Generation
 * ========================================================================== */

void wireguard_random_bytes(void *bytes, size_t size) {
    // Use ESP32 hardware RNG
    esp_fill_random(bytes, size);
}

/* ============================================================================
 * Load Management
 * ========================================================================== */

bool wireguard_is_under_load() {
    // For now, always return false (not under load)
    // Could be enhanced to check:
    // - Free heap memory
    // - CPU usage
    // - Number of active connections
    return false;
}

// The device state lock: see wireguard-platform.h.
void *wireguard_platform_lock_create(void) {
	return xSemaphoreCreateMutex();
}

void wireguard_platform_lock(void *lock) {
	xSemaphoreTake((SemaphoreHandle_t)lock, portMAX_DELAY);
}

void wireguard_platform_unlock(void *lock) {
	xSemaphoreGive((SemaphoreHandle_t)lock);
}

void wireguard_platform_lock_free(void *lock) {
	if (lock) vSemaphoreDelete((SemaphoreHandle_t)lock);
}
