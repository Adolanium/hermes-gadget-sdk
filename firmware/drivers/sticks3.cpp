#include "sticks3.hpp"

#include "hg/battery.hpp"

namespace hg {
namespace {
// M5PM1 registers, from M5Unified's M5PM1 driver and StickS3 bring-up.
constexpr uint8_t kPowerSource = 0x04;   // bit 0: VIN (USB), bit 2: battery
constexpr uint8_t kI2cConfig = 0x09;     // 0 = no I2C idle sleep
constexpr uint8_t kWatchdog = 0x0A;      // 0 = watchdog off
constexpr uint8_t kSystemCommand = 0x0C;
constexpr uint8_t kGpioMode = 0x10;      // 1 bit per pin, 1 = output
constexpr uint8_t kGpioOut = 0x11;       // output level
constexpr uint8_t kGpioIn = 0x12;        // input level
constexpr uint8_t kGpioDrive = 0x13;     // 1 bit per pin, 0 = push-pull
constexpr uint8_t kGpioFunction = 0x16;  // 2 bits per pin for GPIO0-3, 0 = plain GPIO
constexpr uint8_t kBatteryMv = 0x22;     // 16-bit little-endian millivolts

constexpr uint8_t kShutdown = 0xA1;
constexpr uint8_t kFromUsb = 1 << 0;
constexpr uint8_t kChargeStatus = 1 << 0, kLcdRail = 1 << 2, kAmplifier = 1 << 3;  // PM1 GPIO0, 2, 3
constexpr uint8_t kFunctionFields = 0x03 | 0x30 | 0xC0;                             // GPIO0, 2, 3
// A reading outside a single Li-ion cell's range means the gauge has nothing to measure.
constexpr uint16_t kMinCellMv = 2600, kMaxCellMv = 4450;
}  // namespace

bool StickS3Control::change(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t previous;
  return read_(reg, &previous, 1) && write_(reg, static_cast<uint8_t>((previous & ~mask) | (value & mask)));
}

bool StickS3Control::begin() {
  // The PM1 stays powered through a battery shutdown, so an earlier firmware
  // may have left I2C idle sleep or its watchdog on.
  if (!write_(kI2cConfig, 0x00) || !write_(kWatchdog, 0x00)) return false;
  // Set the levels before switching the pins to outputs: LCD rail on,
  // amplifier off. GPIO0 becomes an input for the charge status. The PM1's
  // other pins keep their configuration.
  constexpr uint8_t outputs = kLcdRail | kAmplifier;
  if (!change(kGpioFunction, kFunctionFields, 0) || !change(kGpioDrive, outputs, 0) ||
      !change(kGpioOut, outputs, kLcdRail) || !change(kGpioMode, outputs | kChargeStatus, outputs)) return false;
  amplifier_ = false;
  delay_(100);  // the panel needs its rail settled before reset
  return true;
}

bool StickS3Control::set_amplifier(bool on) {
  if (on == amplifier_) return true;
  if (!change(kGpioOut, kAmplifier, on ? kAmplifier : 0)) return false;
  amplifier_ = on;
  return true;
}

std::optional<bool> StickS3Control::external_power() {
  uint8_t source;
  if (!read_(kPowerSource, &source, 1)) return std::nullopt;
  return (source & kFromUsb) != 0;
}

std::optional<PowerStatus> StickS3Control::read() {
  uint8_t source, pins, mv_raw[2];
  if (!read_(kPowerSource, &source, 1) || !read_(kGpioIn, &pins, 1) || !read_(kBatteryMv, mv_raw, 2))
    return std::nullopt;
  PowerStatus out;
  const bool usb = source & kFromUsb;
  out.external_power = usb;
  const auto mv = static_cast<uint16_t>(mv_raw[0] | (mv_raw[1] << 8));
  out.battery_present = mv >= kMinCellMv && mv <= kMaxCellMv;
  if (*out.battery_present) {
    out.battery_mv = mv;
    out.battery_percent = estimate_liion_curve_percent(mv);
    // The charger pulls its status low only while current flows.
    out.charging = usb && !(pins & kChargeStatus);
  }
  return out;
}

bool StickS3Control::power_off() {
  set_amplifier(false);
  return write_(kSystemCommand, kShutdown);
}

}  // namespace hg
