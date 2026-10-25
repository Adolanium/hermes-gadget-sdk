#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace hg {

// Register facts from the Bosch BMI270 datasheet (BST-BMI270-DS000, credited in
// NOTICE): CHIP_ID (0x00) = 0x24; soft reset is CMD (0x7E) <- 0xB6, then 2 ms;
// the chip runs only after Bosch's configuration image is loaded: PWR_CONF
// (0x7C) <- 0x00, INIT_CTRL (0x59) <- 0x00, the image written to INIT_DATA
// (0x5E) in bursts addressed by INIT_ADDR_0/1 (0x5B/0x5C, in 16-bit words),
// INIT_CTRL <- 0x01, and INTERNAL_STATUS (0x21) reports 0x01 within 20 ms.
// Then ACC_CONF (0x40) <- 0xA6 (25 Hz, normal filter), ACC_RANGE (0x41) <- 0x01
// (+-4 g), PWR_CTRL (0x7D) <- 0x04 (accelerometer only). Acceleration is
// ACC_X_LSB (0x0C) .. ACC_Z_MSB (0x11), little-endian, 8192 LSB/g at +-4 g.
// The port owns I2C.
class Bmi270 final {
 public:
  using Read = std::function<bool(uint8_t, uint8_t*, size_t)>;
  using Write = std::function<bool(uint8_t, const uint8_t*, size_t)>;
  // begin() waits out the reset and the image start; the driver never sleeps.
  using DelayMs = std::function<void(uint32_t)>;
  Bmi270(Read read, Write write, DelayMs delay_ms)
      : read_(std::move(read)), write_(std::move(write)), delay_ms_(std::move(delay_ms)) {}

  // Checks the chip, resets it, loads the configuration image and starts the
  // accelerometer. False when the chip does not answer, is not a BMI270, or
  // does not accept the image.
  bool begin();
  // Acceleration in milli-g. False when the read fails.
  bool read(int16_t mg[3]);

  // Bytes per INIT_DATA burst; the port's write buffer must hold one plus the register.
  static constexpr size_t kBurst = 128;

 private:
  bool write8(uint8_t reg, uint8_t value) { return write_(reg, &value, 1); }
  Read read_;
  Write write_;
  DelayMs delay_ms_;
};

}  // namespace hg
