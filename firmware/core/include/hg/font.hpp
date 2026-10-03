#pragma once

namespace hg::font {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
// Advance and line pitch at scale 1 (glyph plus one pixel of spacing).
constexpr int kCellWidth = 6;
constexpr int kCellHeight = 9;

// Row-major glyph pixels: kGlyphWidth * kGlyphHeight chars, '#' = on.
// Non-printable / non-ASCII bytes map to '?'.
const char* glyph(char c);

}  // namespace hg::font
