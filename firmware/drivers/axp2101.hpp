#pragma once

#include <functional>
#include <utility>

#include "battery_curve.hpp"
#include "hg/hal.hpp"

namespace hg {

// Register definitions follow X-Powers AXP2101 SWcharge v1.0, section 6.13.
// The port owns I2C. Reading never changes charging, rails, or gauge parameters.
class Axp2101 final : public Power {
 public:
  using Read = std::function<bool(uint8_t, uint8_t*, size_t)>;
  using Write = std::function<bool(uint8_t, uint8_t)>;
  Axp2101(Read read, Write write) : read_(std::move(read)), write_(std::move(write)) {}
  std::optional<PowerStatus> read() override;
  bool power_off() override;
  bool enable_aldo1_3v3();  // Only for boards whose audio circuit requires this rail.
  // The power key, for boards where only the PMIC sees it. Enabling changes the
  // interrupt registers only, and drops a press latched before it.
  bool enable_key_press();
  bool take_key_press();  // true once per short press
  std::optional<bool> vbus_good();  // external power present (status 0x00 bit 5)
  // Curves measured on the board's cell. With them the percent is the smoothed
  // discharge curve on battery, the charge curve (or the gauge) while charging and
  // 100 once charged (BatteryPercent); without, the gauge or the linear fallback.
  void use_curves(Curve discharge, Curve charge = {}) { percent_.emplace(discharge, charge); }
  // The constant-current charge limit (ICC, 0x62), for diagnostics. Read only.
  std::optional<uint16_t> charge_current_ma();

 private:
  Read read_;
  Write write_;
  std::optional<BatteryPercent> percent_;
};

}  // namespace hg
