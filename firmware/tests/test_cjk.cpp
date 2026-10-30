// The 12x12 CJK font (docs/cjk.md). Built against hg_core_cjk, so these run whether or not
// HG_CJK_FONT is on for the rest of the build.
#include <string>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/font.hpp"
#include "hg/i18n.hpp"

static_assert(HG_CJK_FONT, "test_cjk.cpp needs a core built with the CJK font");

TEST("canvas: wide characters take two columns and wrap anywhere") {
  CHECK_EQ(hg::font::length("ab"), size_t(2));
  CHECK_EQ(hg::font::length("\xE4\xB8\xAD\xE6\x96\x87"), size_t(4));  // two wide characters
  CHECK_EQ(hg::Canvas::text_width("\xE4\xB8\xAD", 1), 11);
  // 6 columns hold three wide characters per line.
  auto lines = hg::wrap_text("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C\xE6\xAD\xA1\xE8\xBF\x8E", 6);
  CHECK_EQ(lines.size(), size_t(2));
  CHECK_EQ(lines[0], std::string("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96"));
  // Mixed text keeps narrow words whole.
  auto mixed = hg::wrap_text("hello \xE4\xB8\xAD\xE6\x96\x87 world", 8);
  CHECK_EQ(mixed.size(), size_t(2));
  CHECK_EQ(hg::font::length(mixed[0]), size_t(8));
  // Truncation never splits a character.
  const std::string zh = "\xE4\xB8\xAD\xE6\x96\x87";
  CHECK_EQ(hg::font::prefix(zh, 3), size_t(3));  // one character: 3 UTF-8 bytes
  CHECK(hg::font::drawable(0x4E2D));
  CHECK(hg::font::cjk_glyph(0x4E2D) != nullptr);
}

TEST("i18n: folding keeps the CJK the font draws; an emoji it lacks stays one column") {
  CHECK_EQ(hg::i18n::fold_to_font("日本 🙂"), std::string("日本 ?"));
}
