#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "hg/hal.hpp"

namespace hg {

// Register facts from Epson's RX8130CE application manual (ETM50E-10, credited
// in NOTICE): the time registers are SEC (0x10) .. YEAR (0x16), BCD, 24 h; WEEK
// (0x13) holds one bit per day, bit 0 for Sunday; writing SEC clears the
// divider below one second. The flag register (0x1D) has VLF in bit 1: set
// when the oscillator stopped or the supply fell too low, so the time is
// invalid; its flags clear when written 0 and ignore a 1. Control register 0
// (0x1E) has STOP in bit 6 and TEST in bit 7; either set, the clock does not
// run. The port owns I2C and provides the block callbacks.
//
// Control register 1 (0x1F: INIEN, CHGEN) is left as found: it selects how the
// chip switches to and charges a backup supply, which depends on the board.
class Rx8130 final {
 public:
  // One I2C transaction reading `size` registers from `start`.
  using BlockRead = std::function<bool(uint8_t, uint8_t*, size_t)>;
  // One I2C transaction writing `size` bytes (starting at `start`'s address).
  using BlockWrite = std::function<bool(uint8_t, const uint8_t*, size_t)>;
  Rx8130(BlockRead read, BlockWrite write) : read_(std::move(read)), write_(std::move(write)) {}

  // UTC time kept by the RTC. False when a read fails, VLF is set, the clock
  // is stopped, or the registers hold no real time.
  bool read(WallTime& utc);
  // Writes UTC time as one burst from 0x10, lets the clock run (STOP and TEST
  // cleared when either was set) and clears VLF. False on failure.
  bool write(const WallTime& utc);

 private:
  BlockRead read_;
  BlockWrite write_;
};

}  // namespace hg
