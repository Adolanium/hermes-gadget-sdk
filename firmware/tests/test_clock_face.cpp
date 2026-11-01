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
constexpr uint16_t kAccent = hg::rgb565(242, 179, 61);  // the ring's seconds dot

// A panel the test renders on. Emissive, so the background is 0 and any ink shows.
struct FaceDisplay : hg::Display {
  hg::DisplayInfo d;
  std::vector<uint16_t> fb;
  int flushed = 0;  // panel rows sent to the panel
  FaceDisplay(int w, int h, bool round, int font_scale = 1) : fb(static_cast<size_t>(w * h)) {
    d.width = static_cast<uint16_t>(w);
    d.height = static_cast<uint16_t>(h);
    d.round = round;
    d.font_scale = static_cast<uint8_t>(font_scale);
    d.emissive = true;
  }
  hg::DisplayInfo info() const override { return d; }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t y0, uint16_t y1) override { flushed += y1 - y0; }
};

struct Face {
  FaceDisplay display;
  hg::Ui ui;
  Face(int w, int h, bool round, int font_scale = 1) : display(w, h, round, font_scale), ui(display) {}
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

TEST("clock face: the face's ink keeps 3 px clear of the clock ring at every shift, on every round size") {
  // Every round panel, the Knob's larger font included. The widest minute ("23:58") and a
  // long Spanish date, at all 60 burn-in shifts: the face's own ink keeps 3 px inside the
  // band and never enters the clock ring's annulus (the band and the dot's reach, which
  // the ring paints over whatever was there), and nothing draws outside the glass. The
  // face's closest ink measures 3.92 px inside the band (the 240, at the shifts that move 4 px).
  const struct { int w, fs; } panels[] = {{240, 1}, {360, 1}, {360, 2}, {412, 1},
                                          {466, 1}, {480, 1}, {800, 1}};
  for (const auto& t : panels) {
    Face f(t.w, t.w, true, t.fs);
    const int cx = t.w / 2, cy = t.w / 2, r = t.w / 2;
    const int s = f.ui.layout().scale;
    const int band_out = r - (2 * s + 2), band_in = band_out - (3 * s + 2);
    // The clock ring's annulus (ui.cpp, clock_rows_), in half pixels.
    const int mid2 = band_in + band_out + 1, reach2 = 8 * s + 2;
    const auto in_ring = [&](int64_t d2) {
      return 4 * d2 >= static_cast<int64_t>(mid2 - reach2) * (mid2 - reach2) &&
             4 * d2 <= static_cast<int64_t>(mid2 + reach2) * (mid2 + reach2);
    };
    bool saw_time = false, saw_date = false, saw_dot = false;
    for (int minute = 0; minute < 60; ++minute) {
      hg::UiModel m = clock_model(static_cast<uint8_t>(minute), "MI\xC3\x89 30 SEPT", hg::i18n::Lang::Es);
      m.clock_time = "23:58";
      f.ui.render(m);
      for (int y = 0; y < t.w; ++y)
        for (int x = 0; x < t.w; ++x) {
          if (f.at(x, y) == 0) continue;
          const int64_t dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
          if (d2 > static_cast<int64_t>(r) * r)
            check::fail(__FILE__, __LINE__,
                        "ink outside the glass at " + std::to_string(x) + "," + std::to_string(y) +
                            " (minute " + std::to_string(minute) + ")");
          if (in_ring(d2)) {
            // The ring's own ground: the amber dot anywhere in its reach, and the grey
            // ticks inside the band. Nothing else may light it: not the dot's reach
            // outside the band, and not the face's ink.
            if (f.at(x, y) == kAccent) {
              saw_dot = true;
              continue;
            }
            if (d2 > static_cast<int64_t>(band_in) * band_in && d2 <= static_cast<int64_t>(band_out) * band_out &&
                f.at(x, y) == kDim)
              continue;  // a tick
            check::fail(__FILE__, __LINE__,
                        "ink inside the clock ring at " + std::to_string(x) + "," + std::to_string(y) +
                            " (minute " + std::to_string(minute) + ")");
            continue;
          }
          if (d2 > static_cast<int64_t>(band_in - 3) * (band_in - 3) && d2 <= static_cast<int64_t>(band_in) * band_in)
            check::fail(__FILE__, __LINE__,
                        "ink within 3 px of the band at " + std::to_string(x) + "," + std::to_string(y) +
                            " (minute " + std::to_string(minute) + ")");
        }
      saw_time |= ink_of(f, kText).any();
      for (int y = 0; y < t.w && !saw_date; ++y)  // the date's grey, well clear of the ring
        for (int x = 0; x < t.w; ++x) {
          const int64_t dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
          if (d2 <= static_cast<int64_t>(band_in - 10) * (band_in - 10) && f.at(x, y) == kDim) {
            saw_date = true;
            break;
          }
        }
    }
    CHECK(saw_time);
    CHECK(saw_date);
    CHECK(saw_dot);  // the second 0 dot at 12 o'clock, over the ticks
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
  // The date's grey is the ring's tick grey too, so the box looks only well inside the band.
  Box d;
  const int limit = 219 - 8;  // the band's inner edge on 466, less the ring's reach
  for (int y = 0; y < 466; ++y)
    for (int x = 0; x < 466; ++x) {
      const int dx = x - 233, dy = y - 233;
      if (dx * dx + dy * dy > limit * limit || f.at(x, y) != kDim) continue;
      if (!d.any()) {
        d = {x, y, x, y};
      } else {
        if (x < d.x0) d.x0 = x;
        if (x > d.x1) d.x1 = x;
        if (y < d.y0) d.y0 = y;
        if (y > d.y1) d.y1 = y;
      }
    }
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
