// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// ILI9342E register sequence from the CoreS3 BSP, expressed as C++ byte strings.
#pragma once

#include "esp_lcd_ili9341.h"

namespace hgp {
constexpr ili9341_lcd_init_cmd_t kCoreS3EPanelInit[] = {
    {0xcf, "\x00\xaa\xe0", 3, 0},
    {0xed, "\x67\x03\x12\x81", 4, 0},
    {0xe8, "\x8a\x01\x78", 3, 0},
    {0xcb, "\x39\x2c\x00\x34\x02", 5, 0},
    {0xf7, "\x20", 1, 0},
    {0xf7, "\x20", 1, 0},
    {0xea, "\x00\x00", 2, 0},
    {0xc0, "\x23", 1, 0},
    {0xc1, "\x11", 1, 0},
    {0xc5, "\x43\x4c", 2, 0},
    {0xc7, "\xa0", 1, 0},
    {0xb1, "\x00\x1b", 2, 0},
    {0xf2, "\x00", 1, 0},
    {0x26, "\x01", 1, 0},
    {0xe0, "\x1f\x36\x36\x3a\x0c\x05\x4f\x87\x3c\x08\x11\x35\x19\x13\x00", 15, 0},
    {0xe1, "\x00\x09\x09\x05\x13\x0a\x30\x78\x43\x07\x0e\x0a\x26\x2c\x1f", 15, 0},
    {0xb7, "\x07", 1, 0},
    {0xb6, "\x08\x82\x27", 3, 0},
    {0xdd, "\x01", 1, 0},
    {0x3a, "\x55", 1, 0},
    {0x21, nullptr, 0, 0},
    {0x36, "\x08", 1, 0},
    {0xd5, "\x00", 1, 0},
    {0xb1, "\x22", 1, 0},
    {0xc8, "\x38", 1, 0},
    {0xcb, "\x1c", 1, 0},
    {0xc9, "\x1a", 1, 0},
    {0xca, "\x1a", 1, 0},
    {0xb7, "\x5a\x41\x11\x19", 4, 0},
    {0xe4, "\x04\x08\x11\x06\x12\x07\x3a\x76\x47\x07\x0f\x0a\x11\x19\x05", 15, 0},
    {0xe5, "\x02\x03\x07\x06\x12\x07\x36\x5f\x48\x06\x10\x0c\x16\x14\x09", 15, 0},
    {0x11, nullptr, 0, 120},
    {0x29, nullptr, 0, 120},
};
}  // namespace hgp
