#include "stc8_battery.hpp"

#include <array>

namespace hg {
namespace {

uint32_t read_u32_le(const std::array<uint8_t, Stc8Battery::kBatteryInfoBytes>& raw, size_t offset) {
  return static_cast<uint32_t>(raw[offset]) |
         (static_cast<uint32_t>(raw[offset + 1]) << 8) |
         (static_cast<uint32_t>(raw[offset + 2]) << 16) |
         (static_cast<uint32_t>(raw[offset + 3]) << 24);
}

}  // namespace

std::optional<PowerStatus> Stc8Battery::read() {
  if (!read_byte_) return std::nullopt;

  std::array<uint8_t, kBatteryInfoBytes> raw{};
  for (uint8_t reg = 0; reg < kBatteryInfoBytes; ++reg) {
    if (!read_byte_(reg, raw[reg])) return std::nullopt;
  }

  constexpr uint32_t kMaxReportedVoltageMv = 5000;
  PowerStatus status;
  status.battery_registers = raw;
  const uint32_t adc_mv = read_u32_le(raw, 0);
  const uint32_t battery_mv = read_u32_le(raw, 4);
  if (adc_mv <= kMaxReportedVoltageMv) status.battery_adc_mv = static_cast<uint16_t>(adc_mv);
  if (battery_mv <= kMaxReportedVoltageMv) status.battery_mv = static_cast<uint16_t>(battery_mv);
  if (raw[8] <= 100) status.battery_percent = raw[8];
  if (raw[9] <= static_cast<uint8_t>(BatteryChargeState::Error)) {
    const auto state = static_cast<BatteryChargeState>(raw[9]);
    status.battery_state = state;
    if (state != BatteryChargeState::Error)
      status.charging = state == BatteryChargeState::Charging;
  }
  if (raw[10] <= static_cast<uint8_t>(BatteryLedState::LowPower))
    status.battery_led_state = static_cast<BatteryLedState>(raw[10]);
  // raw[11] is the C struct tail-padding byte, not a declared battery field.
  return status;
}

}  // namespace hg
