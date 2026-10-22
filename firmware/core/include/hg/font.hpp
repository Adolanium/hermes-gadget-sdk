#pragma once

#include <cstddef>
#include <string_view>

namespace hg::font {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
// Advance and line pitch at scale 1 (glyph plus one pixel of spacing).
constexpr int kCellWidth = 6;
constexpr int kCellHeight = 9;

// Text is UTF-8. The font draws printable ASCII and Latin-1 (U+00A0..U+00FF);
// any other code point draws as '?'. Widths and columns count code points.

// Row-major glyph pixels: kGlyphWidth * kGlyphHeight chars, '#' = on.
const char* glyph(char32_t code_point);
// True for the code points the font has its own glyph for.
bool drawable(char32_t code_point);
// The code point at byte `i`, advancing `i` past it. A malformed or truncated
// sequence reads as U+FFFD and advances one byte, so any input terminates.
char32_t next(std::string_view s, size_t& i);
// Code points in `s`.
size_t length(std::string_view s);
// Bytes taken by the first `chars` code points of `s` (all of it if shorter).
size_t prefix(std::string_view s, size_t chars);

}  // namespace hg::font
