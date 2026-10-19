#pragma once

#include <functional>
#include <utility>

#include "hg/hal.hpp"

namespace hg {

// Read-only STC8H1KXX battery record. The protocol requires one register read
// per byte; the board port supplies the I2C transaction function.
class Stc8Battery final : public Power {
 public:
  static constexpr uint8_t kI2cAddress = 0x2f;
  static constexpr uint8_t kBatteryInfoBytes = 12;
  using ReadByte = std::function<bool(uint8_t, uint8_t&)>;

  explicit Stc8Battery(ReadByte read_byte) : read_byte_(std::move(read_byte)) {}
  std::optional<PowerStatus> read() override;
  bool can_power_off() const override { return false; }
  bool power_off() override { return false; }

 private:
  ReadByte read_byte_;
};

}  // namespace hg
