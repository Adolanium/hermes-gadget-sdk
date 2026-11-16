#include "axs5106.hpp"
#include "check.hpp"

TEST("AXS5106: one point becomes a press inside the panel") {
  const uint8_t data[6] = {0x00, 0x01, 0x00, 0x0a, 0x00, 0x14};
  bool down = false;
  int16_t x = -1, y = -1;
  CHECK(hg::axs5106_touch(data, 172, 320, down, x, y));
  CHECK(down);
  CHECK_EQ(x, int16_t(10));
  CHECK_EQ(y, int16_t(20));
}

TEST("AXS5106: flags in the high nibble still count as one point") {
  const uint8_t data[6] = {0x00, 0x81, 0x00, 0x0a, 0x00, 0x14};
  bool down = false;
  int16_t x = -1, y = -1;
  CHECK(hg::axs5106_touch(data, 172, 320, down, x, y));
  CHECK(down);
  CHECK_EQ(x, int16_t(10));
  CHECK_EQ(y, int16_t(20));
}

TEST("AXS5106: a zero count is a release") {
  const uint8_t data[6] = {0x00, 0x00, 0x00, 0x22, 0x00, 0x33};
  bool down = true;
  int16_t x = -1, y = -1;
  CHECK(hg::axs5106_touch(data, 172, 320, down, x, y));
  CHECK(!down);
  CHECK_EQ(x, int16_t(0));
  CHECK_EQ(y, int16_t(0));
}

TEST("AXS5106: the invalid point is ignored") {
  const uint8_t data[6] = {0x00, 0x01, 0x0f, 0xff, 0x0f, 0xff};
  bool down = true;
  int16_t x = 0, y = 0;
  CHECK(!hg::axs5106_touch(data, 172, 320, down, x, y));
}

TEST("AXS5106: a point past the glass is ignored") {
  const uint8_t data[6] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x10};
  bool down = false;
  int16_t x = 0, y = 0;
  CHECK(!hg::axs5106_touch(data, 172, 320, down, x, y));
}
