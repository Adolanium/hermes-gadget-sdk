#include "axp2101.hpp"

namespace hg {

std::optional<PowerStatus> Axp2101::read() {
  uint8_t status[2], enable = 0, adc = 0;
  if (!read_(0x00, status, 2) || !read_(0x18, &enable, 1) || !read_(0x30, &adc, 1)) return std::nullopt;
  PowerStatus out;
  out.battery_present = (status[0] & 0x08) != 0;
  out.external_power = (status[0] & 0x20) != 0;
  out.charging = *out.battery_present && (status[1] & 0x60) == 0x20;
  if (*out.battery_present) {
    if (adc & 0x01) {
      uint8_t voltage[2];
      if (!read_(0x34, voltage, 2)) return std::nullopt;
      const uint16_t mv = static_cast<uint16_t>(((voltage[0] & 0x3f) << 8) | voltage[1]);
      if (mv >= 2000 && mv <= 5000) out.battery_mv = mv;
    }
    if (enable & 0x08) {
      uint8_t percent;
      if (!read_(0xa4, &percent, 1)) return std::nullopt;
      if (percent <= 100) out.battery_percent = percent;
    }
  }
  return out;
}

bool Axp2101::power_off() {
  uint8_t config;
  return read_(0x10, &config, 1) && write_(0x10, static_cast<uint8_t>((config & ~0x02) | 0x01));
}

bool Axp2101::enable_aldo1_3v3() {
  uint8_t voltage, enabled;
  if (!read_(0x92, &voltage, 1) || !read_(0x90, &enabled, 1)) return false;
  // ALDO1: 500 mV + 100 mV per step. Preserve the other rail controls.
  return write_(0x92, static_cast<uint8_t>((voltage & 0xe0) | 28)) &&
         write_(0x90, static_cast<uint8_t>(enabled | 0x01));
}

}  // namespace hg
