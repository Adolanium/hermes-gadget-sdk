#pragma once

#include <cstdint>

namespace hg {

// Uncalibrated linear single-cell Li-ion estimate; not a fuel gauge.
inline uint8_t estimate_liion_percent(uint16_t mv) {
  constexpr uint16_t kEmptyMv = 3300, kFullMv = 4200;
  if (mv <= kEmptyMv) return 0;
  if (mv >= kFullMv) return 100;
  return static_cast<uint8_t>(static_cast<uint32_t>(mv - kEmptyMv) * 100U / (kFullMv - kEmptyMv));
}

}  // namespace hg
