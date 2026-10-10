// Battery percent from a measured discharge curve, apart from the PMIC driver so
// it can be tested on the host. The AXP2101's fuel gauge reads low on some cells
// (the AMOLED-1.75C's 500 mAh cell: 18% at 31% left), and a straight voltage line
// is worse; a curve measured on the board's own cell is close.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hg {

struct CurvePoint {
  uint16_t mv;
  uint8_t percent;
};

// Points ordered from full to empty (falling voltage and percent). Linear between
// points; above the first point 100, below the last 0.
struct Curve {
  const CurvePoint* points = nullptr;
  size_t size = 0;
  explicit operator bool() const { return points && size >= 2; }
};

inline uint8_t percent_on_curve(Curve curve, uint16_t mv) {
  const CurvePoint* c = curve.points;
  const size_t n = curve.size;
  if (mv >= c[0].mv) return c[0].percent;
  if (mv <= c[n - 1].mv) return c[n - 1].percent;
  for (size_t i = 1; i < n; ++i) {
    const CurvePoint hi = c[i - 1], lo = c[i];
    if (mv >= lo.mv) {
      const uint32_t span = hi.mv - lo.mv, into = mv - lo.mv;
      return static_cast<uint8_t>(lo.percent + ((hi.percent - lo.percent) * into + span / 2) / span);
    }
  }
  return c[n - 1].percent;
}

// The AMOLED-1.75C's 500 mAh cell: one full discharge at about 93 mA (screen
// off, Wi-Fi on), percent = share of the time left until shutdown. 80-95% had no
// readings (the device was offline) and come from a quadratic fit to the rest.
inline constexpr CurvePoint kAmoled175cPoints[] = {
    {4100, 100}, {4090, 99}, {4056, 95}, {4015, 90}, {3974, 85}, {3934, 80}, {3873, 75}, {3813, 63},
    {3754, 55},  {3704, 49}, {3658, 43}, {3624, 37}, {3594, 31}, {3571, 27}, {3530, 21}, {3508, 17},
    {3480, 13},  {3441, 9},  {3408, 7},  {3368, 5},  {3294, 3},  {3157, 1},  {3050, 0},
};
inline constexpr Curve kAmoled175cCurve{kAmoled175cPoints, sizeof(kAmoled175cPoints) / sizeof(kAmoled175cPoints[0])};

// The same cell charging from empty at the PMIC's constant current (200 mA on
// the measured board; the datasheet lists the reset value as factory-set): percent =
// share of the time to "charge done" (the current only tapered in the last 7 of
// 152 minutes, so charge grew with time). Tops out at 99 until the PMIC reports
// charge done. Checked against a second charge it wasn't fitted to: within 1 point
// on average.
inline constexpr CurvePoint kAmoled175cChargePoints[] = {
    {4200, 99}, {4192, 98}, {4173, 94}, {4161, 90}, {4140, 86}, {4111, 82}, {4076, 78}, {4044, 74},
    {4016, 70}, {3991, 66}, {3966, 62}, {3937, 58}, {3904, 54}, {3868, 50}, {3835, 46}, {3804, 42},
    {3778, 38}, {3754, 34}, {3733, 30}, {3715, 26}, {3698, 22}, {3665, 18}, {3641, 14}, {3613, 10}, {3581, 6},
    {3466, 2}, {3316, 0},
};
inline constexpr Curve kAmoled175cChargeCurve{kAmoled175cChargePoints,
                                              sizeof(kAmoled175cChargePoints) / sizeof(kAmoled175cChargePoints[0])};

// What the device shows, from readings every few seconds, with the median of the
// last kWindow voltages (about a minute) on the curve for the direction of flow:
// - on battery, the discharge curve, never rising: a lighter load lifts the
//   voltage, not the charge;
// - charging, the charge curve (or, without one, the PMIC's gauge), never falling
//   and at most 99 (the curve's top); it starts no lower than what battery showed before plugging in,
//   while the voltage is still climbing;
// - charge done, 100.
class BatteryPercent {
 public:
  static constexpr size_t kWindow = 12;
  explicit BatteryPercent(Curve discharge, Curve charge = {}) : discharge_(discharge), charge_(charge) {}

  enum class Charge { Discharging, Charging, Done };

  uint8_t update(uint16_t mv, Charge charge, std::optional<uint8_t> gauge) {
    if (charge == Charge::Done) {
      flow_ = Flow::None;
      shown_.reset();
      return 100;
    }
    const Flow flow = charge == Charge::Charging ? Flow::In : Flow::Out;
    if (flow == Flow::In && !charge_) {
      flow_ = Flow::None;  // no charge curve: the gauge, then a fresh window on battery
      shown_.reset();
      return gauge ? *gauge : percent_on_curve(discharge_, mv);
    }
    if (flow != flow_) {
      // Plugging in keeps what battery showed as a floor; unplugging starts afresh.
      if (!(flow == Flow::In && flow_ == Flow::Out)) shown_.reset();
      flow_ = flow;
      count_ = 0;
    }
    window_[next_] = mv;
    next_ = (next_ + 1) % kWindow;
    if (count_ < kWindow) ++count_;
    std::array<uint16_t, kWindow> sorted{};
    for (size_t i = 0; i < count_; ++i) sorted[i] = window_[(next_ + kWindow - count_ + i) % kWindow];
    std::sort(sorted.begin(), sorted.begin() + count_);
    const uint16_t median = sorted[count_ / 2];
    if (flow == Flow::In) {
      const uint8_t now = percent_on_curve(charge_, median);  // charge curves top out at 99
      shown_ = shown_ ? std::max(*shown_, now) : now;
    } else {
      const uint8_t now = percent_on_curve(discharge_, median);
      shown_ = shown_ ? std::min(*shown_, now) : now;
    }
    return *shown_;
  }

 private:
  enum class Flow { None, Out, In };
  Curve discharge_, charge_;
  std::array<uint16_t, kWindow> window_{};
  size_t next_ = 0, count_ = 0;
  Flow flow_ = Flow::None;
  std::optional<uint8_t> shown_;
};

}  // namespace hg
