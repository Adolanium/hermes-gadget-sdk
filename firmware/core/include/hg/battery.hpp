#pragma once

#include <cstddef>
#include <cstdint>

namespace hg {

// Uncalibrated linear single-cell Li-ion estimate; not a fuel gauge.
inline uint8_t estimate_liion_percent(uint16_t mv) {
  constexpr uint16_t kEmptyMv = 3300, kFullMv = 4200;
  if (mv <= kEmptyMv) return 0;
  if (mv >= kFullMv) return 100;
  return static_cast<uint8_t>(static_cast<uint32_t>(mv - kEmptyMv) * 100U / (kFullMv - kEmptyMv));
}

// Uncalibrated single-cell Li-ion estimate from a typical resting-voltage curve,
// linear between points and floored: a cell spends most of its charge between
// 3.7 and 4.0 V, where a straight 3.3..4.2 V line reads high. Not a fuel gauge,
// and it reads high while charging.
inline uint8_t estimate_liion_curve_percent(uint16_t mv) {
  struct Point { uint16_t mv; uint8_t percent; };
  static constexpr Point kCurve[] = {{3300, 0},  {3610, 5},  {3690, 10}, {3730, 20}, {3770, 30}, {3800, 40},
                                     {3840, 50}, {3870, 60}, {3950, 70}, {4020, 80}, {4110, 90}, {4200, 100}};
  constexpr size_t kPoints = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[kPoints - 1].mv) return 100;
  for (size_t i = 1; i < kPoints; ++i) {
    if (mv < kCurve[i].mv) {
      const Point lo = kCurve[i - 1], hi = kCurve[i];
      return static_cast<uint8_t>(lo.percent + static_cast<uint32_t>(mv - lo.mv) * (hi.percent - lo.percent) /
                                                   (hi.mv - lo.mv));
    }
  }
  return 100;
}

}  // namespace hg
