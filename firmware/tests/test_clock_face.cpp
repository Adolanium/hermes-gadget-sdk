// The resting clock face (Ui::draw_clock): the time and the date stay inside the
// glass at every burn-in shift, small panels drop the mascot, the Spanish date
// draws whole, a profile's face takes the mascot's place, and the face repaints
// on the minute, on a new face and on coming back from the alarm.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/mascot.hpp"
#include "hg/ui.hpp"

using hg::Screen;

namespace {

// The colours the face uses (ui.cpp).
constexpr uint16_t kText = hg::rgb565(232, 238, 242);
constexpr uint16_t kDim = hg::rgb565(132, 146, 160);
constexpr uint16_t kGreen = hg::rgb565(61, 214, 140);

// A panel the test renders on. Emissive, so the background is 0 and any ink shows.
struct FaceDisplay : hg::Display {
  hg::DisplayInfo d;
  std::vector<uint16_t> fb;
  int flushed = 0;  // panel rows sent to the panel
  FaceDisplay(int w, int h, bool round) : fb(static_cast<size_t>(w * h)) {
    d.width = static_cast<uint16_t>(w);
    d.height = static_cast<uint16_t>(h);
    d.round = round;
    d.emissive = true;
  }
  hg::DisplayInfo info() const override { return d; }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t y0, uint16_t y1) override { flushed += y1 - y0; }
};

struct Face {
  FaceDisplay display;
  hg::Ui ui;
  Face(int w, int h, bool round) : display(w, h, round), ui(display) {}
  uint16_t at(int x, int y) const { return display.fb[static_cast<size_t>(y * display.d.width + x)]; }
  int width() const { return display.d.width; }
  int height() const { return display.d.height; }
};

hg::UiModel clock_model(uint8_t minute, const std::string& date = "SAT OCT 24",
                        hg::i18n::Lang lang = hg::i18n::Lang::En) {
  hg::UiModel m;
  m.screen = Screen::Clock;
  m.clock_time = "09:41";
  m.clock_date = date;
  m.clock_minute = minute;
  m.lang = lang;
  return m;
}

// The tightest box around every pixel of one colour, or "not found".
struct Box {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // [x0, x1] inclusive
  bool any() const { return x1 >= x0; }
};

Box ink_of(const Face& f, uint16_t color) {
  Box b;
  for (int y = 0; y < f.height(); ++y)
    for (int x = 0; x < f.width(); ++x)
      if (f.at(x, y) == color) {
        if (!b.any()) {
          b.x0 = b.x1 = x;
          b.y0 = b.y1 = y;
        } else {
          if (x < b.x0) b.x0 = x;
          if (x > b.x1) b.x1 = x;
          if (y < b.y0) b.y0 = y;
          if (y > b.y1) b.y1 = y;
        }
      }
  return b;
}

size_t count_of(const Face& f, uint16_t color) {
  size_t n = 0;
  for (uint16_t px : f.display.fb) n += px == color;
  return n;
}

// A face Hermes sent: every bitmap solid ink, with the mascot's anchors.
struct SolidFace {
  hg::mascot::Face face;
  explicit SolidFace(const std::string& hash) {
    const size_t n = 3 * (512 + 1152 + 2592 + 4608);  // 64, 96, 144 and 192 px, three frames each
    auto* bits = static_cast<uint8_t*>(std::malloc(n));
    std::memset(bits, 0xff, n);
    face.bits.reset(bits);
    face.hash = hash;
    face.anchors = hg::mascot::anchors();
  }
};

}  // namespace

TEST("clock face: the time, the date and the mascot stay inside the glass at every shift") {
  const struct { int w, h; } panels[] = {{466, 466}, {360, 360}, {240, 240}};
  for (const auto& p : panels) {
    Face f(p.w, p.h, true);
    const int cx = p.w / 2, cy = p.h / 2, r = p.w / 2;
    bool saw_time = false, saw_date = false;
    for (int minute = 0; minute < 60; ++minute) {
      f.ui.render(clock_model(static_cast<uint8_t>(minute)));
      for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
          if (f.at(x, y) == 0) continue;
          const int64_t dx = x - cx, dy = y - cy;
          if (dx * dx + dy * dy > static_cast<int64_t>(r) * r)
            check::fail(__FILE__, __LINE__,
                        "ink outside the glass at " + std::to_string(x) + "," + std::to_string(y) + " (minute " +
                            std::to_string(minute) + ")");
        }
      saw_time |= ink_of(f, kText).any();
      saw_date |= ink_of(f, kDim).any();
    }
    CHECK(saw_time);
    CHECK(saw_date);
  }
}

TEST("clock face: small panels keep the time and drop the mascot") {
  const struct { int w, h; } panels[] = {{240, 135}, {128, 128}};
  for (const auto& p : panels) {
    Face f(p.w, p.h, false);
    f.ui.render(clock_model(0));
    const Box time = ink_of(f, kText);
    CHECK(time.any());
    // The largest scale the time alone fits at: anything taller is the mascot.
    int k = 1;
    for (int kk = 10; kk >= 1; --kk)
      if (hg::Canvas::text_width("09:41", kk) <= p.w - 8) {
        k = kk;
        break;
      }
    CHECK(time.y1 - time.y0 + 1 <= hg::Canvas::line_height(k));
    CHECK(ink_of(f, kDim).any());
  }
}

TEST("clock face: the Spanish date draws whole") {
  Face f(466, 466, true);
  const std::string date = "S\xC3\x81" "B 24 OCT";  // SÁB 24 OCT
  f.ui.render(clock_model(41, date, hg::i18n::Lang::Es));
  const Box d = ink_of(f, kDim);
  CHECK(d.any());
  CHECK_EQ(d.x1 - d.x0 + 1, hg::Canvas::text_width(date, 3));  // the alarm label's scale
}

TEST("clock face: a new minute repaints, the same minute does not, and the alarm comes back clean") {
  Face f(466, 466, true);
  f.ui.render(clock_model(7));
  f.display.flushed = 0;
  f.ui.render(clock_model(7));  // the same minute again: nothing to send
  CHECK_EQ(f.display.flushed, 0);
  f.ui.render(clock_model(8));  // the next minute: the shifted face
  CHECK(f.display.flushed > 0);

  // The alarm takes the panel, and the clock redraws even in the same minute.
  hg::UiModel alarm;
  alarm.screen = Screen::Alarm;
  alarm.headline = "Timer";
  alarm.hint = "Tap to stop";
  f.ui.render(alarm);
  f.ui.render(clock_model(8));
  bool green = false;
  for (int i = 0; i < f.width() * f.height(); ++i) green |= f.display.fb[static_cast<size_t>(i)] == kGreen;
  CHECK(!green);  // the alarm's whole-panel fill is gone
  CHECK(ink_of(f, kText).any());

  // Leaving the clock repaints the whole panel.
  hg::UiModel ready;
  ready.screen = Screen::Ready;
  f.display.flushed = 0;
  f.ui.render(ready);
  CHECK(f.display.flushed > 0);
}

TEST("clock face: a profile's face takes the mascot's place, and a new face repaints in the same minute") {
  Face f(466, 466, true);
  f.ui.render(clock_model(7));
  const size_t mascot_ink = count_of(f, kText);
  SolidFace solid("a1");
  hg::UiModel m = clock_model(7);
  m.face = &solid.face;
  f.display.flushed = 0;
  f.ui.render(m);  // the same minute, with a face now
  CHECK(f.display.flushed > 0);
  CHECK(count_of(f, kText) > mascot_ink + 2000);  // a solid icon square where the mascot's lines were
}

// The vertical bands of kText ink (rows with any of it, split where two or more
// rows are empty), top to bottom: the icon's band, then the time's.
std::vector<Box> text_bands(const Face& f) {
  std::vector<Box> bands;
  int empty = 2;
  for (int y = 0; y < f.height(); ++y) {
    int x0 = -1, x1 = -1;
    for (int x = 0; x < f.width(); ++x)
      if (f.at(x, y) == kText) {
        if (x0 < 0) x0 = x;
        x1 = x;
      }
    if (x0 < 0) {
      ++empty;
      continue;
    }
    if (empty >= 2 || bands.empty()) bands.push_back({x0, y, x1, y});
    else {
      Box& b = bands.back();
      if (x0 < b.x0) b.x0 = x0;
      if (x1 > b.x1) b.x1 = x1;
      b.y1 = y;
    }
    empty = 0;
  }
  return bands;
}

TEST("clock face: the icon grows to 96 px and sits below the middle of the free rows") {
  Face f(466, 466, true);
  f.ui.render(clock_model(0));
  const std::vector<Box> bands = text_bands(f);
  CHECK(bands.size() >= 2);  // the icon above, the time below
  const Box& icon = bands.front();
  const Box& time = bands.back();
  const int icon_h = icon.y1 - icon.y0 + 1;
  CHECK(icon_h > 64);  // the 96 px bitmap, not the 64 px one
  CHECK(icon_h <= 96);
  CHECK(time.y0 - icon.y1 < icon.y0);  // less room below the icon than above it
}

TEST("clock face: a small round panel keeps a 64 px icon") {
  Face f(240, 240, true);
  f.ui.render(clock_model(0));
  const std::vector<Box> bands = text_bands(f);
  CHECK(bands.size() >= 2);
  const Box& icon = bands.front();
  CHECK(icon.y1 - icon.y0 + 1 <= 64);
}
