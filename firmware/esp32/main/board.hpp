// Board description: which peripherals exist and how they are wired.
//
// A board is data, not code: add one by returning another BoardConfig from
// board.cpp (selected through the "Board" Kconfig choice). Boards whose audio
// goes through a codec chip, or whose display is not an SPI ST7789, add a
// driver beside port_audio.cpp / port_display.cpp — see docs/porting.md.
#pragma once

#include <cstdint>

namespace hgp {

struct LcdConfig {
  bool enabled = false;
  uint16_t width = 320, height = 240;  // after rotation
  bool swap_xy = true, mirror_x = true, mirror_y = false, invert = true;
  int gap_x = 0, gap_y = 0;
  int mosi = -1, sclk = -1, cs = -1, dc = -1, rst = -1, backlight = -1;
  int spi_mhz = 40;
};

struct I2sMicConfig {
  bool enabled = false;
  int sck = -1, ws = -1, sd = -1;
};

struct I2sSpeakerConfig {
  bool enabled = false;
  int bclk = -1, ws = -1, dout = -1;
};

struct ButtonConfig {
  int talk = -1, cancel = -1, up = -1, down = -1;  // active-low GPIOs, -1 = absent
};

struct BoardConfig {
  const char* name;
  LcdConfig lcd;
  I2sMicConfig mic;
  I2sSpeakerConfig speaker;
  ButtonConfig buttons;
  int status_led = -1;
  const char* talk_label = "TALK";
  const char* cancel_label = "CANCEL";
};

const BoardConfig& board_config();

}  // namespace hgp
