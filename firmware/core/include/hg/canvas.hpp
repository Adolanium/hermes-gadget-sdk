// Drawing primitives over an RGB565 framebuffer.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hg {

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

class Canvas {
 public:
  // `stride` is the framebuffer's row length in pixels when this canvas is a
  // view into a wider framebuffer (0 = `width`).
  Canvas(uint16_t* pixels, int width, int height, bool swap_bytes, int stride = 0)
      : px_(pixels), w_(width), h_(height), stride_(stride > 0 ? stride : width), swap_(swap_bytes) {}

  int width() const { return w_; }
  int height() const { return h_; }

  // Restricts drawing to rows [y0, y1); everything outside is left untouched.
  void set_clip_rows(int y0, int y1);
  void reset_clip() { set_clip_rows(0, h_); }

  void pixel(int x, int y, uint16_t color);
  void fill_rect(int x, int y, int w, int h, uint16_t color);
  void rect(int x, int y, int w, int h, uint16_t color);
  void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
  void fill_circle(int cx, int cy, int r, uint16_t color);
  void ring(int cx, int cy, int r, int thickness, uint16_t color);
  // The part of a ring facing right (dir > 0) or left (dir < 0), about +-55 degrees.
  void arc(int cx, int cy, int r, int thickness, int dir, uint16_t color);
  // 1-bit bitmap (row-major, MSB first, rows padded to bytes): set bits are
  // drawn in `color`, clear bits are left untouched.
  void bitmap1(int x, int y, int w, int h, const uint8_t* bits, uint16_t color);

  // Text at an integer scale; returns the x after the last glyph.
  int text(int x, int y, std::string_view s, int scale, uint16_t color);
  static int text_width(std::string_view s, int scale);
  static int line_height(int scale);

  // Copies a w*h RGB565 block (native byte order) into the framebuffer.
  void blit(int x, int y, int w, int h, const uint16_t* src);

 private:
  uint16_t encode(uint16_t c) const { return swap_ ? static_cast<uint16_t>((c >> 8) | (c << 8)) : c; }

  uint16_t* px_;
  int w_, h_, stride_;
  bool swap_;
  int clip_y0_ = 0;
  int clip_y1_ = 1 << 30;
};

// Greedy word wrap to `max_chars` columns. Honours '\n'; breaks words longer
// than a line.
std::vector<std::string> wrap_text(std::string_view text, int max_chars);

}  // namespace hg
