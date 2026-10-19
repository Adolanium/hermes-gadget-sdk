#include "stc8_battery.hpp"

#include <array>

// Register offsets and status codes are hardware facts from Elecrow V1.1 config.h
// at pinned commit 6c7c851f3105349dc24b12ad058655ba6afa98ee; this is an
// independent implementation, not copied vendor source.

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
  constexpr uint8_t kChargingState = 1;
  constexpr uint8_t kErrorState = 4;
  PowerStatus status;
  const uint32_t battery_mv = read_u32_le(raw, 4);
  if (battery_mv <= kMaxReportedVoltageMv) status.battery_mv = static_cast<uint16_t>(battery_mv);
  if (raw[8] <= 100) status.battery_percent = raw[8];
  // The STC8 error code is not evidence that the pack is not charging.
  if (raw[9] < kErrorState) status.charging = raw[9] == kChargingState;
  return status;
}

}  // namespace hg
