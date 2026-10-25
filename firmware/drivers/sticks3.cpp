#include "sticks3.hpp"

namespace hg {
namespace {
// M5PM1 registers, from M5GFX/M5Unified's StickS3 bring-up.
constexpr uint8_t kGpioMode = 0x10;      // 1 = output
constexpr uint8_t kGpioOut = 0x11;       // output level
constexpr uint8_t kGpioDrive = 0x13;     // 0 = push-pull
constexpr uint8_t kGpioFunction = 0x16;  // 0 = plain GPIO
constexpr uint8_t kI2cConfig = 0x09;     // 0 = no I2C idle sleep
constexpr uint8_t kLcdRail = 1 << 2, kAmplifier = 1 << 3;
}  // namespace

bool StickS3Control::change(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t previous;
  return read_(reg, previous) && write_(reg, static_cast<uint8_t>((previous & ~mask) | (value & mask)));
}

bool StickS3Control::begin() {
  // The PM1 stays powered through a battery shutdown, so an earlier firmware
  // may have left I2C idle sleep on; it then misses later accesses.
  if (!write_(kI2cConfig, 0x00)) return false;
  // Set the levels before switching the pins to outputs: LCD rail on,
  // amplifier off. The PM1's other pins keep their configuration.
  constexpr uint8_t pins = kLcdRail | kAmplifier;
  if (!change(kGpioFunction, pins, 0) || !change(kGpioDrive, pins, 0) ||
      !change(kGpioOut, pins, kLcdRail) || !change(kGpioMode, pins, pins)) return false;
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

}  // namespace hg
