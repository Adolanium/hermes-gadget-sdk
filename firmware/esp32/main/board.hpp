// Board description: which peripherals exist and how they are wired.
//
// Select the board's pins and drivers in board.cpp through the Kconfig choice.
// Board-specific power/reset sequencing runs before peripheral initialization.
// See docs/porting.md for the display, audio, input and power contracts.
#pragma once

#include <cstdint>

#include "battery_curve.hpp"

namespace hgp {

// How a parallel panel is wired. An I80 panel uses eight data lines and a write
// strobe here; an RGB panel is wired in LcdConfig::rgb.
struct LcdBus {
  enum class Type : uint8_t { Spi, I80, Rgb };
  Type type = Type::Spi;
  int data[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int wr = -1;
  int pclk_mhz = 16;
};

// A 16-bit RGB panel that is scanned out continuously. Two bring-up styles
// exist in the CrowPanel family: the 2.1-inch ST7701 takes its init commands
// over a separate 3-wire SPI and is powered/reset through a PCF8574 expander,
// while the 5.0-inch ILI6122 needs no command sequence, expander or enable
// line. Set `controller` to pick the bring-up; the scanout contract is
// identical for both.
enum class RgbController { St7701, Ili6122 };

// RGB scanout timings. Each CrowPanel's datasheet differs, so the values live
// in the board config rather than being hardcoded in the driver.
struct RgbTimings {
  uint16_t hsync_pulse_width = 4, hsync_back_porch = 20, hsync_front_porch = 10;
  uint16_t vsync_pulse_width = 4, vsync_back_porch = 20, vsync_front_porch = 10;
  bool hsync_idle_low = false, vsync_idle_low = false;  // vendor polarity: false = idle high
  bool de_idle_high = false;
  bool pclk_active_neg = false;
  bool pclk_idle_high = false;
};

struct RgbPanelConfig {
  int de = -1, vsync = -1, hsync = -1, pclk = -1;
  int data[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  int cmd_cs = -1, cmd_sclk = -1, cmd_sda = -1;  // 3-wire SPI command bus (ST7701)
  int i2c_expander = -1;  // PCF8574 address; -1 when no expander is fitted
  // True when data[0..4] carry red (the 2.1's wiring), so flush() swaps red and
  // blue; false when they carry blue, which is esp_lcd's native RGB565.
  bool swap_red_blue = true;
  uint32_t pclk_hz = 12000000;
  RgbController controller = RgbController::St7701;
  RgbTimings timings{};   // panel scanout timings (per-datasheet)
};

enum class LcdController { St7789, Box3, CoreS3, St77916 };

struct LcdConfig {
  bool enabled = false;
  uint16_t width = 320, height = 240;  // after rotation
  bool swap_xy = true, mirror_x = true, mirror_y = false, invert = true;
  int gap_x = 0, gap_y = 0;
  int mosi = -1, sclk = -1, cs = -1, dc = -1, rst = -1, backlight = -1;
  int spi_mhz = 40;
  LcdBus bus{};
  RgbPanelConfig rgb{};
  LcdController controller = LcdController::St7789;
  bool reset_active_high = false;
  bool backlight_invert = false;  // active-low GPIO backlight, independent of pixel inversion
  bool bgr = false;  // the panel's color filter is BGR (SpiDisplay)
  int d1 = -1, d2 = -1, d3 = -1;  // QSPI data0 uses mosi
  bool round = false;
};

struct I2sMicConfig {
  bool enabled = false;
  int sck = -1, ws = -1, sd = -1;
};

struct I2sSpeakerConfig {
  bool enabled = false;
  int bclk = -1, ws = -1, dout = -1;
};

// Which vendor bring-up table a CO5300 panel needs. The round 466x466 1.75"
// modules write extra page-2 registers; the rectangular 368x448 1.8" module uses
// the shorter table from Waveshare's board support package.
enum class AmoledPanel { Co5300_466, Co5300_368 };

// QSPI AMOLED with a CO5300 controller (round 466x466 or rectangular 368x448).
struct AmoledConfig {
  bool enabled = false;
  uint16_t width = 466, height = 466;
  int cs = -1, sclk = -1, d0 = -1, d1 = -1, d2 = -1, d3 = -1, rst = -1;
  int gap_x = 0, gap_y = 0;  // the controller's RAM is wider than the glass
  int qspi_mhz = 40;
  bool round = false;
  uint8_t corner_inset = 0;  // extra top-bar padding for rounded corners (hg::DisplayInfo)
  AmoledPanel panel = AmoledPanel::Co5300_466;
};

struct I2cBusConfig {
  int sda = -1, scl = -1;
  uint32_t hz = 400000;
};

// ES8311/AW88298 (speaker) and the microphone ADC sharing one duplex I2S bus,
// controlled over the I2C bus.
enum class SpeakerCodec { Es8311, Aw88298 };

// The microphone ADC: an ES7210 for MEMS microphones, or the ES8311's own ADC
// when the board wires an analog microphone to the speaker codec.
enum class MicCodec { Es7210, Es8311 };

struct CodecAudioConfig {
  bool enabled = false;
  int mclk = -1, bclk = -1, ws = -1, dout = -1, din = -1;
  int pa = -1;               // speaker amplifier enable, active high
  float amp_supply_v = 5.0f;  // amplifier supply; the ES8311 driver sets its output level from it
  float mic_gain_db = 24.0f;
  SpeakerCodec speaker = SpeakerCodec::Es8311;
  MicCodec mic = MicCodec::Es7210;
  bool stereo32 = false;      // two 32-bit I2S slots per frame instead of one 16-bit slot
  bool rmnm_mics = false;     // the ES7210 sends four 16-bit channels R M N M; average the M slots (ws185.hpp)
  bool speaker_pa = false;    // CodecSpeaker drives `pa`, not esp_codec_dev (see CodecAudio::begin)
  bool dac_mclk = true;       // false: the ES8311 derives its clock from BCLK
  uint8_t es7210_mics = 0x03;  // ES7210_SEL_MIC1 | ES7210_SEL_MIC2
};

// Capacitive touch on the I2C bus: hold to talk, tap, swipe down to cancel.
enum class TouchController { Cst9217, Box3, Ft5x06, Cst816 };

struct TouchConfig {
  bool enabled = false;
  uint8_t addr = 0x5A;
  int rst = -1;
  int int_gpio = -1;  // touch INT line; -1 when not wired (driver polls instead)
  uint16_t width = 0, height = 0;
  bool mirror_x = false, mirror_y = false;
  int16_t offset_y = 0;  // subtracted from y when the UI starts below the panel's first row
  TouchController controller = TouchController::Cst9217;
};

// A key whose level is read from a TCA9554 or PCF8574 I/O expander input (e.g.
// a PMIC's power key). Acts as CANCEL: a press cancels, holding 2 s starts a
// new session.
struct ExpanderKeyConfig {
  bool enabled = false;
  uint8_t addr = 0x20;
  uint8_t bit = 0;
  bool active_high = true;
  bool pcf8574 = false;  // a PCF8574 has no registers: read its port directly
};

// Reset lines on a TCA9554 I/O expander, pulsed before the display and touch
// start. Only the bits in `mask` change.
struct ExpanderResetConfig {
  uint8_t addr = 0x20;
  uint8_t mask = 0;  // 0 = no expander resets
  uint16_t hold_ms = 10, settle_ms = 50;  // low pulse, then the wait before the display starts
};

struct ButtonConfig {
  int talk = -1, cancel = -1, up = -1, down = -1;  // active-low GPIOs, -1 = absent
};

// A quadrature rotary encoder on two GPIOs. Each detent is an Up or Down press.
struct EncoderConfig {
  int a = -1, b = -1;  // -1 = absent
};

// A battery behind a resistive divider, with a latch that keeps it powered.
struct LatchPowerConfig {
  bool enabled = false;
  int adc = -1, enable = -1, charging = -1;  // adc -1: no battery voltage reading
  int backlight = -1;
  // VBAT = VADC * ratio. Waveshare's LCD-1.54 divides by three; the T-Display-S3
  // divides by two. Ignored when the ADC pin is -1.
  int mv_ratio = 3;
  uint16_t max_battery_mv = 4998;
  bool power_off_supported = true;
};

struct BoardConfig {
  const char* name;
  LcdConfig lcd;
  I2sMicConfig mic;
  I2sSpeakerConfig speaker;
  ButtonConfig buttons;
  EncoderConfig encoder;
  AmoledConfig amoled;
  I2cBusConfig i2c;
  CodecAudioConfig codec;
  TouchConfig touch;
  ExpanderKeyConfig pwr_key;
  ExpanderResetConfig expander_reset;
  bool axp2101 = false;
  bool axp_audio_supply = false;
  bool axp_power_key = false;  // PWR reaches only the AXP2101: read its short press there
  // The battery's measured discharge and charge curves (drivers/battery_curve.hpp).
  hg::Curve battery_curve, battery_charge_curve;
  bool cores3 = false;
  LatchPowerConfig latch_power;
  int status_led = -1;
  const char* talk_label = "TALK";
  const char* cancel_label = "CANCEL";
};

// GPIO held high to power the panel's peripheral rail. Boards without one
// leave it -1; the driver must see it high before it initialises the panel.
int lcd_power_pin(const BoardConfig& b);

const BoardConfig& board_config();
// "HGBOARD=<board name>", the tag every image carries.
const char* board_tag();

}  // namespace hgp
