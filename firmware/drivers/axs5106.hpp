// AXS5106L touch controller: the six-byte point report at register 0x01.
#pragma once
#include <cstdint>

namespace hg {

// The low nibble of the second byte is the point count (the high nibble is
// flags). Then 12-bit X and Y. A count of 0 is a release. 0x0fff, 0x0fff is
// not a point. False when a press lands outside width x height.
inline bool axs5106_touch(const uint8_t* data, uint16_t width, uint16_t height, bool& down, int16_t& x, int16_t& y) {
  const int points = data[1] & 0x0f;
  x = static_cast<int16_t>(((data[2] & 0x0f) << 8) | data[3]);
  y = static_cast<int16_t>(((data[4] & 0x0f) << 8) | data[5]);
  down = points >= 1 && points <= 2;
  if (!down) {
    x = 0;
    y = 0;
    return true;
  }
  if (x == 0x0fff && y == 0x0fff) return false;
  return x < width && y < height;
}

}  // namespace hg
