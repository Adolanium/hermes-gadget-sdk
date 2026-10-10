#include "axp2101.hpp"

namespace hg {

// Battery percent from voltage for when the fuel gauge reports an invalid
// value. This board never initializes the gauge, so it can read 0 on a full
// battery; the voltage (0x34/0x35) is the honest signal. Linear Li-ion map.
static uint8_t millivolts_to_percent(uint16_t mv) {
  constexpr uint16_t kEmptyMv = 3300, kFullMv = 4200;
  if (mv <= kEmptyMv) return 0;
  if (mv >= kFullMv) return 100;
  return static_cast<uint8_t>(static_cast<uint32_t>(mv - kEmptyMv) * 100U / (kFullMv - kEmptyMv));
}

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
    std::optional<uint8_t> gauge;
    if (enable & 0x08) {
      // Trust the gauge only in its valid range. A gauge that reads 0 (never
      // initialized on this board) must not read as an empty battery when the
      // voltage says otherwise; derive percent from the voltage instead.
      uint8_t percent = 0;
      if (!read_(0xa4, &percent, 1)) return std::nullopt;  // a failed read is unavailable, not an estimate
      if (percent >= 1 && percent <= 100) gauge = percent;
    }
    if (percent_ && out.battery_mv) {
      // Status 2 (0x01) bits 2:0: 100 charge done; bits 6:5 01 charging.
      using Charge = BatteryPercent::Charge;
      const bool done = *out.external_power && (status[1] & 0x07) == 0x04;
      const Charge charge = done ? Charge::Done : *out.charging ? Charge::Charging : Charge::Discharging;
      out.battery_percent = percent_->update(*out.battery_mv, charge, gauge);
    } else if (enable & 0x08) {
      if (gauge)
        out.battery_percent = gauge;
      else if (out.battery_mv)
        out.battery_percent = millivolts_to_percent(*out.battery_mv);
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

// IRQ enable 1 (0x41) and IRQ status 1 (0x49), bit 3: PWRON short press.
// Status bits clear when written back as 1.
constexpr uint8_t kKeyShortPress = 0x08;

bool Axp2101::enable_key_press() {
  uint8_t enabled;
  return read_(0x41, &enabled, 1) && write_(0x41, static_cast<uint8_t>(enabled | kKeyShortPress)) &&
         write_(0x49, kKeyShortPress);
}

std::optional<uint16_t> Axp2101::charge_current_ma() {
  uint8_t reg;
  if (!read_(0x62, &reg, 1)) return std::nullopt;
  const unsigned n = reg & 0x1f;  // 25 mA steps to 200 mA, then 100 mA steps
  if (n > 21) return std::nullopt;  // reserved
  return static_cast<uint16_t>(n <= 8 ? 25 * n : 200 + 100 * (n - 8));
}

std::optional<bool> Axp2101::vbus_good() {
  uint8_t status;
  if (!read_(0x00, &status, 1)) return std::nullopt;
  return (status & 0x20) != 0;
}

bool Axp2101::take_key_press() {
  uint8_t status;
  if (!read_(0x49, &status, 1) || !(status & kKeyShortPress)) return false;
  write_(0x49, kKeyShortPress);
  return true;
}

}  // namespace hg
