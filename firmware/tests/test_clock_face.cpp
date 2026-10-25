// The resting clock face (Ui::draw_clock): the time and the date stay inside the
// glass at every burn-in shift, small panels drop the mascot, the Spanish date
// draws whole, a profile's face takes the mascot's place, and the face repaints
// on the minute, on a new face and on coming back from the alarm.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
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

TEST("clock face: a colour face moves with the burn-in shift and its transparent middle shows the background") {
  constexpr uint16_t kRose = hg::rgb565(220, 60, 120);
  const int px = 192;
  std::vector<uint16_t> pixels(static_cast<size_t>(px * px), kRose);
  for (int y = px / 4; y < 3 * px / 4; ++y)
    for (int x = px / 4; x < 3 * px / 4; ++x) pixels[static_cast<size_t>(y * px + x)] = 0;
  hg::mascot::Face colour;
  colour.hash = "c1";
  colour.rgb565 = pixels.data();
  colour.px = px;
  colour.anchors = hg::mascot::anchors();

  Face f(466, 466, true);
  hg::UiModel m = clock_model(7);
  m.face = &colour;
  f.ui.render(m);
  const Box a = ink_of(f, kRose);
  CHECK(a.any());
  CHECK_EQ(a.x1 - a.x0 + 1, 96);  // drawn at the clock's icon size
  CHECK_EQ(f.at((a.x0 + a.x1) / 2, (a.y0 + a.y1) / 2), uint16_t(0));  // the background through the hole
  m.clock_minute = 8;
  f.ui.render(m);
  const Box b = ink_of(f, kRose);
  CHECK(b.any() && (b.x0 != a.x0 || b.y0 != a.y0));  // shifted with the time

  // On the alarm's green flash too, the face keeps its colours and its middle shows the flash.
  hg::UiModel alarm;
  alarm.screen = Screen::Alarm;
  alarm.headline = "Timer";
  alarm.hint = "Tap to stop";
  alarm.flash = true;
  alarm.face = &colour;
  f.ui.render(alarm);
  const Box c = ink_of(f, kRose);
  CHECK(c.any());
  CHECK_EQ(f.at((c.x0 + c.x1) / 2, (c.y0 + c.y1) / 2), kGreen);
}

TEST("ready: a colour face is sent at the largest size Ready draws it, so it is only ever scaled down") {
  constexpr uint16_t kRose = hg::rgb565(220, 60, 120);
  int shrunk = 0;
  for (auto [w, h, round] : {std::tuple{240, 240, true}, {320, 240, false}, {360, 360, true}, {466, 466, true}}) {
    Face f(w, h, round);
    f.ui.set_touch_px(60);
    const int px = f.ui.face_px();
    std::vector<uint16_t> pixels(static_cast<size_t>(px * px), kRose);
    hg::mascot::Face colour;
    colour.hash = "c1";
    colour.rgb565 = pixels.data();
    colour.px = static_cast<uint16_t>(px);
    colour.anchors = hg::mascot::anchors();
    hg::UiModel ready;
    ready.screen = Screen::Ready;
    ready.hero = true;
    ready.headline = "Hi, I'm Hermes";
    ready.detail = "Ask me anything";
    ready.face = &colour;
    f.ui.render(ready);
    const Box one_line = ink_of(f, kRose);
    CHECK_EQ(one_line.x1 - one_line.x0 + 1, px);
    ready.caption_lines = 2;  // offering profiles: a second line, and the name panel on touch boards
    ready.name_target = true;
    ready.detail = "Tap the name to switch profile. Ask me anything";
    f.ui.render(ready);
    const Box switching = ink_of(f, kRose);
    CHECK(switching.any() && switching.x1 - switching.x0 + 1 <= px);
    shrunk += switching.x1 - switching.x0 + 1 < px;
  }
  CHECK(shrunk > 0);  // the case this guards: some panels draw Ready's face smaller while switching
}

TEST("ready: a colour face blinks and talks at the mascot's cadence, and repaints every row its bands change") {
  constexpr uint16_t kRose = hg::rgb565(220, 60, 120);
  constexpr uint16_t kTeal = hg::rgb565(40, 180, 170);
  constexpr uint16_t kAmber = hg::rgb565(240, 160, 40);
  for (auto [w, h, round] : {std::tuple{240, 240, true}, {320, 240, false}, {360, 360, true}, {466, 466, true}}) {
    for (int lines : {1, 2}) {  // two lines draw the face smaller than px, so the bands' rows are scaled
      const int px = Face(w, h, round).ui.face_px();
      // Bands at rows that do not fall on whole drawn rows when scaled, one row more than a multiple of 3.
      const int by = px / 3 + 1, br = px / 9 + 1, ty = 2 * px / 3 + 1, tr = px / 12 + 1;
      std::vector<uint16_t> idle(static_cast<size_t>(px * px), kRose), blink(static_cast<size_t>(br * px), kTeal),
          talk(static_cast<size_t>(tr * px), kAmber);
      hg::mascot::Face colour;
      colour.hash = "m1";
      colour.rgb565 = idle.data();
      colour.px = static_cast<uint16_t>(px);
      colour.anchors = hg::mascot::anchors();
      colour.blink = {blink.data(), static_cast<uint16_t>(by), static_cast<uint16_t>(br)};
      colour.talk = {talk.data(), static_cast<uint16_t>(ty), static_cast<uint16_t>(tr)};
      hg::UiModel m;
      m.hero = true;
      m.headline = "Hi, I'm Hermes";
      m.detail = lines == 2 ? "Tap the name to switch profile. Ask me anything" : "Ask me anything";
      m.caption_lines = static_cast<uint8_t>(lines);
      m.face = &colour;
      Face f(w, h, round);
      // Rendered after the frame before, so only the rows the face changes repaint; a fresh Ui draws it whole.
      auto step = [&](Screen screen, bool speaking, uint32_t frame) {
        m.screen = screen;
        m.speaking = speaking;
        m.frame = frame;
        f.ui.render(m);
        Face fresh(w, h, round);
        fresh.ui.render(m);
        CHECK(f.display.fb == fresh.display.fb);
      };
      int blinks = 0, talks = 0;
      for (uint32_t frame = 2; frame < 92; ++frame) {
        step(Screen::Ready, false, frame);
        blinks += count_of(f, kTeal) > 0;
        CHECK_EQ(count_of(f, kAmber), size_t(0));
      }
      CHECK_EQ(blinks, 4);  // 2 frames in 45, as the mascot blinks
      for (uint32_t frame = 0; frame < 8; ++frame) {
        step(Screen::Responding, true, frame);
        talks += count_of(f, kAmber) > 0;
      }
      CHECK_EQ(talks, 4);  // the mouth toggles every 2 frames (200 ms) while speaking

      colour.blink = {};  // only a talk band: no blink, and nothing repaints for one
      colour.hash = "m2";
      step(Screen::Ready, false, 2);
      const std::vector<uint16_t> still = f.display.fb;
      const int flushed = f.display.flushed;
      step(Screen::Ready, false, 45);
      CHECK(f.display.fb == still && f.display.flushed == flushed);
    }
  }
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
