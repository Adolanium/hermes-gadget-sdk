#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "hg/hal.hpp"

namespace hg {

// Register facts from the NXP PCF85063(A) datasheet (credited in NOTICE): the
// time registers are Seconds (0x04) .. Years (0x0A), BCD, 24 h mode; seconds
// bit 7 is OS (oscillator stopped: the time is invalid); writing seconds
// restarts the clock, so the whole table moves as one burst — reading or
// writing register by register could straddle a seconds rollover. The port
// owns I2C and provides the block callbacks.
class Pcf85063 final {
 public:
  // One I2C transaction reading `size` registers from `start`.
  using BlockRead = std::function<bool(uint8_t, uint8_t*, size_t)>;
  // One I2C transaction writing `size` bytes (starting at `start`'s address).
  using BlockWrite = std::function<bool(uint8_t, const uint8_t*, size_t)>;
  Pcf85063(BlockRead read, BlockWrite write)
      : read_(std::move(read)), write_(std::move(write)) {}

  // UTC time kept by the RTC. False when the read fails or the time is
  // invalid (the OS flag set: backup power was lost).
  bool read(WallTime& utc);
  // Writes UTC time as one burst from 0x04, which restarts the clock and
  // clears OS. False on failure.
  bool write(const WallTime& utc);

 private:
  BlockRead read_;
  BlockWrite write_;
};

// Days between 1970-01-01 and the given civil date (Howard Hinnant's
// days_from_civil). Converts the RTC's UTC fields to an epoch without mktime,
// which would apply the local timezone.
int64_t days_from_civil(uint16_t year, uint8_t month, uint8_t day);

}  // namespace hg
