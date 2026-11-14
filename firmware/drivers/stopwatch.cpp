#include "stopwatch.hpp"

#include <algorithm>

namespace hg {
namespace {
// M5PM1 registers, from M5Stack's M5PM1 driver (M5PM1.h, commit 315d2899).
constexpr uint8_t kI2cConfig = 0x09;      // 0 = no I2C idle sleep
constexpr uint8_t kWatchdog = 0x0A;       // 0 = watchdog off
constexpr uint8_t kSystemCommand = 0x0C;
constexpr uint8_t kGpioIn = 0x12;         // input level, 1 bit per pin
constexpr uint8_t kBatteryMv = 0x22;      // millivolts, little-endian
constexpr uint8_t kVinMv = 0x24;          // millivolts, little-endian
constexpr uint8_t kIrqMask1 = 0x43, kIrqMask2 = 0x44, kIrqMask3 = 0x45;  // 1 = masked
constexpr uint8_t kButtonStatus = 0x48;   // bit 0: down now; bit 7: was pressed, cleared by the read
constexpr uint8_t kButtonConfig = 0x49;

constexpr uint8_t kShutdown = 0xA1;
constexpr uint8_t kChargeStatus = 1 << 2;  // PMG2, CHG_STAT: low while charge current flows
constexpr uint8_t kButtonDown = 1 << 0, kButtonWasPressed = 1 << 7;
// A single click must not reset the ESP32, and the long press that cuts power takes 4 s.
constexpr uint8_t kNoClickReset = 1 << 0, kLongPressMask = 0x18, kLongPress4s = 0x18;
// A reading outside a single Li-ion cell's range means the gauge has nothing to measure.
constexpr uint16_t kMinCellMv = 2600, kMaxCellMv = 4450;
constexpr uint16_t kVinPresentMv = 4000;
constexpr uint8_t kShortPressPolls = 10;  // the app polls every 100 ms: under a second

// M5IOE1 registers, 16 bits each, and its pins (M5Stack's labels PYG1.. are bits 0..).
constexpr uint8_t kIoeMode = 0x03;       // 1 = output
constexpr uint8_t kIoeOut = 0x05;
constexpr uint8_t kIoeOpenDrain = 0x13;  // 0 = push-pull
constexpr uint8_t kIoePwm1 = 0x1B;       // the vibration motor's duty; 0 = off
constexpr uint8_t kIoeI2cConfig = 0x23;  // 0 = no I2C idle sleep
constexpr uint16_t kAudioSupply = 1 << 2;  // PYG3, AU_EN: codec and microphone
constexpr uint16_t kTouchReset = 1 << 3;   // PYG4, active low
constexpr uint16_t kPanelReset = 1 << 4;   // PYG5, active low
constexpr uint16_t kPanelSupply = 1 << 7;  // PYG8, L3B_EN
constexpr uint16_t kMotor = 1 << 8;        // PYG9
constexpr uint16_t kAmplifier = 1 << 9;    // PYG10, SPK_EN
constexpr uint16_t kOutputs = kAudioSupply | kTouchReset | kPanelReset | kPanelSupply | kMotor | kAmplifier;

// M5PM1.h calls the second byte "the high 4 bits", but a unit reads 4174 mV for its cell and
// 5082 mV for VIN, which 12 bits cannot hold: the value is 16 bits, little-endian.
uint16_t millivolts(const uint8_t* raw) { return static_cast<uint16_t>(raw[0] | (raw[1] << 8)); }
}  // namespace

bool StopWatchControl::pm1_change(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t previous;
  if (!pm1_.read(reg, &previous, 1)) return false;
  const auto next = static_cast<uint8_t>((previous & ~mask) | (value & mask));
  return next == previous || pm1_.write(reg, next);
}

bool StopWatchControl::ioe_change(uint8_t reg, uint16_t mask, uint16_t value) {
  uint8_t raw[2];
  if (!ioe_read_(reg, raw, 2)) return false;
  const auto previous = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
  const auto next = static_cast<uint16_t>((previous & ~mask) | (value & mask));
  return next == previous || ioe_write_(reg, next);
}

bool StopWatchControl::begin() {
  // Both chips stay powered through an ESP32 reset, so an earlier firmware may
  // have left their I2C idle sleep or the PM1's watchdog on.
  if (!pm1_.write(kI2cConfig, 0x00) || !pm1_.write(kWatchdog, 0x00)) return false;
  if (!pm1_change(kButtonConfig, kNoClickReset | kLongPressMask, kNoClickReset | kLongPress4s)) return false;
  // The key is polled (take_key_press), so nothing may pull the PM1's interrupt line.
  if (!pm1_.write(kIrqMask1, 0x1F) || !pm1_.write(kIrqMask2, 0x3F) || !pm1_.write(kIrqMask3, 0x07)) return false;

  // Levels before directions: supplies on, both resets released, motor and
  // amplifier off. The expander's other pins keep their configuration.
  if (!ioe_write_(kIoeI2cConfig, 0x0000) || !ioe_write_(kIoePwm1, 0x0000)) return false;
  if (!ioe_change(kIoeOpenDrain, kOutputs, 0) ||
      !ioe_change(kIoeOut, kOutputs, kAudioSupply | kPanelSupply | kTouchReset | kPanelReset) ||
      !ioe_change(kIoeMode, kOutputs, kOutputs))
    return false;
  amplifier_ = false;
  delay_(20);  // the panel's rail settles before its reset
  if (!ioe_change(kIoeOut, kTouchReset | kPanelReset, 0)) return false;
  delay_(10);
  if (!ioe_change(kIoeOut, kTouchReset | kPanelReset, kTouchReset | kPanelReset)) return false;
  delay_(120);  // the CO5300 wakes
  return true;
}

bool StopWatchControl::set_amplifier(bool on) {
  if (on == amplifier_) return true;
  if (!ioe_change(kIoeOut, kAmplifier, on ? kAmplifier : 0)) return false;
  amplifier_ = on;
  return true;
}

std::optional<bool> StopWatchControl::external_power() {
  uint8_t raw[2];
  if (!pm1_.read(kVinMv, raw, 2)) return std::nullopt;
  return millivolts(raw) > kVinPresentMv;
}

std::optional<PowerStatus> StopWatchControl::read() {
  uint8_t cell[2], vin[2], pins;
  if (!pm1_.read(kBatteryMv, cell, 2) || !pm1_.read(kVinMv, vin, 2) || !pm1_.read(kGpioIn, &pins, 1))
    return std::nullopt;
  PowerStatus out;
  const bool usb = millivolts(vin) > kVinPresentMv;
  out.external_power = usb;
  const uint16_t mv = millivolts(cell);
  out.battery_present = mv >= kMinCellMv && mv <= kMaxCellMv;
  if (*out.battery_present) {
    out.battery_mv = mv;
    // No percentage: this cell's discharge curve has not been measured.
    out.charging = usb && !(pins & kChargeStatus);
  }
  return out;
}

bool StopWatchControl::take_key_press() {
  uint8_t status;
  if (!pm1_.read(kButtonStatus, &status, 1)) return false;
  const bool down = status & kButtonDown;
  if (down) {
    key_down_ = true;
    key_polls_ = static_cast<uint8_t>(std::min<int>(key_polls_ + 1, 255));
    return false;
  }
  // Up now. A press that began and ended between two polls shows only in the flag.
  const bool pressed = key_down_ ? key_polls_ < kShortPressPolls : (status & kButtonWasPressed) != 0;
  key_down_ = false;
  key_polls_ = 0;
  return pressed;
}

bool StopWatchControl::power_off() {
  set_amplifier(false);
  return pm1_.write(kSystemCommand, kShutdown);
}

}  // namespace hg
