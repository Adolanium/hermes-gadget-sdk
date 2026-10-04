// Board description: which peripherals exist and how they are wired.
//
// A board is data, not code: add one by returning another BoardConfig from
// board.cpp (selected through the "Board" Kconfig choice). The drivers it can
// pick from: an SPI ST7789 or a QSPI CO5300 AMOLED display; plain I2S
// microphone and amplifier, or ES7210/ES8311 codecs; GPIO buttons, a CST9217
// touchscreen and a key read through a TCA9554 expander. See docs/porting.md.
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

// QSPI AMOLED with a CO5300 controller (round 466x466 panels).
struct AmoledConfig {
  bool enabled = false;
  uint16_t width = 466, height = 466;
  int cs = -1, sclk = -1, d0 = -1, d1 = -1, d2 = -1, d3 = -1, rst = -1;
  int gap_x = 0, gap_y = 0;  // the controller's RAM is wider than the glass
  int qspi_mhz = 40;
  bool round = false;
};

struct I2cBusConfig {
  int sda = -1, scl = -1;
  uint32_t hz = 400000;
};

// ES8311 (speaker DAC) and ES7210 (microphone ADC) sharing one duplex I2S bus,
// controlled over the I2C bus.
struct CodecAudioConfig {
  bool enabled = false;
  int mclk = -1, bclk = -1, ws = -1, dout = -1, din = -1;
  int pa = -1;               // speaker amplifier enable, active high
  float amp_supply_v = 5.0f;  // amplifier supply; the ES8311 driver sets its output level from it
  float mic_gain_db = 24.0f;
};

// CST9217 capacitive touch on the I2C bus: hold to talk, tap, swipe down to cancel.
struct TouchConfig {
  bool enabled = false;
  uint8_t addr = 0x5A;
  int rst = -1;
  uint16_t width = 0, height = 0;
  bool mirror_x = false, mirror_y = false;
};

// A key whose level is read from a TCA9554 I/O expander input (e.g. a PMIC's
// power key). Acts as CANCEL: a press cancels, holding 2 s starts a new session.
struct ExpanderKeyConfig {
  bool enabled = false;
  uint8_t addr = 0x20;
  uint8_t bit = 0;
  bool active_high = true;
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
  AmoledConfig amoled;
  I2cBusConfig i2c;
  CodecAudioConfig codec;
  TouchConfig touch;
  ExpanderKeyConfig pwr_key;
  bool axp2101 = false;
  int status_led = -1;
  const char* talk_label = "TALK";
  const char* cancel_label = "CANCEL";
};

const BoardConfig& board_config();

}  // namespace hgp
