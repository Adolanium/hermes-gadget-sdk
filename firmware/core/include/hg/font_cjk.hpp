// Data of the 12x12 bitmap CJK font; the lookup is font::cjk_glyph (font.hpp).
#pragma once

#include <cstdint>

namespace hg::font {

constexpr int kCjkSize = 12;
constexpr int kCjkBytes = 18;  // 12 rows * 12 bits, packed MSB first

extern const int kCjkGlyphCount;
extern const uint32_t kCjkCodepoints[];
extern const uint8_t kCjkBitmaps[];

}  // namespace hg::font
