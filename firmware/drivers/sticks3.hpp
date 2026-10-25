#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace hg {

// StickS3's M5PM1 power manager. Its GPIO2 switches the LCD rail (L3B) and its
// GPIO3 enables the AW8737 speaker amplifier, so both sit behind I2C rather
// than an ESP32 pin. Register accesses are supplied by the port so sequencing
// and preservation of the other PM1 pins can be tested.
class StickS3Control {
 public:
  using Read = std::function<bool(uint8_t, uint8_t&)>;
  using Write = std::function<bool(uint8_t, uint8_t)>;
  using Delay = std::function<void(uint32_t)>;
  StickS3Control(Read read, Write write, Delay delay)
      : read_(std::move(read)), write_(std::move(write)), delay_(std::move(delay)) {}
  bool begin();
  // Writes only on a change: the speaker task calls this for every chunk.
  bool set_amplifier(bool on);

 private:
  bool change(uint8_t reg, uint8_t mask, uint8_t value);
  Read read_;
  Write write_;
  Delay delay_;
  bool amplifier_ = false;
};

}  // namespace hg
