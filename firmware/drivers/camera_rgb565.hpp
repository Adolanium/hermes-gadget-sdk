#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace hgp {

// Nearest-neighbor downsample of tightly packed RGB565 pixels. The source stride
// may include row padding; output is tightly packed and remains little-endian.
inline bool downsample_rgb565(const uint8_t* source, size_t source_size,
                              uint32_t source_width, uint32_t source_height, size_t source_stride,
                              uint8_t* destination, size_t destination_size,
                              uint32_t destination_width, uint32_t destination_height) {
  if (!source || !destination || source_width == 0 || source_height == 0 ||
      destination_width == 0 || destination_height == 0 ||
      destination_width > source_width || destination_height > source_height) {
    return false;
  }
  if (source_width > std::numeric_limits<size_t>::max() / 2 ||
      source_stride < static_cast<size_t>(source_width) * 2 ||
      source_height > std::numeric_limits<size_t>::max() / source_stride ||
      destination_width > std::numeric_limits<size_t>::max() / 2 ||
      destination_height > std::numeric_limits<size_t>::max() / (static_cast<size_t>(destination_width) * 2)) {
    return false;
  }
  const size_t required_source = source_stride * source_height;
  const size_t required_destination = static_cast<size_t>(destination_width) * destination_height * 2;
  if (source_size < required_source || destination_size < required_destination) return false;

  for (uint32_t y = 0; y < destination_height; ++y) {
    const uint32_t source_y = static_cast<uint32_t>(
        ((static_cast<uint64_t>(2) * y + 1) * source_height) / (static_cast<uint64_t>(2) * destination_height));
    for (uint32_t x = 0; x < destination_width; ++x) {
      const uint32_t source_x = static_cast<uint32_t>(
          ((static_cast<uint64_t>(2) * x + 1) * source_width) / (static_cast<uint64_t>(2) * destination_width));
      const uint8_t* in = source + static_cast<size_t>(source_y) * source_stride + static_cast<size_t>(source_x) * 2;
      uint8_t* out = destination + (static_cast<size_t>(y) * destination_width + x) * 2;
      std::memcpy(out, in, 2);
    }
  }
  return true;
}

// Swap the bytes of packed little-endian RGB565 pixels in place. DVP sensors
// such as the CoreS3's GC0308 transfer the high byte of each 16-bit pixel
// first, so the frame buffer arrives byte-swapped relative to the format the
// gateway expects.
inline bool swap_rgb565_bytes(uint8_t* pixels, size_t byte_count) {
  if (!pixels || byte_count == 0 || byte_count % 2 != 0) return false;
  for (size_t i = 0; i + 1 < byte_count; i += 2) {
    const uint8_t first = pixels[i];
    pixels[i] = pixels[i + 1];
    pixels[i + 1] = first;
  }
  return true;
}

}  // namespace hgp
