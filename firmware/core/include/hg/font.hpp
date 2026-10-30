#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hg::font {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
// Advance and line pitch at scale 1 (glyph plus one pixel of spacing).
constexpr int kCellWidth = 6;
// With the 12x12 CJK font (-DHG_CJK_FONT=1, CMake option HG_CJK_FONT) the line
// pitch grows to fit it, and ASCII sits 3 rows lower to share its baseline. Off,
// ASCII layouts are unchanged and CJK glyphs overlap the next line.
#ifndef HG_CJK_FONT
#define HG_CJK_FONT 0
#endif
#if HG_CJK_FONT
constexpr int kCellHeight = 13;
constexpr int kAsciiYOffset = 3;
#else
constexpr int kCellHeight = 9;
constexpr int kAsciiYOffset = 0;
#endif

// Text is UTF-8. The font draws printable ASCII and Latin-1 (U+00A0..U+00FF)
// at one column each, plus about 7,400 CJK characters at two columns (a 12x12
// bitmap, font_cjk.hpp); any other code point draws as '?'. Widths and columns
// count display columns: 1 for narrow code points, 2 for wide ones.

// Row-major glyph pixels: kGlyphWidth * kGlyphHeight chars, '#' = on.
const char* glyph(char32_t code_point);
// True for the code points the font has its own glyph for.
bool drawable(char32_t code_point);
// Columns one code point takes: 2 for CJK and fullwidth forms, else 1.
int columns(char32_t code_point);
// The 12x12 bitmap (18 bytes, row-major, MSB first), or nullptr if there is none.
const uint8_t* cjk_glyph(char32_t code_point);
// The code point at byte `i`, advancing `i` past it. A malformed or truncated
// sequence reads as U+FFFD and advances one byte, so any input terminates.
char32_t next(std::string_view s, size_t& i);
// Columns of `s` (a wide code point is two).
size_t length(std::string_view s);
// Bytes of the longest prefix of `s` that fits in `chars` columns, never splitting
// a character (all of it if shorter).
size_t prefix(std::string_view s, size_t chars);

}  // namespace hg::font
