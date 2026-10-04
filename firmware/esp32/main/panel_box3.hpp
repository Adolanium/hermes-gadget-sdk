// SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
// Register values from Espressif's ESP32-S3-BOX-3 BSP.
// See NOTICE and LICENSES/Apache-2.0.txt. Sleep-out/display-on use explicit
// delays and zero data bytes with esp_lcd_ili9341's command interface.
#pragma once

#include "esp_lcd_ili9341.h"

namespace hgp {
constexpr ili9341_lcd_init_cmd_t kBox3PanelInit[] = {
    {0xc8, "\xff\x93\x42", 3, 0},
    {0xc0, "\x0e\x0e", 2, 0},
    {0xc5, "\xd0", 1, 0},
    {0xc1, "\x02", 1, 0},
    {0xb4, "\x02", 1, 0},
    {0xe0, "\x00\x03\x08\x06\x13\x09\x39\x39\x48\x02\x0a\x08\x17\x17\x0f", 15, 0},
    {0xe1, "\x00\x28\x29\x01\x0d\x03\x3f\x33\x52\x04\x0f\x0e\x37\x38\x0f", 15, 0},
    {0xb1, "\x00\x1b", 2, 0},
    {0x36, "\x08", 1, 0},
    {0x3a, "\x55", 1, 0},
    {0xb7, "\x06", 1, 0},
    {0x11, nullptr, 0, 120},
    {0x29, nullptr, 0, 20},
};
}  // namespace hgp
