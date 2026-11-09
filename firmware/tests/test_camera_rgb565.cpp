#include <array>
#include <cstdint>

#include "camera_rgb565.hpp"
#include "check.hpp"

TEST("camera RGB565 downsample samples pixel centers on the source grid") {
  // Four rows of distinct little-endian 16-bit pixel values.
  const std::array<uint8_t, 32> source{
      0, 0, 1, 0, 2, 0, 3, 0,
      4, 0, 5, 0, 6, 0, 7, 0,
      8, 0, 9, 0, 10, 0, 11, 0,
      12, 0, 13, 0, 14, 0, 15, 0,
  };
  std::array<uint8_t, 8> output{};

  CHECK(hgp::downsample_rgb565(source.data(), source.size(), 4, 4, 8, output.data(), output.size(), 2, 2));
  CHECK_EQ(output.size(), size_t(8));
  CHECK_EQ(output[0], uint8_t(5));
  CHECK_EQ(output[2], uint8_t(7));
  CHECK_EQ(output[4], uint8_t(13));
  CHECK_EQ(output[6], uint8_t(15));
}

TEST("camera RGB565 downsample rejects invalid stride and out-of-range dimensions") {
  const std::array<uint8_t, 8> source{};
  std::array<uint8_t, 8> output{};
  CHECK(!hgp::downsample_rgb565(source.data(), source.size(), 2, 2, 2, output.data(), output.size(), 1, 1));
  CHECK(!hgp::downsample_rgb565(source.data(), source.size(), 2, 2, 4, output.data(), output.size(), 3, 1));
  CHECK(!hgp::downsample_rgb565(source.data(), source.size(), 2, 2, 4, output.data(), output.size(), 1, 3));
}

TEST("camera RGB565 byte swap puts the sensor's high byte into the little-endian slot") {
  std::array<uint8_t, 6> pixels{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  CHECK(hgp::swap_rgb565_bytes(pixels.data(), pixels.size()));
  CHECK_EQ(pixels[0], uint8_t(0x22));
  CHECK_EQ(pixels[1], uint8_t(0x11));
  CHECK_EQ(pixels[2], uint8_t(0x44));
  CHECK_EQ(pixels[3], uint8_t(0x33));
  CHECK_EQ(pixels[4], uint8_t(0x66));
  CHECK_EQ(pixels[5], uint8_t(0x55));
}

TEST("camera RGB565 byte swap is its own inverse") {
  std::array<uint8_t, 8> pixels{0, 1, 2, 3, 4, 5, 6, 7};
  CHECK(hgp::swap_rgb565_bytes(pixels.data(), pixels.size()));
  CHECK(hgp::swap_rgb565_bytes(pixels.data(), pixels.size()));
  const std::array<uint8_t, 8> untouched{0, 1, 2, 3, 4, 5, 6, 7};
  CHECK(pixels == untouched);
}

TEST("camera RGB565 byte swap rejects odd and missing payloads") {
  std::array<uint8_t, 3> pixels{};
  CHECK(!hgp::swap_rgb565_bytes(pixels.data(), pixels.size()));
  CHECK(!hgp::swap_rgb565_bytes(nullptr, 6));
  CHECK(!hgp::swap_rgb565_bytes(pixels.data(), 0));
}
