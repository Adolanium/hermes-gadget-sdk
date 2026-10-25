#include "board.hpp"

#include "sdkconfig.h"

namespace hgp {
namespace {

// Every image carries "HGBOARD=<board name>" so `hermes gadget update` can refuse
// an image built for another board. The name in the config points into it,
// which also keeps the linker from dropping it.
#if CONFIG_HG_BOARD_ESP32S3_BREADBOARD
#define HG_BOARD_NAME "esp32s3-breadboard"
#elif CONFIG_HG_BOARD_AMOLED_175
#define HG_BOARD_NAME "esp32s3-touch-amoled-1.75"
#elif CONFIG_HG_BOARD_AMOLED_175C
#define HG_BOARD_NAME "esp32s3-touch-amoled-1.75c"
#elif CONFIG_HG_BOARD_AMOLED_18
#define HG_BOARD_NAME "esp32s3-touch-amoled-1.8"
#elif CONFIG_HG_BOARD_AMOLED_206
#define HG_BOARD_NAME "esp32s3-touch-amoled-2.06"
#elif CONFIG_HG_BOARD_AMOLED_216
#define HG_BOARD_NAME "esp32s3-touch-amoled-2.16"
#elif CONFIG_HG_BOARD_AIPI_LITE
#define HG_BOARD_NAME "aipi-lite"
#elif CONFIG_HG_BOARD_SPOTPEAR_MUMA
#define HG_BOARD_NAME "spotpear-esp32-s3-1.54-muma"
#elif CONFIG_HG_BOARD_SPOTPEAR_BOX128
#define HG_BOARD_NAME "spotpear-esp32-s3-1.28-box"
#elif CONFIG_HG_BOARD_BOX3
#define HG_BOARD_NAME "esp32-s3-box-3"
#elif CONFIG_HG_BOARD_CORES3
#define HG_BOARD_NAME "m5stack-cores3"
#elif CONFIG_HG_BOARD_WS_ESP32S3_TOUCH_LCD_185C_V2
#define HG_BOARD_NAME "waveshare-esp32-s3-touch-lcd-1.85c-v2"
#elif CONFIG_HG_BOARD_WS_KNOB_LCD_18
#define HG_BOARD_NAME "waveshare-esp32s3-knob-touch-lcd-1.8"
#elif CONFIG_HG_BOARD_ESP_VOCAT
#define HG_BOARD_NAME "esp-vocat"
#elif CONFIG_HG_BOARD_T_DISPLAY_S3
#define HG_BOARD_NAME "tdisplay-s3"
#elif CONFIG_HG_BOARD_CROWPANEL_21
#define HG_BOARD_NAME "crowpanel-2.1"
#elif CONFIG_HG_BOARD_CROWPANEL_50
#define HG_BOARD_NAME "crowpanel-5.0"
#elif CONFIG_HG_BOARD_CROWPANEL_P4_101
#define HG_BOARD_NAME "crowpanel-advance-p4-10.1"
#elif CONFIG_HG_BOARD_WS_P4_TOUCH_LCD_3_4C
#define HG_BOARD_NAME "waveshare-esp32-p4-touch-lcd-3.4c"
#elif CONFIG_HG_BOARD_WS_ESP32S3_LCD_154
#define HG_BOARD_NAME "waveshare-esp32s3-lcd-154"
#elif CONFIG_HG_BOARD_ESP32_CYD
#define HG_BOARD_NAME "esp32-2432s028-cyd"
#else
#define HG_BOARD_NAME "custom"
#endif
constexpr char kBoardTag[] = "HGBOARD=" HG_BOARD_NAME;
constexpr const char* kBoardName = kBoardTag + 8;

#if CONFIG_HG_BOARD_ESP32S3_BREADBOARD
// Wiring table: docs/hardware.md#esp32-s3-breadboard
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 320;
  b.lcd.height = 240;
  b.lcd.mosi = 11;
  b.lcd.sclk = 12;
  b.lcd.cs = 10;
  b.lcd.dc = 9;
  b.lcd.rst = 8;
  b.lcd.backlight = 7;
  b.mic = {true, 4, 5, 6};
  b.speaker = {true, 15, 16, 17};
  b.buttons = {0, 14, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "B2";
  return b;
}
#elif CONFIG_HG_BOARD_AMOLED_175 || CONFIG_HG_BOARD_AMOLED_175C
// Waveshare ESP32-S3-Touch-AMOLED-1.75: round 466x466 AMOLED (CO5300, QSPI),
// CST9217 touch, ES8311 + ES7210 codecs, AXP2101 PMIC, TCA9554 expander.
// Pins: docs/hardware.md#esp32-s3-touch-amoled-175
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.amoled.enabled = true;
  b.amoled.width = 466;
  b.amoled.height = 466;
  b.amoled.cs = 12;
  b.amoled.sclk = 38;
  b.amoled.d0 = 4;
  b.amoled.d1 = 5;
  b.amoled.d2 = 6;
  b.amoled.d3 = 7;
  b.amoled.rst = 39;
  b.amoled.gap_x = 6;
  b.amoled.round = true;
  b.i2c = {15, 14, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 42;
  b.codec.bclk = 9;
  b.codec.ws = 45;
  b.codec.dout = 8;
  b.codec.din = 10;
  b.codec.pa = 46;
  b.touch.enabled = true;
  b.touch.ppi = 266;  // estimate (see TouchConfig::ppi)
  b.touch.addr = 0x5A;
  b.touch.rst = 40;
  b.touch.width = 466;
  b.touch.height = 466;
  b.touch.mirror_x = true;
  b.touch.mirror_y = true;
  // The side PWR key goes to the AXP2101; its conditioned level (SYS_OUT) is on expander
  // pin P4, high while pressed (Waveshare's hardware reference for this board).
  b.pwr_key = {true, 0x20, 4, true};
  b.axp2101 = true;
  b.buttons = {0, -1, -1, -1};  // BOOT also works as TALK
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
#if CONFIG_HG_BOARD_AMOLED_175
  // QMI8658 (0x6B) on the shared bus, behind the display. Measured on a non-C
  // board standing with its USB-C port at the bottom, which puts the picture's
  // right edge up: chip y +1018 mg; lying face down: chip z +968 mg. So device
  // x = +chip y and z = -chip z; device y = +chip x follows from the right-hand
  // rule (no sideways tilt was measured). Face-down sleep reads z and the sizes
  // of x and y only.
  b.imu.enabled = true;
  b.imu.x_from = 1; b.imu.y_from = 0; b.imu.z_from = 2;
  b.imu.x_sign = 1; b.imu.y_sign = 1; b.imu.z_sign = -1;
  b.rtc.enabled = true;  // PCF85063A at 0x51 on the same bus keeps UTC across power loss
#endif
#if CONFIG_HG_BOARD_AMOLED_175C
  b.amoled.rst = 1;
  b.touch.rst = 2;
  b.codec.mclk = 16;
  b.pwr_key = {};  // This model has no TCA9554: PWR's short press is read from the AXP2101.
  b.axp_power_key = true;
  b.light_sleep = true;
  b.battery_curve = hg::kAmoled175cCurve;
  b.battery_charge_curve = hg::kAmoled175cChargeCurve;
  b.axp_audio_supply = true;  // ALDO1 supplies the analog audio circuit.
  b.codec.speaker_pa = true;  // the amplifier draws power only while sound plays
#endif
  return b;
}
#elif CONFIG_HG_BOARD_AMOLED_18
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2): rectangular 368x448 AMOLED (CO5300,
// QSPI), CST820 touch, an ES8311 codec with an analog electret microphone,
// AXP2101 PMIC and a TCA9554 expander that drives the display/touch resets.
// Pins: docs/hardware.md#esp32-s3-touch-amoled-18
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.amoled.enabled = true;
  b.amoled.width = 368;
  b.amoled.height = 432;    // the UI stays 8 rows clear of the rounded corners (448 glass - 16)
  b.amoled.cs = 12;
  b.amoled.sclk = 11;
  b.amoled.d0 = 4;
  b.amoled.d1 = 5;
  b.amoled.d2 = 6;
  b.amoled.d3 = 7;
  b.amoled.rst = -1;        // reset comes from the TCA9554 (LCD_RST, P0)
  b.amoled.gap_x = 16;      // the controller RAM is wider than the 368-wide glass
  b.amoled.gap_y = 8;       // the 8-row inset at the top
  b.amoled.corner_inset = 18;  // the top bar's text clears the rounded corners
  b.amoled.panel = AmoledPanel::Co5300_368;
  b.i2c = {15, 14, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 16;
  b.codec.bclk = 9;
  b.codec.ws = 45;
  b.codec.dout = 8;
  b.codec.din = 10;
  b.codec.pa = 46;
  b.codec.mic = MicCodec::Es8311;  // analog electret microphone into the ES8311 ADC
  b.touch.enabled = true;
  b.touch.ppi = 322;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;  // the CST820 sends the same report
  b.touch.addr = 0x15;
  b.touch.width = 368;
  b.touch.height = 448;
  b.touch.offset_y = 8;     // touch reports glass rows; the UI starts at row 8
  // The side PWR key goes to the AXP2101; its conditioned level is on expander pin P4,
  // high while pressed, as on the AMOLED-1.75. It acts as CANCEL.
  b.pwr_key = {true, 0x20, 4, true};
  b.axp2101 = true;
  // TCA9554: P0 LCD_RST, P1 the panel's power rail, P2 TOUCH_RST, P4 the PWR key (input).
  b.expander_reset = {0x20, 0x07, 20, 150};
  b.buttons = {0, -1, -1, -1};  // BOOT also works as TALK
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_AMOLED_206
// Waveshare ESP32-S3-Touch-AMOLED-2.06 watch: rectangular 410x502 AMOLED (CO5300,
// QSPI) with rounded corners, FT3168 touch, ES8311 + ES7210 codecs (two
// microphones), NS4150B amplifier, AXP2101 PMIC. No I/O expander.
// Pins: docs/hardware.md#esp32-s3-touch-amoled-206
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.amoled.enabled = true;
  b.amoled.width = 410;
  b.amoled.height = 486;    // the UI stays 8 rows clear of the rounded corners (502 glass - 16)
  b.amoled.cs = 12;
  b.amoled.sclk = 11;
  b.amoled.d0 = 4;
  b.amoled.d1 = 5;
  b.amoled.d2 = 6;
  b.amoled.d3 = 7;
  b.amoled.rst = 8;
  b.amoled.gap_x = 22;      // the controller RAM is wider than the 410-wide glass
  b.amoled.gap_y = 8;       // the 8-row inset at the top
  // The glass corners are ~100 px in radius (measured with a grid): the top bar's text, on glass
  // rows 10-24, needs ~57 px at its top row; 64 plus the bar's 6 px pad leaves some to spare.
  b.amoled.corner_inset = 64;
  b.amoled.panel = AmoledPanel::Co5300_410;
  b.i2c = {15, 14, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 16;
  b.codec.bclk = 41;
  b.codec.ws = 45;
  b.codec.dout = 40;
  b.codec.din = 42;
  b.codec.pa = 46;
  b.codec.speaker_pa = true;  // the amplifier draws power only while sound plays
  // Speech measured ~170 RMS on average (peaks ~3.8k) at the default 24 dB: under tap
  // mode's 500 floor, so tap mode never heard anyone. 30 dB doubles it with headroom
  // to spare, and the floor drops to 200 (the noise estimate still sets the threshold
  // above it in a loud room).
  b.codec.mic_gain_db = 30.0f;
  b.codec.vad_min_rms = 200.0f;
  b.touch.enabled = true;
  b.touch.ppi = 315;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Ft5x06;  // the FT3168 sends the same report
  b.touch.addr = 0x38;
  b.touch.rst = 9;
  b.touch.int_gpio = 38;  // pulses low on a report: reads follow it, and a dozing chip is left alone
  b.touch.width = 410;
  b.touch.height = 502;
  b.touch.offset_y = 8;     // touch reports glass rows; the UI starts at row 8
  b.axp2101 = true;
  b.rtc.enabled = true;  // PCF85063 at 0x51 on the shared bus keeps UTC across power loss
  // PWR reaches only the AXP2101 (PWRON): its short press is read there. Holding it
  // keeps the PMIC's own power behaviour.
  b.axp_power_key = true;
  b.buttons = {0, -1, -1, -1};  // BOOT is TALK
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  // The FT3168's short INT pulse is not configured as a light-sleep wake
  // source. Keep the CPU awake to read its report before it expires.
  return b;
}
#elif CONFIG_HG_BOARD_AMOLED_216
// Waveshare ESP32-S3-Touch-AMOLED-2.16 development board (SKU 33969): square
// 480x480 AMOLED (CO5300, QSPI), CST9220 touch, ES8311 + ES7210 codecs (two
// microphones with echo cancellation), AXP2101 PMIC with a battery header,
// PWR on the AXP2101, GPIO18 as KEY3, BOOT. No TCA9554 expander.
// Pins from Waveshare esp32_s3_touch_amoled_2_16 BSP 2.0.1 (commit 63ac675).
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.amoled.enabled = true;
  b.amoled.width = 480;
  b.amoled.height = 480;    // square glass, no round corners
  b.amoled.cs = 12;
  b.amoled.sclk = 38;
  b.amoled.d0 = 4;
  b.amoled.d1 = 5;
  b.amoled.d2 = 6;
  b.amoled.d3 = 7;
  b.amoled.rst = 39;
  b.amoled.gap_x = 0;       // the controller RAM is exactly 480 wide
  b.amoled.gap_y = 0;
  b.amoled.panel = AmoledPanel::Co5300_480;
  b.i2c = {15, 14, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 42;
  b.codec.bclk = 9;
  b.codec.ws = 45;
  b.codec.dout = 8;
  b.codec.din = 10;
  b.codec.pa = 46;
  b.codec.speaker_pa = true;  // the amplifier draws power only while sound plays
  b.touch.enabled = true;
  b.touch.ppi = 314;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst9217;  // the CST9220 sends the same report
  b.touch.addr = 0x5A;
  b.touch.rst = 40;
  b.touch.int_gpio = 11;
  b.touch.width = 480;
  b.touch.height = 480;
  // MADCTL 0xA0 turns the picture; the BSP turns touch to match with
  // mirror_y then swap_xy (bsp_display_start's touch_flags).
  b.touch.mirror_y = true;
  b.touch.swap_xy = true;
  b.axp2101 = true;
  b.rtc.enabled = true;  // PCF85063 at 0x51 on the shared bus keeps UTC across power loss
  b.axp_power_key = true;
  b.buttons = {0, -1, -1, -1};  // BOOT is TALK
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_WS_ESP32S3_TOUCH_LCD_185C_V2
// Rev2.0 only. Pins and RMNM audio packing: Waveshare factory demo and V2
// schematic, pinned sources in docs/hardware.md. Not compatible with V1.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.controller = LcdController::St77916;
  b.lcd.width = b.lcd.height = 360;
  b.lcd.swap_xy = b.lcd.mirror_x = b.lcd.mirror_y = false;
  b.lcd.round = true;
  b.lcd.sclk = 40;
  b.lcd.mosi = 46;
  b.lcd.d1 = 45;
  b.lcd.d2 = 42;
  b.lcd.d3 = 41;
  b.lcd.cs = 21;
  b.lcd.backlight = 5;
  b.lcd.spi_mhz = 80;
  b.i2c = {11, 10, 400000};
  b.expander_reset.mask = 0x03;  // P0 touch reset, P1 LCD reset (EXIO1/EXIO2 in the demo)
  b.codec.enabled = true;
  b.codec.mclk = 2;
  b.codec.bclk = 48;
  b.codec.ws = 38;
  b.codec.dout = 47;
  b.codec.din = 39;
  b.codec.pa = 15;
  b.codec.stereo32 = true;
  b.codec.rmnm_mics = true;
  b.codec.speaker_pa = true;
  b.codec.dac_mclk = false;     // the factory demo clocks the ES8311 from BCLK
  b.codec.es7210_mics = 0x0f;   // MIC1 playback reference, MIC2/MIC4 microphones
  b.touch.enabled = true;
  b.touch.ppi = 195;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;
  b.touch.width = b.touch.height = 360;
  b.buttons = {0, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_WS_KNOB_LCD_18
// Waveshare ESP32-S3-Knob-Touch-LCD-1.8: round 360x360 SH8601 QSPI LCD, CST816S
// touch, one PDM microphone, rotary encoder, battery + DRV2605 haptics. This port
// doesn't drive the PCM5100A headphone DAC, so it is display + microphone only:
// replies are shown as text, never spoken. Pins: Waveshare's ESP-IDF demo
// (08_LVGL_Test, 04_Encoder_Test and 07_Audio_Test user_config.h); see NOTICE.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.controller = LcdController::Sh8601;  // the 1.8" round glass is SH8601, not ST77916
  b.lcd.width = b.lcd.height = 360;
  b.lcd.swap_xy = b.lcd.mirror_x = b.lcd.mirror_y = false;
  b.lcd.round = true;
  b.lcd.font_scale = 2;  // the 360x360 round glass picks scale 1 automatically; double it for readability
  b.lcd.sclk = 13;
  b.lcd.mosi = 15;
  b.lcd.d1 = 16;
  b.lcd.d2 = 17;
  b.lcd.d3 = 18;
  b.lcd.cs = 14;
  b.lcd.rst = 21;
  b.lcd.backlight = 47;
  b.lcd.spi_mhz = 80;
  b.i2c = {11, 12, 400000};
  b.mic = {true, 45, -1, 46, true};  // PDM mic: CLK 45, DATA 46 (sck=45 sd=46, ws unused)
  b.touch.enabled = true;
  b.touch.ppi = 200;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;
  b.touch.rst = 10;
  b.touch.int_gpio = 9;
  b.touch.width = b.touch.height = 360;
  b.encoder = {8, 7, true};  // knob ring: one pulse per detent, channel = direction
  b.buttons = {0, -1, -1, -1};  // BOOT also works as TALK
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_WS_ESP32S3_LCD_154
// Waveshare ESP32-S3-LCD-1.54 (SKUs 33866/33867, the non-touch version): 1.54"
// 240x240 ST7789 over SPI, ES8311 speaker DAC + ES7210 microphone ADC on one
// duplex I2S bus, NS4150B amplifier, QMI8658 IMU, BOOT/PLUS/PWR keys.
// Pins: docs/hardware.md#waveshare-esp32-s3-lcd-154 (Waveshare's factory demo).
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 240;
  b.lcd.height = 240;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.invert = true;
  b.lcd.gap_x = 0;
  b.lcd.gap_y = 0;
  b.lcd.mosi = 39;
  b.lcd.sclk = 38;
  b.lcd.cs = 21;
  b.lcd.dc = 45;
  b.lcd.rst = 40;
  b.lcd.backlight = 46;
  b.i2c = {42, 41, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 8;
  b.codec.bclk = 9;
  b.codec.ws = 10;
  b.codec.dout = 12;
  b.codec.din = 11;
  b.codec.pa = 7;
  // R27/R32 divide VBAT by three. BAT_EN is high to keep battery power on;
  // CHG_STAT is low while charging. PWR remains part of the power circuit.
  b.latch_power = {true, 1, 2, 3};
  // No USB-status input: report external power from CHG_STAT or a USB host.
  b.latch_power.usb_external_power = true;
  // PWR also reads low on GPIO5 while pressed (Waveshare's factory demo powers
  // off when it is held).
  b.latch_power.key = 5;
  // The Touch-LCD-1.54 (SKUs 33868/33869) adds a CST816 on the same I2C bus,
  // reset on GPIO47 (Waveshare's factory demo). On the non-touch SKUs nothing
  // answers at 0x15 and the touchscreen is reported absent.
  b.touch.enabled = true;
  b.touch.ppi = 220;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;
  b.touch.rst = 47;
  b.touch.width = b.touch.height = 240;
  b.buttons = {0, 4, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "PLUS";
  b.light_sleep = true;  // dozing on battery; keep_pins_through_sleep holds the latch and panel
  return b;
}
#elif CONFIG_HG_BOARD_BOX3
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.controller = LcdController::Box3;
  b.lcd.width = 320;
  b.lcd.height = 240;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = true;
  b.lcd.mirror_y = true;
  b.lcd.invert = false;
  b.lcd.mosi = 6;
  b.lcd.sclk = 7;
  b.lcd.cs = 5;
  b.lcd.dc = 4;
  b.lcd.rst = 48;
  b.lcd.reset_active_high = true;
  b.lcd.backlight = 47;
  b.i2c = {8, 18, 400000};
  b.codec = {true, 2, 17, 45, 15, 16, 46};
  b.touch.enabled = true;
  b.touch.ppi = 167;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Box3;
  b.touch.int_gpio = 3;  // Box3 wires the touch IRQ to GPIO3
  b.touch.width = 320;
  b.touch.height = 240;
  b.buttons = {0, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_CORES3
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.cores3 = true;
  b.axp2101 = true;
  b.i2c = {12, 11, 400000};
  b.lcd.enabled = true;
  b.lcd.controller = LcdController::CoreS3;
  b.lcd.width = 320;
  b.lcd.height = 240;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.invert = true;
  b.lcd.mosi = 37;
  b.lcd.sclk = 36;
  b.lcd.cs = 3;
  b.lcd.dc = 35;
  b.codec = {true, 0, 34, 33, 13, 14, -1};
  b.codec.speaker = SpeakerCodec::Aw88298;
  b.touch.enabled = true;
  b.touch.ppi = 200;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Ft5x06;
  b.touch.addr = 0x38;
  b.touch.width = 320;
  b.touch.height = 240;
  b.talk_label = "Hold screen";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_T_DISPLAY_S3
// LilyGO T-Display-S3: 1.9" 170x320 ST7789 on an 8-bit i80 parallel bus, BOOT
// (GPIO 0) and Button2 (GPIO 14), battery ADC on GPIO 4 behind a 1:2 divider.
// GPIO 15 gates the panel's peripheral rail and must be high before the display
// is initialised, or the screen stays dark on battery.
// Pins: docs/hardware.md#lilygo-t-display-s3 (LilyGO's own examples).
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 320;
  b.lcd.height = 170;
  b.lcd.swap_xy = true;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = true;
  b.lcd.invert = true;
  b.lcd.gap_x = 0;
  b.lcd.gap_y = 35;
  b.lcd.cs = 6;
  b.lcd.dc = 7;
  b.lcd.rst = 5;
  b.lcd.backlight = 38;
  b.lcd.bus.type = LcdBus::Type::I80;
  b.lcd.bus.data[0] = 39;
  b.lcd.bus.data[1] = 40;
  b.lcd.bus.data[2] = 41;
  b.lcd.bus.data[3] = 42;
  b.lcd.bus.data[4] = 45;
  b.lcd.bus.data[5] = 46;
  b.lcd.bus.data[6] = 47;
  b.lcd.bus.data[7] = 48;
  b.lcd.bus.wr = 8;
  b.lcd.bus.pclk_mhz = 16;
  b.latch_power = {true, 4, 15, -1, 38, 2, 4300, false};
  b.buttons = {0, 14, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "B2";
  return b;
}
#elif CONFIG_HG_BOARD_AIPI_LITE
// AIPI Lite (Xorigin): 128x128 ST7789-family SPI panel, one ES8311 codec doing
// both directions on a single duplex I2S bus, BOOT (GPIO42) and power (GPIO1)
// keys, and a power latch on GPIO10.
// Pins: xiaozhi-esp32 board xorigin/aipi-lite (upstream mapping).
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 128;
  b.lcd.height = 128;
  b.lcd.swap_xy = true;
  b.lcd.mirror_x = true;
  b.lcd.mirror_y = false;
  b.lcd.invert = false;
  b.lcd.gap_x = 0;
  b.lcd.gap_y = 0;
  b.lcd.mosi = 17;
  b.lcd.sclk = 16;
  b.lcd.cs = 15;
  b.lcd.dc = 7;
  b.lcd.rst = 18;
  b.lcd.backlight = 3;
  b.lcd.bgr = true;      // upstream drives the panel in BGR order
  b.lcd.spi_mhz = 20;    // and at 20 MHz
  b.i2c = {5, 4, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 6;
  b.codec.bclk = 14;
  b.codec.ws = 12;
  b.codec.dout = 11;
  b.codec.din = 13;
  b.codec.pa = 9;
  b.codec.mic = MicCodec::Es8311;
  // Power rail: GPIO10 must latch high before the ES8311 on I2C answers. The
  // battery divider on GPIO2 (ADC1_CH1) is not measured yet, and upstream reads
  // the GPIO8 charge signal active-high where LatchPower expects active-low, so
  // neither is reported for now.
  b.latch_power = {true, -1, 10, -1};
  b.buttons = {42, 1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "PWR";
  return b;
}
#elif CONFIG_HG_BOARD_ESP_VOCAT
// Espressif ESP-VoCat v1.2: ESP32-S3-WROOM-1 (16 MB flash, 16 MB octal PSRAM),
// round 1.85" 360x360 ST77916 over QSPI, CST816S touch, ES8311 DAC into an
// NS4150B amplifier, ES7210 with two microphones on MIC1/MIC2 (MIC3 carries the
// speaker loopback, unused here). v1.0 boards wire audio and LCD reset
// differently and are not supported.
// Pins: esp-bsp bsp/esp_vocat and Espressif's v1.2 schematic, pinned in
// docs/hardware.md#esp-vocat.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  // GPIO9 low switches the LCD/SD 3.3 V rail on (P-FET); GPIO48 high enables
  // the codec supply. Both must be on before the panel, touch and codecs start.
  b.power_rails[0] = {9, false};
  b.power_rails[1] = {48, true};
  b.power_rails_settle_ms = 20;
  b.lcd.enabled = true;
  b.lcd.controller = LcdController::St77916;
  b.lcd.st77916_panel = St77916Panel::Vocat;
  b.lcd.width = b.lcd.height = 360;
  b.lcd.swap_xy = b.lcd.mirror_x = b.lcd.mirror_y = false;
  b.lcd.round = true;
  b.lcd.sclk = 18;
  b.lcd.mosi = 46;  // QSPI D0
  b.lcd.d1 = 13;
  b.lcd.d2 = 11;
  b.lcd.d3 = 12;
  b.lcd.cs = 14;
  b.lcd.rst = 47;
  b.lcd.reset_active_high = true;  // drives an inverting FET on LCD_RST
  b.lcd.backlight = 44;            // the UART0 RX pin; the console is USB-Serial-JTAG
  b.lcd.spi_mhz = 80;
  b.i2c = {2, 1, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 42;
  b.codec.bclk = 40;
  b.codec.ws = 39;
  b.codec.dout = 41;
  b.codec.din = 3;
  b.codec.pa = 15;
  b.touch.enabled = true;
  b.touch.ppi = 195;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;  // INT on GPIO 10; the CST816 driver polls
  b.touch.width = b.touch.height = 360;
  b.buttons = {0, -1, -1, -1};  // BOOT, under the base
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_SPOTPEAR_MUMA
// Spotpear ESP32-S3-1.54 "muma": ESP32-S3R8, 16 MB flash, 1.54" 240x240 ST7789
// over SPI, one ES8311 codec for both directions, CST816D touch on its own I2C
// bus, BOOT key and a power latch on GPIO3.
// Pins: xiaozhi-esp32 board spotpear/sp-esp32-s3-1.54-muma (upstream mapping).
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 240;
  b.lcd.height = 240;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.invert = true;
  b.lcd.gap_x = 0;
  b.lcd.gap_y = 0;
  b.lcd.mosi = 2;
  b.lcd.sclk = 4;
  b.lcd.cs = 5;
  b.lcd.dc = 47;
  b.lcd.rst = 38;
  b.lcd.backlight = 42;
  b.lcd.backlight_invert = true;  // upstream drives the backlight PWM inverted
  b.i2c = {15, 14, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 16;
  b.codec.bclk = 9;
  b.codec.ws = 45;
  b.codec.dout = 8;
  b.codec.din = 10;
  b.codec.pa = 46;
  b.codec.mic = MicCodec::Es8311;
  // The touch controller sits on a second I2C bus. Only the touch version has
  // it; the driver reports the touchscreen absent when nothing answers.
  b.touch.enabled = true;
  b.touch.ppi = 220;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;
  b.touch.bus = {11, 7, 400000};
  b.touch.rst = 6;
  b.touch.width = b.touch.height = 240;
  // GPIO3 high keeps the battery path on; upstream lowers it to shut down.
  // GPIO1 reads the battery through a divider by two (upstream's 1980..2480 raw
  // range at 12 dB is about 3.1..4.0 V at the cell), and GPIO41 reads high
  // while charging.
  b.latch_power = {true, 1, 3, 41};
  b.latch_power.mv_ratio = 2;
  b.latch_power.charging_active_high = true;
  b.latch_power.usb_external_power = true;
  b.buttons = {0, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  b.light_sleep = true;  // dozing on battery; keep_pins_through_sleep holds the latch and panel
  return b;
}
#elif CONFIG_HG_BOARD_SPOTPEAR_BOX128
// Spotpear ESP32-S3-1.28-BOX, the round robot: ESP32-S3R8, 16 MB flash, 1.28"
// 240x240 round GC9A01 over SPI, one ES8311 for both directions, CST816D touch
// on its own I2C bus, BOOT key, a power latch on GPIO3 and a battery divider.
// The muma's wiring with a round panel in place of the ST7789.
// Pins: xiaozhi-esp32 board spotpear/sp-esp32-s3-1.28-box at d91d682.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.controller = LcdController::Gc9a01;
  b.lcd.gc9a01_panel = Gc9a01Panel::SpotpearBox128;
  b.lcd.width = b.lcd.height = 240;
  b.lcd.round = true;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = true;
  b.lcd.mirror_y = false;
  b.lcd.invert = true;
  b.lcd.bgr = true;
  b.lcd.mosi = 2;
  b.lcd.sclk = 4;
  b.lcd.cs = 5;
  b.lcd.dc = 47;
  b.lcd.rst = 38;
  b.lcd.backlight = 42;
  b.lcd.backlight_invert = true;
  b.i2c = {15, 14, 400000};
  b.codec.enabled = true;
  b.codec.mclk = 16;
  b.codec.bclk = 9;
  b.codec.ws = 45;
  b.codec.dout = 8;
  b.codec.din = 10;
  b.codec.pa = 46;
  b.codec.mic = MicCodec::Es8311;
  b.touch.enabled = true;
  b.touch.ppi = 188;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;
  b.touch.bus = {11, 7, 400000};
  b.touch.rst = 6;
  b.touch.width = b.touch.height = 240;
  // GPIO3 high keeps the battery path on; upstream lowers it to shut down.
  // GPIO1 reads the battery through a halving divider, and GPIO41 is high while
  // charging. Upstream's charge table (raw 1980-2480 at 12 dB) puts a full cell
  // near 4.0 V at that ratio; the ratio is not measured on a board yet.
  b.latch_power = {true, 1, 3, 41, -1, 2};
  b.latch_power.charging_active_high = true;
  b.status_led = 48;
  b.buttons = {0, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_CROWPANEL_21
// Elecrow CrowPanel 2.1-inch HMI; pin map from Elecrow's official Arduino demo.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 480;
  b.lcd.height = 480;
  b.lcd.round = true;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.backlight = 6;
  // Vendor sketch drives GPIO6 PWM active-high (ledcWrite 204/255 = bright).
  b.lcd.backlight_invert = false;
  b.lcd.bus.type = LcdBus::Type::Rgb;
  b.lcd.rgb.de = 40;
  b.lcd.rgb.vsync = 7;
  b.lcd.rgb.hsync = 15;
  b.lcd.rgb.pclk = 41;
  const int rgb_data[16] = {46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9, 5, 45, 48, 47, 21};
  for (int i = 0; i < 16; ++i) b.lcd.rgb.data[i] = rgb_data[i];
  b.lcd.rgb.cmd_cs = 16;
  b.lcd.rgb.cmd_sclk = 2;
  b.lcd.rgb.cmd_sda = 1;
  b.lcd.rgb.i2c_expander = 0x21;
  b.lcd.rgb.pclk_hz = 12000000;
  b.i2c = {38, 39, 400000};
  b.touch.enabled = true;
  b.touch.ppi = 229;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Cst816;  // its CST826 sends the same report
  b.touch.addr = 0x15;
  b.touch.width = 480;
  b.touch.height = 480;
  b.pwr_key.enabled = true;
  b.pwr_key.addr = 0x21;
  b.pwr_key.bit = 5;
  b.pwr_key.active_high = false;
  b.pwr_key.pcf8574 = true;  // the knob's push button is PCF8574 P5
  b.encoder = {42, 4};
  b.buttons = {-1, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Knob";
  return b;
}
#elif CONFIG_HG_BOARD_CROWPANEL_50
// Elecrow CrowPanel 5.0-inch HMI (DIS07050H). 800x480 RGB ILI6122/ILI5960 with
// GT911 capacitive touch. Pin map from Elecrow's official Arduino demo (repo
// Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480). No onboard audio.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 800;
  b.lcd.height = 480;
  b.lcd.round = false;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.backlight = 2;
  // GPIO2 drives the backlight rail; the vendor sketch writes it HIGH on boot.
  b.lcd.backlight_invert = false;
  b.lcd.bus.type = LcdBus::Type::Rgb;
  b.lcd.rgb.controller = RgbController::Ili6122;
  b.lcd.rgb.de = 40;
  b.lcd.rgb.vsync = 41;
  b.lcd.rgb.hsync = 39;
  b.lcd.rgb.pclk = 0;
  // B0..B4, G0..G5, R0..R4 in the vendor sketch's D0..D15 order.
  const int rgb_data[16] = {8, 3, 46, 9, 1, 5, 6, 7, 15, 16, 4, 45, 48, 47, 21, 14};
  for (int i = 0; i < 16; ++i) b.lcd.rgb.data[i] = rgb_data[i];
  b.lcd.rgb.swap_red_blue = false;  // blue on data[0..4] is native RGB565
  b.lcd.rgb.i2c_expander = -1;  // no PCF8574; GPIO 38 is the user GPIO_D header pin, not a panel enable
  b.lcd.rgb.pclk_hz = 15000000;
  // Scanout timings from the vendor LGFX config.
  b.lcd.rgb.timings.hsync_pulse_width = 4;
  b.lcd.rgb.timings.hsync_back_porch = 43;
  b.lcd.rgb.timings.hsync_front_porch = 8;
  b.lcd.rgb.timings.vsync_pulse_width = 4;
  b.lcd.rgb.timings.vsync_back_porch = 12;
  b.lcd.rgb.timings.vsync_front_porch = 8;
  b.lcd.rgb.timings.pclk_active_neg = true;
  b.i2c = {19, 20, 400000};
  b.touch.enabled = true;
  b.touch.ppi = 187;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Box3;  // managed GT911/TT21100 driver
  b.touch.int_gpio = -1;  // touch INT not wired; the driver polls
  b.touch.width = 800;
  b.touch.height = 480;
  b.buttons = {-1, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Screen";
  return b;
}
#elif CONFIG_HG_BOARD_CROWPANEL_P4_101
// Elecrow CrowPanel Advanced 10.1-inch ESP32-P4 V1.1.
// Hardware facts follow Elecrow V1.1 files at pinned commit 6c7c851f3105349dc24b12ad058655ba6afa98ee; no vendor code is copied. URLs are in docs/hardware.md.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.mic = {true, 24, -1, 26, true};  // PDM clock/data on I2S0
  b.speaker.enabled = true;
  b.speaker.bclk = 22;
  b.speaker.ws = 21;
  b.speaker.dout = 23;
  b.p4_speaker.enabled = true;
  b.p4_speaker.i2s_port = 1;
  b.p4_speaker.pa = 30;
  b.p4_speaker.pa_active_low = true;
  b.p4_speaker.stereo = true;
  b.p4_speaker.left_align = true;
  b.lcd.enabled = true;
  b.lcd.width = 1024;
  b.lcd.height = 600;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.invert = false;
  b.lcd.backlight = 31;
  b.lcd.rst = -1;  // not bonded on V1.1
  b.lcd.bus.type = LcdBus::Type::Dsi;
  b.lcd.dsi.lanes = 2;
  b.lcd.dsi.lane_bit_rate_mbps = 900;
  b.lcd.dsi.dpi_clock_mhz = 51;
  b.lcd.dsi.ldo_channel = 3;
  b.lcd.dsi.ldo_voltage_mv = 2500;
  b.lcd.dsi.use_dma2d = true;
  b.i2c = {45, 46, 400000};
  b.stc8_battery = true;
  b.touch.enabled = true;
  b.touch.ppi = 118;  // estimate (see TouchConfig::ppi)
  b.touch.controller = TouchController::Gt911;
  b.touch.addr = 0x14;
  b.touch.width = 1024;
  b.touch.height = 600;
  b.touch.rst = 40;
  b.touch.int_gpio = 42;
  b.buttons = {35, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_ESP32_CYD
// ESP32-2432S028 "Cheap Yellow Display" (CYD), the revision with two USB ports
// (micro-USB and USB-C) and an ST7789 panel: a classic ESP32-WROOM-32 with 4 MB
// flash and no PSRAM, a 2.8" 240x320 panel in landscape, a resistive XPT2046
// touchscreen on its own pins, a microSD slot on VSPI and an SC8002B amplifier
// on the DAC. The panel, the BOOT key, the amplifier (through the chip's DAC) and
// a PDM microphone on the 4-pin header are wired here; touch and microSD are not
// brought up yet. The original micro-USB-only CYD has an ILI9341 and needs its
// own profile.
// Pins: witnessmenow/ESP32-Cheap-Yellow-Display (MIT), commit 0564a140, PINS.md
// and DisplayConfig/CYD2USB/User_Setup.h: the panel on HSPI with SCLK 14,
// MOSI 13, MISO 12, DC 2, CS 15, no reset, backlight GPIO 21 active-high; the
// amplifier on GPIO 26; IO22 and IO27 on the header.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  b.lcd.enabled = true;
  b.lcd.width = 320;
  b.lcd.height = 240;
  // Landscape orientation. MADCTL 0x60 (MV|MX) rendered the UI rotated 180
  // degrees on this panel, so both mirror bits are swapped: MADCTL 0xA0
  // (MV|MY), the same landscape orientation the ST7789 T-Display-S3 uses.
  b.lcd.swap_xy = true;
  b.lcd.mirror_x = true;
  b.lcd.mirror_y = true;
  b.lcd.invert = false;    // LovyanGFX CYD_INVERT = 1
  b.lcd.bgr = false;      // LovyanGFX rgb_order = true writes MAD_RGB, i.e. native RGB565
  b.lcd.mosi = 13;
  b.lcd.miso = 12;
  b.lcd.sclk = 14;
  b.lcd.cs = 15;
  b.lcd.dc = 2;
  b.lcd.rst = -1;         // no reset line; the panel resets on power-up
  b.lcd.backlight = 21;
  b.lcd.spi_mhz = 40;
  b.speaker_dac = 26;     // SC8002B amplifier input on DAC channel 1 (GPIO26)
  // A PDM MEMS microphone on the 4-pin header: IO22 carries the PDM clock and
  // IO27 the data, so nothing else is needed (SELECT is tied to ground on the
  // module). ESP32's only PDM-to-PCM converter is on I2S0, and I2sMic picks that
  // port for a PDM mic; the board has no I2S speaker, so the port is free.
  b.mic = {true, 22, -1, 27, true};
  b.buttons = {0, -1, -1, -1};  // BOOT, the only key on the board
  b.talk_label = "BOOT";
  return b;
}
#elif CONFIG_HG_BOARD_WS_P4_TOUCH_LCD_3_4C
// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C (SKU 31523): 800x800 round MIPI-DSI
// JD9365 panel, GT911 touch, ES8311 speaker codec + ES7210 mic ADC, C6 Wi-Fi.
BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
  // 800x800 MIPI-DSI JD9365 panel.
  b.lcd.enabled = true;
  b.lcd.width = 800;
  b.lcd.height = 800;
  b.lcd.swap_xy = false;
  b.lcd.mirror_x = false;
  b.lcd.mirror_y = false;
  b.lcd.invert = false;
  b.lcd.round = true;
  b.lcd.backlight = 26;          // active-high PWM
  b.lcd.backlight_invert = true;
  b.lcd.rst = 27;               // LCD reset (active low)
  b.lcd.bus.type = LcdBus::Type::Dsi;
  b.lcd.dsi.controller = DsiController::Jd9365;
  b.lcd.dsi.lanes = 2;
  b.lcd.dsi.lane_bit_rate_mbps = 1500;
  b.lcd.dsi.dpi_clock_mhz = 80;
  b.lcd.dsi.ldo_channel = 3;
  b.lcd.dsi.ldo_voltage_mv = 2500;
  b.lcd.dsi.use_dma2d = true;
  b.lcd.dsi.hsync_pulse_width = 20;
  b.lcd.dsi.hsync_back_porch = 20;
  b.lcd.dsi.hsync_front_porch = 40;
  b.lcd.dsi.vsync_pulse_width = 4;
  b.lcd.dsi.vsync_back_porch = 12;
  b.lcd.dsi.vsync_front_porch = 24;
  // Shared I2C bus on GPIO7/8: touch, both codecs.
  b.i2c = {7, 8, 400000};
  // Touch: GT911, reset/int not bonded.
  b.touch.enabled = true;
  b.touch.controller = TouchController::Gt911;
  b.touch.width = 800;
  b.touch.height = 800;
  b.touch.ppi = 235;  // estimate (see TouchConfig::ppi)
  b.touch.rst = -1;
  b.touch.int_gpio = -1;
  // Audio: ES8311 speaker + ES7210 mic.
  b.codec.enabled = true;
  b.codec.mclk = 13;
  b.codec.bclk = 12;
  b.codec.ws = 10;
  b.codec.dout = 9;
  b.codec.din = 11;
  b.codec.pa = 53;
  b.codec.speaker_pa = false;
  b.codec.mic = MicCodec::Es7210;
  b.codec.speaker = SpeakerCodec::Es8311;
  b.codec.dac_mclk = true;
  b.codec.rmnm_mics = false;
  b.codec.es7210_mics = 0x03;
  // Wi-Fi via C6 over SDIO (standard P4 function board pins).
  b.buttons = {0, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Swipe down";
  return b;
}
#elif CONFIG_HG_BOARD_CUSTOM
// Kconfig leaves a disabled bool undefined, so map each one explicitly.
#ifdef CONFIG_HG_LCD_SWAP_XY
constexpr bool kSwapXY = true;
#else
constexpr bool kSwapXY = false;
#endif
#ifdef CONFIG_HG_LCD_MIRROR_X
constexpr bool kMirrorX = true;
#else
constexpr bool kMirrorX = false;
#endif
#ifdef CONFIG_HG_LCD_MIRROR_Y
constexpr bool kMirrorY = true;
#else
constexpr bool kMirrorY = false;
#endif
#ifdef CONFIG_HG_LCD_INVERT
constexpr bool kInvert = true;
#else
constexpr bool kInvert = false;
#endif

BoardConfig make() {
  BoardConfig b{};
  b.name = kBoardName;
#if CONFIG_HG_LCD_ENABLED
  b.lcd.enabled = true;
  b.lcd.width = CONFIG_HG_LCD_WIDTH;
  b.lcd.height = CONFIG_HG_LCD_HEIGHT;
  b.lcd.swap_xy = kSwapXY;
  b.lcd.mirror_x = kMirrorX;
  b.lcd.mirror_y = kMirrorY;
  b.lcd.invert = kInvert;
  b.lcd.gap_x = CONFIG_HG_LCD_GAP_X;
  b.lcd.gap_y = CONFIG_HG_LCD_GAP_Y;
  b.lcd.spi_mhz = CONFIG_HG_LCD_SPI_MHZ;
#endif
  b.lcd.mosi = CONFIG_HG_LCD_PIN_MOSI;
  b.lcd.sclk = CONFIG_HG_LCD_PIN_SCLK;
  b.lcd.cs = CONFIG_HG_LCD_PIN_CS;
  b.lcd.dc = CONFIG_HG_LCD_PIN_DC;
  b.lcd.rst = CONFIG_HG_LCD_PIN_RST;
  b.lcd.backlight = CONFIG_HG_LCD_PIN_BL;
#if CONFIG_HG_MIC_ENABLED
  b.mic = {true, CONFIG_HG_MIC_PIN_SCK, CONFIG_HG_MIC_PIN_WS, CONFIG_HG_MIC_PIN_SD};
#endif
#if CONFIG_HG_SPK_ENABLED
  b.speaker = {true, CONFIG_HG_SPK_PIN_BCLK, CONFIG_HG_SPK_PIN_WS, CONFIG_HG_SPK_PIN_DOUT};
#endif
  b.buttons = {CONFIG_HG_BTN_TALK, CONFIG_HG_BTN_CANCEL, CONFIG_HG_BTN_UP, CONFIG_HG_BTN_DOWN};
  b.status_led = CONFIG_HG_STATUS_LED;
  return b;
}
#else
#error "Select a board in menuconfig (Hermes Gadget -> Board)"
#endif

}  // namespace

const BoardConfig& board_config() {
  static const BoardConfig config = make();
  return config;
}

int lcd_power_pin(const BoardConfig& b) {
  // The T-Display-S3's panel rail is gated by GPIO 15. It reuses the battery
  // latch's enable pin, so report it from there rather than from the LCD config.
#if CONFIG_HG_BOARD_T_DISPLAY_S3
  return b.latch_power.enable;
#else
  (void)b;
  return -1;
#endif
}

const char* board_tag() { return kBoardTag; }

}  // namespace hgp
