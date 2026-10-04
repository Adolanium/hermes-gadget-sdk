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
#elif CONFIG_HG_BOARD_AMOLED_175
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
  b.buttons = {0, -1, -1, -1};  // BOOT also works as TALK
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

}  // namespace hgp
