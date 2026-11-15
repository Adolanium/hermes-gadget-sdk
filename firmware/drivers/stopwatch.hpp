#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "battery_curve.hpp"
#include "hg/hal.hpp"

namespace hg {

// The M5Stack StopWatch's two helper chips on the internal I2C bus. Neither its
// panel, its touch controller, its codec nor its amplifier has an ESP32 pin for
// power or reset: the M5IOE1 I/O expander (0x4F) switches the audio and AMOLED
// supplies, pulses the touch and panel resets and enables the AW8737A amplifier.
// The M5PM1 power manager (0x6E) measures the cell and VIN, reports the charger's
// status pin, shuts the device down and is the only thing that sees the red key.
// Register accesses are supplied by the port so the sequence, the preservation
// of the other pins and the key can be tested.
class StopWatchControl final : public Power {
 public:
  using Read = std::function<bool(uint8_t reg, uint8_t* data, size_t size)>;
  using Write = std::function<bool(uint8_t reg, uint8_t value)>;
  // The expander's registers are 16 bits wide, low byte first.
  using Write16 = std::function<bool(uint8_t reg, uint16_t value)>;
  using Delay = std::function<void(uint32_t ms)>;
  struct Chip {
    Read read;
    Write write;
  };
  StopWatchControl(Chip pm1, Read ioe_read, Write16 ioe_write, Delay delay)
      : pm1_(std::move(pm1)), ioe_read_(std::move(ioe_read)), ioe_write_(std::move(ioe_write)),
        delay_(std::move(delay)) {}

  // The supplies come on, the touch controller and the panel get their reset
  // pulse, and the amplifier stays off. Returns once the panel may be started.
  bool begin();
  // Writes only on a change: the speaker task calls this for every chunk.
  bool set_amplifier(bool on);

  // With the cell's measured curves read() reports a percentage; without, none.
  void use_curves(Curve discharge, Curve charge = {}) { percent_.emplace(discharge, charge); }
  std::optional<PowerStatus> read() override;
  // The PM1's shutdown command. The amplifier goes off first.
  bool power_off() override;
  // The red key: true once per press shorter than a second, on its release.
  // A double click and a long press stay the PM1's own (power off, download mode).
  bool take_key_press() override;
  std::optional<bool> external_power() override;

 private:
  bool pm1_change(uint8_t reg, uint8_t mask, uint8_t value);
  bool ioe_change(uint8_t reg, uint16_t mask, uint16_t value);
  Chip pm1_;
  Read ioe_read_;
  Write16 ioe_write_;
  Delay delay_;
  std::optional<BatteryPercent> percent_;
  bool amplifier_ = false;
  bool key_down_ = false;
  uint8_t key_polls_ = 0;  // polls the key has been down for, saturating
};

}  // namespace hg
