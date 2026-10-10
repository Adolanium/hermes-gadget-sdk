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
#elif CONFIG_HG_BOARD_AIPI_LITE
#define HG_BOARD_NAME "aipi-lite"
#elif CONFIG_HG_BOARD_BOX3
#define HG_BOARD_NAME "esp32-s3-box-3"
#elif CONFIG_HG_BOARD_CORES3
#define HG_BOARD_NAME "m5stack-cores3"
#elif CONFIG_HG_BOARD_WS_ESP32S3_TOUCH_LCD_185C_V2
#define HG_BOARD_NAME "waveshare-esp32-s3-touch-lcd-1.85c-v2"
#elif CONFIG_HG_BOARD_T_DISPLAY_S3
#define HG_BOARD_NAME "tdisplay-s3"
#elif CONFIG_HG_BOARD_CROWPANEL_21
#define HG_BOARD_NAME "crowpanel-2.1"
#elif CONFIG_HG_BOARD_CROWPANEL_50
#define HG_BOARD_NAME "crowpanel-5.0"
#elif CONFIG_HG_BOARD_WS_ESP32S3_LCD_154
#define HG_BOARD_NAME "waveshare-esp32s3-lcd-154"
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
#if CONFIG_HG_BOARD_AMOLED_175C
  b.amoled.rst = 1;
  b.touch.rst = 2;
  b.codec.mclk = 16;
  b.pwr_key = {};  // This model has no TCA9554. PWR retains its hardware role.
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
  b.touch.controller = TouchController::Cst816;  // the CST820 sends the same report
  b.touch.addr = 0x15;
  b.touch.width = 368;
  b.touch.height = 448;
  b.touch.offset_y = 8;     // touch reports glass rows; the UI starts at row 8
  b.axp2101 = true;
  // TCA9554: P0 LCD_RST, P1 the panel's power rail, P2 TOUCH_RST.
  b.expander_reset = {0x20, 0x07, 20, 150};
  b.buttons = {0, -1, -1, -1};  // BOOT also works as TALK
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
  b.touch.controller = TouchController::Cst816;
  b.touch.addr = 0x15;
  b.touch.width = b.touch.height = 360;
  b.buttons = {0, -1, -1, -1};
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
  b.buttons = {0, 4, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "PLUS";
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
  b.touch.controller = TouchController::Box3;  // managed GT911/TT21100 driver
  b.touch.int_gpio = -1;  // touch INT not wired; the driver polls
  b.touch.width = 800;
  b.touch.height = 480;
  b.buttons = {-1, -1, -1, -1};
  b.talk_label = "BOOT";
  b.cancel_label = "Screen";
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
