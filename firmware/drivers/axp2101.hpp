#pragma once

#include <functional>
#include <utility>

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

 private:
  Read read_;
  Write write_;
};

}  // namespace hg
