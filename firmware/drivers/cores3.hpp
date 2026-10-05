#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace hg {

// CoreS3's AXP2101 rails and AW9523B reset lines. Register accesses are supplied
// by the port so sequencing and preservation of unrelated outputs can be tested.
class CoreS3Control {
 public:
  using Read = std::function<bool(uint8_t, uint8_t, uint8_t&)>;
  using Write = std::function<bool(uint8_t, uint8_t, uint8_t)>;
  using Delay = std::function<void(uint32_t)>;
  CoreS3Control(Read read, Write write, Delay delay)
      : read_(std::move(read)), write_(std::move(write)), delay_(std::move(delay)) {}
  bool begin();
  bool set_brightness(uint8_t percent);

 private:
  bool change(uint8_t address, uint8_t reg, uint8_t mask, uint8_t value);
  Read read_;
  Write write_;
  Delay delay_;
};

}  // namespace hg
