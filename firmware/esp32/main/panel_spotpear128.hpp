// GC9A01 register changes for the Spotpear ESP32-S3-1.28-BOX's round panel,
// sent after Espressif's default esp_lcd_gc9a01 bring-up. The values follow
// xiaozhi-esp32's spotpear/sp-esp32-s3-1.28-box board at d91d682 (MIT; hardware
// facts only). Source: THIRD_PARTY_NOTICES.md, NOTICE.
#pragma once
#include "esp_lcd_gc9a01.h"
namespace hgp {
static const gc9a01_lcd_init_cmd_t kSpotpearBox128Panel[] = {
    // Gate timing: the first byte of each group is 0x18 where the driver's
    // default table writes 0x38.
    {0x62, "\x18\x0d\x71\xed\x70\x70\x18\x0f\x71\xef\x70\x70", 12, 0},
    {0x63, "\x18\x11\x71\xf1\x70\x70\x18\x13\x71\xf3\x70\x70", 12, 0},
    // VREG1A / VREG1B: 0x1f where the default is 0x13.
    {0xC3, "\x1f", 1, 0},
    {0xC4, "\x1f", 1, 0},
};
}  // namespace hgp
