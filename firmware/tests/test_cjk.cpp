// The 12x12 CJK font (docs/cjk.md). Built against hg_core_cjk, so these run whether or not
// HG_CJK_FONT is on for the rest of the build.
#include <cstdint>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/font.hpp"
#include "hg/hal.hpp"
#include "hg/i18n.hpp"
#include "hg/mascot.hpp"
#include "hg/ui.hpp"

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

namespace {

struct Panel : hg::Display {
  static constexpr int kW = 320, kH = 240;
  std::vector<uint16_t> fb = std::vector<uint16_t>(static_cast<size_t>(kW * kH), 0);
  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = kW;
    d.height = kH;
    return d;
  }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t, uint16_t) override {}
};

}  // namespace

TEST("face: with the CJK font, a step of an expression's mark leaves nothing of the last step behind") {
  // This font draws narrow glyphs 3 rows down their line. The mark's glyphs must stay inside the
  // mark's box, which is all a step redraws: a "?" or "!" whose dot hung below it kept its old dot.
  for (hg::mascot::Expression e : {hg::mascot::Expression::Happy, hg::mascot::Expression::Confused,
                                   hg::mascot::Expression::Alert, hg::mascot::Expression::Sleepy}) {
    hg::UiModel m;
    m.screen = hg::Screen::Ready;
    m.hero = true;
    m.link = hg::Link::Online;
    m.title = "Desk";
    m.headline = "Hi, I'm Hermes";
    m.detail = "Ask me anything";
    m.hint = "Hold TALK";
    m.frame = 10;
    m.expression = e;
    Panel p;
    hg::Ui ui(p);
    ui.render(m);
    for (uint32_t f : {15u, 20u, 25u}) {
      m.frame = f;
      ui.render(m);
      Panel whole;
      hg::Ui again(whole);
      again.render(m);
      CHECK(p.fb == whole.fb);
    }
  }
}

namespace {

struct SmallPanel : hg::Display {
  static constexpr int kW = 240, kH = 135;
  std::vector<uint16_t> fb = std::vector<uint16_t>(static_cast<size_t>(kW * kH), 0);
  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = kW;
    d.height = kH;
    d.emissive = true;
    return d;
  }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t, uint16_t) override {}
  int last_inked_row() const {
    for (int y = kH - 1; y >= 0; --y)
      for (int x = 0; x < kW; ++x)
        if (fb[static_cast<size_t>(y * kW + x)]) return y;
    return -1;
  }
};

}  // namespace

TEST("widget: with the CJK font a value is drawn whole on a small panel") {
  // A wide character is 12 rows tall and a narrow one sits 3 rows down its line: a value sized
  // by the 7 rows of the narrow glyph alone ran off the bottom of a 240x135 screen.
  for (const char* value : {"ä¸­", "21"}) {
    hg::UiModel m;
    m.screen = hg::Screen::Widget;
    m.widget.kind = hg::Widget::Kind::Value;
    m.widget.label = "Office";
    m.widget.value = value;
    m.widget_count = 1;
    SmallPanel p;
    hg::Ui ui(p);
    ui.render(m);
    CHECK(p.last_inked_row() >= 0);
    CHECK(p.last_inked_row() < SmallPanel::kH - 2);
  }
}
