#include "board.hpp"

#include "sdkconfig.h"

namespace hgp {
namespace {

#if CONFIG_HG_BOARD_ESP32S3_BREADBOARD
// Wiring table: docs/hardware.md#esp32-s3-breadboard
BoardConfig make() {
  BoardConfig b{};
  b.name = "esp32s3-breadboard";
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
  b.name = "custom";
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
