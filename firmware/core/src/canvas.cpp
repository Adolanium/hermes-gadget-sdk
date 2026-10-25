#include "hg/canvas.hpp"

#include <algorithm>

#include "hg/font.hpp"

namespace hg {

void Canvas::set_clip_rows(int y0, int y1) {
  clip_y0_ = std::max(0, y0);
  clip_y1_ = std::min(h_, y1);
}

void Canvas::pixel(int x, int y, uint16_t color) {
  if (x < 0 || x >= w_ || y < clip_y0_ || y >= clip_y1_ || y >= h_) return;
  if (y < base_y_ || y >= base_y_ + rows_) return;
  px_[(y - base_y_) * stride_ + x] = encode(color);
}

void Canvas::fill_rect(int x, int y, int w, int h, uint16_t color) {
  int x0 = std::max(0, x), x1 = std::min(w_, x + w);
  int y0 = std::max(clip_y0_, y), y1 = std::min(std::min(h_, clip_y1_), y + h);
  y0 = std::max(y0, base_y_);
  y1 = std::min(y1, base_y_ + rows_);
  if (x0 >= x1 || y0 >= y1) return;
  uint16_t c = encode(color);
  for (int yy = y0; yy < y1; ++yy) {
    std::fill(px_ + (yy - base_y_) * stride_ + x0, px_ + (yy - base_y_) * stride_ + x1, c);
  }
}

void Canvas::rect(int x, int y, int w, int h, uint16_t color) {
  fill_rect(x, y, w, 1, color);
  fill_rect(x, y + h - 1, w, 1, color);
  fill_rect(x, y, 1, h, color);
  fill_rect(x + w - 1, y, 1, h, color);
}

void Canvas::fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  r = std::min(r, std::min(w, h) / 2);
  if (r <= 0) {
    fill_rect(x, y, w, h, color);
    return;
  }
  fill_rect(x, y + r, w, h - 2 * r, color);
  for (int i = 0; i < r; ++i) {
    // Inset of row i from the top/bottom edge for a quarter circle of radius r.
    int dy = r - i;
    int inset = 0;
    while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r) ++inset;
    fill_rect(x + inset, y + i, w - 2 * inset, 1, color);
    fill_rect(x + inset, y + h - 1 - i, w - 2 * inset, 1, color);
  }
}

void Canvas::fill_circle(int cx, int cy, int r, uint16_t color) {
  for (int dy = -r; dy <= r; ++dy) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) ++dx;
    fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
  }
}

void Canvas::ring(int cx, int cy, int r, int thickness, uint16_t color) {
  int inner = std::max(0, r - thickness);
  for (int dy = -r; dy <= r; ++dy) {
    for (int dx = -r; dx <= r; ++dx) {
      int d2 = dx * dx + dy * dy;
      if (d2 <= r * r && d2 > inner * inner) pixel(cx + dx, cy + dy, color);
    }
  }
}

void Canvas::arc(int cx, int cy, int r, int thickness, int dir, uint16_t color) {
  int inner = std::max(0, r - thickness);
  for (int dy = -r; dy <= r; ++dy) {
    int ady = dy < 0 ? -dy : dy;
    for (int dx = -r; dx <= r; ++dx) {
      int d2 = dx * dx + dy * dy;
      if (d2 > r * r || d2 <= inner * inner) continue;
      if (dir * dx * 10 < ady * 7) continue;  // outside the +-55 degree cone
      pixel(cx + dx, cy + dy, color);
    }
  }
}

void Canvas::bitmap1(int x, int y, int w, int h, const uint8_t* bits, uint16_t color) {
  const int stride = (w + 7) / 8;
  const uint16_t c = encode(color);
  int y0, y1;
  if (row_window(y0, y1)) return;
  y0 = std::max(y0, y);
  y1 = std::min(y1, y + h);
  for (int yy = y0; yy < y1; ++yy) {
    const uint8_t* row = bits + (yy - y) * stride;
    for (int col = 0; col < w; ++col) {
      int xx = x + col;
      if (xx < 0 || xx >= w_) continue;
      if ((row[col >> 3] >> (7 - (col & 7))) & 1) px_[(yy - base_y_) * stride_ + xx] = c;
    }
  }
}

int Canvas::text(int x, int y, std::string_view s, int scale, uint16_t color) {
  if (scale < 1) scale = 1;
  for (size_t i = 0; i < s.size();) {
    const char* g = font::glyph(font::next(s, i));
    for (int row = 0; row < font::kGlyphHeight; ++row) {
      for (int col = 0; col < font::kGlyphWidth; ++col) {
        if (g[row * font::kGlyphWidth + col] == '#') {
          fill_rect(x + col * scale, y + row * scale, scale, scale, color);
        }
      }
    }
    x += font::kCellWidth * scale;
  }
  return x;
}

int Canvas::text_width(std::string_view s, int scale) {
  if (s.empty()) return 0;
  return static_cast<int>(font::length(s)) * font::kCellWidth * scale - scale;
}

int Canvas::line_height(int scale) { return font::kCellHeight * scale; }

void Canvas::blit(int x, int y, int w, int h, const uint16_t* src) {
  for (int row = 0; row < h; ++row) {
    int yy = y + row;
    if (yy < clip_y0_ || yy >= clip_y1_ || yy >= h_ || yy < 0) continue;
    if (yy < base_y_ || yy >= base_y_ + rows_) continue;
    for (int col = 0; col < w; ++col) {
      int xx = x + col;
      if (xx < 0 || xx >= w_) continue;
      px_[(yy - base_y_) * stride_ + xx] = encode(src[row * w + col]);
    }
  }
}

void Canvas::picture(int x, int y, int size, const uint16_t* src, int src_px, const uint16_t* band, int band_y,
                     int band_rows) {
  for (int row = 0; row < size; ++row) {
    int yy = y + row;
    if (yy < clip_y0_ || yy >= clip_y1_ || yy >= h_ || yy < 0) continue;
    if (yy < base_y_ || yy >= base_y_ + rows_) continue;
    const int src_row = row * src_px / size, in_band = src_row - band_y;
    const uint16_t* line = in_band >= 0 && in_band < band_rows ? band + in_band * src_px : src + src_row * src_px;
    for (int col = 0; col < size; ++col) {
      int xx = x + col;
      uint16_t c = line[col * src_px / size];
      if (xx < 0 || xx >= w_ || !c) continue;
      px_[(yy - base_y_) * stride_ + xx] = encode(c);
    }
  }
}

// Widths are in code points (font::length): a UTF-8 letter is one column.
std::vector<std::string> wrap_text(std::string_view text, int max_chars) {
  std::vector<std::string> lines;
  const size_t max = static_cast<size_t>(max_chars < 1 ? 1 : max_chars);
  if (max_chars < 1) max_chars = 1;
  size_t i = 0;
  while (i <= text.size()) {
    size_t nl = text.find('\n', i);
    std::string_view para = text.substr(i, nl == std::string_view::npos ? std::string_view::npos : nl - i);
    std::string line;
    size_t p = 0;
    while (p < para.size()) {
      while (p < para.size() && para[p] == ' ') ++p;
      size_t end = para.find(' ', p);
      if (end == std::string_view::npos) end = para.size();
      std::string_view word = para.substr(p, end - p);
      p = end;
      if (word.empty()) continue;
      while (font::length(word) > max) {
        if (!line.empty()) {
          lines.push_back(line);
          line.clear();
        }
        const size_t cut = font::prefix(word, max);
        lines.emplace_back(word.substr(0, cut));
        word.remove_prefix(cut);
      }
      if (word.empty()) continue;
      const size_t words = font::length(word);
      size_t needed = line.empty() ? words : font::length(line) + 1 + words;
      if (needed > max) {
        lines.push_back(line);
        line.assign(word);
      } else {
        if (!line.empty()) line.push_back(' ');
        line.append(word);
      }
    }
    lines.push_back(line);
    if (nl == std::string_view::npos) break;
    i = nl + 1;
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

}  // namespace hg
