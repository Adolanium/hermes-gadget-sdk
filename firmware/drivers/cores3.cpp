#include "cores3.hpp"

#include <algorithm>

namespace hg {
namespace {
constexpr uint8_t kPmic = 0x34, kExpander = 0x58;
}

bool CoreS3Control::change(uint8_t address, uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t previous;
  return read_(address, reg, previous) &&
         write_(address, reg, static_cast<uint8_t>((previous & ~mask) | (value & mask)));
}

bool CoreS3Control::begin() {
  // ALDO1 powers the amplifier logic at 1.8 V; ALDO2 powers microphones at
  // 3.3 V. Camera, SD, charger and external USB/bus-output settings stay intact.
  if (!change(kPmic, 0x92, 0x1f, 13) || !change(kPmic, 0x93, 0x1f, 28) ||
      !change(kPmic, 0x90, 0x03, 0x03)) return false;
  // Configure just touch/amp reset (P0_0/P0_2), LCD reset (P1_1), and
  // the onboard 5 V boost enable (P1_7). Keep reset low while switching modes.
  if (!change(kExpander, 0x02, 0x05, 0x00) || !change(kExpander, 0x03, 0x82, 0x80) ||
      !change(kExpander, 0x12, 0x05, 0x05) || !change(kExpander, 0x13, 0x82, 0x82) ||
      !change(kExpander, 0x11, 0x10, 0x10) ||
      !change(kExpander, 0x04, 0x05, 0x00) || !change(kExpander, 0x05, 0x82, 0x00)) return false;
  delay_(10);
  if (!change(kExpander, 0x02, 0x05, 0x05) || !change(kExpander, 0x03, 0x02, 0x02)) return false;
  delay_(300);
  return set_brightness(0);
}

bool CoreS3Control::set_brightness(uint8_t percent) {
  // DLDO1's 2.5–3.3 V range controls the backlight. Zero disables this rail.
  if (!percent) return change(kPmic, 0x90, 0x80, 0);
  const uint8_t code = static_cast<uint8_t>(20 + (std::min<uint8_t>(percent, 100) * 8) / 100);
  return change(kPmic, 0x99, 0x1f, code) && change(kPmic, 0x90, 0x80, 0x80);
}

}  // namespace hg
