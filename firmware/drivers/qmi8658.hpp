#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace hg {

// Register facts from the QST QMI8658A datasheet Rev A (credited in NOTICE):
// WHO_AM_I (0x00) = 0x05; soft reset is RESET (0x60) <- 0xB0, then wait the
// 15 ms boot time; CTRL1 (0x02) <- 0x40 (address auto-increment, little-endian
// data); CTRL2 (0x03) <- 0x1D (+-4 g, 21 Hz low-power output rate); CTRL7
// (0x08) <- 0x01 (accelerometer only); accelerometer data is AX_L (0x35) ..
// AZ_H (0x3A), little-endian, 8192 LSB/g at +-4 g. The port owns I2C.
class Qmi8658 final {
 public:
  using Read = std::function<bool(uint8_t, uint8_t*, size_t)>;
  using Write = std::function<bool(uint8_t, uint8_t)>;
  // begin() waits out the reset; the driver itself never sleeps.
  using DelayMs = std::function<void(uint32_t)>;
  Qmi8658(Read read, Write write, DelayMs delay_ms)
      : read_(std::move(read)), write_(std::move(write)), delay_ms_(std::move(delay_ms)) {}

  // Checks the chip, resets it and configures the accelerometer. False when
  // the chip does not answer or is not a QMI8658.
  bool begin();
  // Acceleration in milli-g. False when the read fails.
  bool read(int16_t mg[3]);

 private:
  Read read_;
  Write write_;
  DelayMs delay_ms_;
};

}  // namespace hg
