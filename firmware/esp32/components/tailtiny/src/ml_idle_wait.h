/**
 * @file ml_idle_wait.h
 * @brief Wait for relay RX or a queued-work eventfd in one select().
 *
 * Waiting on both descriptors together avoids polling, and avoids the race
 * between checking the TX queue and blocking for socket input. One task owns
 * and drains wake_fd. Header-only so the host test runs the same code.
 */
#pragma once

#include <errno.h>
#include <stdint.h>
#include <sys/select.h>
#include <unistd.h>

static inline int ml_idle_wait(int socket_fd, int wake_fd, uint32_t timeout_ms) {
    if (wake_fd < 0 || wake_fd >= FD_SETSIZE || socket_fd >= FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(wake_fd, &reads);
    int max_fd = wake_fd;
    if (socket_fd >= 0) {
        FD_SET(socket_fd, &reads);
        if (socket_fd > max_fd) max_fd = socket_fd;
    }
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int ready = select(max_fd + 1, &reads, NULL, NULL, &timeout);
    if (ready > 0 && FD_ISSET(wake_fd, &reads)) {
        uint64_t count;
        // A single read drains all queued signals. A later signal stays latched
        // for the next select; it must never be cleared before checking work.
        ssize_t n;
        do { n = read(wake_fd, &count, sizeof(count)); } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof(count)) return -1;
    }
    return ready;
}
