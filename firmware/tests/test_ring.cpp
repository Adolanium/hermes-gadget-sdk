// The edge ring on round panels (Ui::edge_ring, ring_pixel, update_ring): what each
// state shows, the pixels of the level arcs, the comets and the clock's ticks, how the
// ticks turn with the clock's burn-in shift, that a ring-only change touches only the band,
// and that the battery strip keeps clear of it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/ui.hpp"

using hg::Screen;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The colours the ring uses (ui.cpp).
constexpr uint16_t kBg = hg::rgb565(10, 14, 20);
constexpr uint16_t kAccent = hg::rgb565(242, 179, 61);
constexpr uint16_t kAccentDim = hg::rgb565(110, 82, 30);
constexpr uint16_t kGreen = hg::rgb565(61, 214, 140);
constexpr uint16_t kGreenDim = hg::rgb565(28, 92, 62);
constexpr uint16_t kDim = hg::rgb565(132, 146, 160);
constexpr uint16_t kBlue = hg::rgb565(90, 169, 242);

// A round panel the test renders on, whole-panel (update_ring's path): it records each
// flush's row range and how many rows went out. With `swap`, the framebuffer holds
// byte-swapped pixels, as on most SPI and QSPI panels; at() reads them back as colours.
struct RingDisplay : hg::Display {
  hg::DisplayInfo d;
  std::vector<uint16_t> fb;
  int flushes = 0, flushed_rows = 0;
  std::vector<std::pair<int, int>> ranges;  // each flush's [y0, y1)
  RingDisplay(int w, int h, int font_scale = 1, bool emissive = false, bool swap = false)
      : fb(static_cast<size_t>(w * h)) {
    d.width = static_cast<uint16_t>(w);
    d.height = static_cast<uint16_t>(h);
    d.round = true;
    d.font_scale = static_cast<uint8_t>(font_scale);
    d.emissive = emissive;
    d.swap_bytes = swap;
  }
  hg::DisplayInfo info() const override { return d; }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t y0, uint16_t y1) override {
    ++flushes;
    flushed_rows += y1 - y0;
    ranges.emplace_back(y0, y1);
  }
  uint16_t at(int x, int y) const {
    const uint16_t v = fb[static_cast<size_t>(y * d.width + x)];
    return d.swap_bytes ? static_cast<uint16_t>((v >> 8) | (v << 8)) : v;
  }
};

struct Panel {
  RingDisplay display;
  hg::Ui ui;
  int w = 0, h = 0, cx = 0, cy = 0, in = 0, out = 0, scale = 1;  // the band (ui.cpp)
  Panel(int width, int height, int font_scale = 1, bool emissive = false, bool swap = false)
      : display(width, height, font_scale, emissive, swap), ui(display) {
    w = width;
    h = height;
    cx = w / 2;
    cy = h / 2;
    const int s = ui.layout().scale;
    scale = s;
    out = std::min(w, h) / 2 - (2 * s + 2);
    in = out - (3 * s + 2);
  }
  uint16_t at(int x, int y) const { return display.at(x, y); }
  uint16_t bg() const { return display.d.emissive ? 0 : kBg; }
  bool in_band(int x, int y) const {
    const int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
    return d2 <= out * out && d2 > in * in;
  }
  // The resting clock's ring owns more than the band: the seconds dot's reach, the radii
  // within the dot's radius plus a pixel of the band's middle (ui.cpp, clock_rows_). In
  // half pixels, so every bound is a whole number.
  bool in_clock_ring(int x, int y) const {
    const int dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
    const int mid2 = in + out + 1, reach2 = 8 * scale + 2;  // the dot's radius 4s, plus a pixel
    return 4 * d2 >= (mid2 - reach2) * (mid2 - reach2) && 4 * d2 <= (mid2 + reach2) * (mid2 + reach2);
  }
  // The panel row and column of the band pixel nearest `deg` clockwise from 12 o'clock.
  std::pair<int, int> at_angle(double deg, int radius) const {
    const double rad = deg * kPi / 180.0;
    return {cx + static_cast<int>(std::lround(radius * std::sin(rad))),
            cy - static_cast<int>(std::lround(radius * std::cos(rad)))};
  }
  int mid_radius() const { return (in + out + 1) / 2; }
  // The UI square's origin: everything outside it is only the ring's to draw.
  int square_origin() const { return (w - ui.area().width) / 2; }
};

double angle_deg(int x, int y, int cx, int cy) {
  const double a = std::atan2(x - cx, cy - y) * 180.0 / kPi;
  return a < 0 ? a + 360.0 : a;
}

// A comet's head is the brightest band pixel; its angle, as atan2 measures it.
double brightest_angle(const Panel& p) {
  int best = -1;
  double ang = -1.0;
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x) {
      if (!p.in_band(x, y)) continue;
      const uint16_t px = p.at(x, y);
      const int sum = (px >> 11) + ((px >> 5) & 63) + (px & 31);
      if (sum > best) {
        best = sum;
        ang = angle_deg(x, y, p.cx, p.cy);
      }
    }
  return ang;
}

// A comet's tail pixel is the colour blended over the background by a fraction: dimmer
// than the colour, brighter than the background, and unlike both (the blend runs on the
// panel's 5-bit channels, so a single channel can round back to the background's).
int channel_sum(uint16_t px) { return (px >> 11) + ((px >> 5) & 63) + (px & 31); }
bool tail_pixel(uint16_t px, uint16_t bg, uint16_t fg) {
  return px != bg && px != fg && channel_sum(bg) < channel_sum(px) && channel_sum(px) < channel_sum(fg);
}

hg::UiModel ring_model(Screen screen, uint32_t frame) {
  hg::UiModel m;
  m.screen = screen;
  m.frame = frame;
  m.hero = true;
  m.title = "Hermes";
  return m;
}

// The panel's pixels of one colour, as (x, y): the clock's dot, say.
std::vector<std::pair<int, int>> pixels_of(const Panel& p, uint16_t colour) {
  std::vector<std::pair<int, int>> out;
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x)
      if (p.at(x, y) == colour) out.emplace_back(x, y);
  return out;
}

}  // namespace

TEST("ring: listening fills from 12 o'clock, green, and turns amber for the last 5 s") {
  Panel p(466, 466);
  hg::UiModel m = ring_model(Screen::Listening, 3);
  m.level = 40;
  m.headline = "Listening";
  m.detail = "Release to send";
  m.hint = "CANCEL to discard";

  m.progress_permille = 500;
  p.ui.render(m);
  CHECK_EQ(p.at(456, 233), kGreen);    // 3 o'clock: half the limit used, on the arc
  CHECK_EQ(p.at(10, 233), kGreenDim);  // 9 o'clock: past the arc, on the dim track
  CHECK_EQ(p.at(233, 10), kGreen);     // 12 o'clock: every arc starts here

  m.progress_permille = 833;  // one permille before the switch: still green
  p.ui.render(m);
  CHECK_EQ(p.at(456, 233), kGreen);
  m.progress_permille = 834;  // the last 5 s of the 30 s limit begin here
  p.ui.render(m);
  CHECK_EQ(p.at(456, 233), kAccent);

  m.progress_permille = 866;  // well inside the last 5 s: the arc ends at 312 degrees
  p.ui.render(m);
  CHECK_EQ(p.at(456, 233), kAccent);   // on the arc
  const auto [x, y] = p.at_angle(330, 223);
  CHECK(p.in_band(x, y));
  CHECK_EQ(p.at(x, y), kAccentDim);  // past the arc's end, on the dim track
}

TEST("ring: the thinking comet is blue at its head, dark 90 degrees away, and moves 15 degrees a frame") {
  Panel p(466, 466);
  hg::UiModel m = ring_model(Screen::Thinking, 6);
  m.headline = "Thinking";

  p.ui.render(m);  // frame 6: the head is at 90 degrees, 3 o'clock
  CHECK_EQ(p.at(456, 233), kBlue);  // exactly the head: the full colour
  CHECK_EQ(p.at(233, 10), p.bg());  // 12 o'clock, 90 degrees behind: dark
  CHECK_EQ(p.at(233, 456), p.bg());  // 6 o'clock, 90 degrees ahead: dark
  const auto [tx, ty] = p.at_angle(45, 223);  // 45 degrees behind the head
  CHECK(p.in_band(tx, ty));
  CHECK(tail_pixel(p.at(tx, ty), p.bg(), kBlue));  // the tail: dimmer blue
  const auto [nx, ny] = p.at_angle(70, 223);  // 20 degrees behind the head: nearer to it
  CHECK(channel_sum(p.at(nx, ny)) > channel_sum(p.at(tx, ty)));  // the tail fades with distance
  const double first = brightest_angle(p);
  CHECK(first >= 87.0 && first <= 93.0);

  m.frame = 7;  // same hero animation key (the mascot's dots move every other frame)
  p.ui.render(m);
  const double second = brightest_angle(p);
  CHECK(second >= 102.0 && second <= 108.0);         // 15 degrees further
  CHECK(second - first >= 12.0 && second - first <= 18.0);
}

TEST("ring: speaking is an amber comet nine degrees a frame, and a silent response has no ring") {
  Panel p(466, 466);
  hg::UiModel m = ring_model(Screen::Responding, 5);
  m.speaking = true;
  m.headline = "Speaking";

  p.ui.render(m);  // frame 5 of 40: the head is at 45 degrees
  const double first = brightest_angle(p);
  CHECK(first >= 42.0 && first <= 48.0);
  m.frame = 6;
  p.ui.render(m);
  const double second = brightest_angle(p);
  CHECK(second >= 51.0 && second <= 57.0);  // nine degrees further
  CHECK(second - first >= 6.0 && second - first <= 12.0);
  m.frame = 10;  // the head passes 3 o'clock, where the angle is exact
  p.ui.render(m);
  CHECK_EQ(p.at(456, 233), kAccent);

  // The turn still runs, but nothing is spoken: no ring at all, also where one just was.
  m.speaking = false;
  m.frame = 11;
  p.ui.render(m);
  const int ox = p.square_origin(), side = p.ui.area().width;
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x)
      if (p.in_band(x, y) && (x < ox || x >= ox + side)) CHECK_EQ(p.at(x, y), p.bg());
}

TEST("ring: boot and connecting show the amber comet") {
  for (Screen screen : {Screen::Boot, Screen::Connecting}) {
    Panel p(466, 466);
    hg::UiModel m = ring_model(screen, 6);
    if (screen == Screen::Boot) m.detail = "Starting firmware 1.2.3";
    else m.detail = "ws://hermes.local:8765/gadget";

    p.ui.render(m);  // frame 6 of 24: the head is at 90 degrees
    CHECK_EQ(p.at(456, 233), kAccent);   // exactly the head
    CHECK_EQ(p.at(233, 10), p.bg());     // 90 degrees behind: dark
    const auto [tx, ty] = p.at_angle(45, 223);
    CHECK(tail_pixel(p.at(tx, ty), p.bg(), kAccent));  // the tail fades behind
    const double head = brightest_angle(p);
    CHECK(head >= 87.0 && head <= 93.0);
    m.frame = 7;  // one lap every 24 frames: 15 degrees further
    p.ui.render(m);
    const double next = brightest_angle(p);
    CHECK(next - head >= 12.0 && next - head <= 18.0);
  }
}

TEST("ring: updating covers the received part, and the restart fills the band") {
  Panel p(466, 466);
  hg::UiModel m = ring_model(Screen::Updating, 3);
  m.headline = "Updating";
  m.detail = "firmware 9.9.9: 30% of 40 KB";
  m.hint = "Keep power on";

  m.progress_permille = 0;  // nothing received: the dim track only
  p.ui.render(m);
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x)
      if (p.in_band(x, y)) CHECK_EQ(p.at(x, y), kAccentDim);

  m.progress_permille = 300;  // 30 percent received
  p.ui.render(m);
  CHECK_EQ(p.at(456, 233), kAccent);     // 3 o'clock, before the arc's end
  CHECK_EQ(p.at(10, 233), kAccentDim);   // 9 o'clock, past it

  m.progress_permille = 1000;  // the whole image, and the restart: the full ring
  p.ui.render(m);
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x)
      if (p.in_band(x, y)) CHECK_EQ(p.at(x, y), kAccent);
}

TEST("ring: a running timer keeps its arc over the comets, while listening and updating take the ring") {
  {
    Panel p(466, 466);
    hg::UiModel m = ring_model(Screen::Thinking, 6);  // would be a blue comet
    m.timer_permille = 700;                           // the arc ends at 252 degrees
    m.headline = "Thinking";
    p.ui.render(m);
    CHECK_EQ(p.at(456, 233), kAccent);  // 3 o'clock, on the timer's arc
    const auto [x, y] = p.at_angle(300, 223);
    CHECK(p.in_band(x, y));
    CHECK_EQ(p.at(x, y), kAccentDim);  // past the arc's end
    for (int yy = 0; yy < p.h; ++yy)  // no comet anywhere in the band
      for (int xx = 0; xx < p.w; ++xx)
        if (p.in_band(xx, yy)) CHECK((p.at(xx, yy) == kAccent || p.at(xx, yy) == kAccentDim));

    m.screen = Screen::Responding;  // speaking would be an amber comet
    m.speaking = true;
    m.frame = 5;
    p.ui.render(m);
    CHECK_EQ(p.at(456, 233), kAccent);
    for (int yy = 0; yy < p.h; ++yy)
      for (int xx = 0; xx < p.w; ++xx)
        if (p.in_band(xx, yy)) CHECK((p.at(xx, yy) == kAccent || p.at(xx, yy) == kAccentDim));
  }
  {
    Panel p(466, 466);
    hg::UiModel m = ring_model(Screen::Listening, 3);  // the recording wins
    m.level = 40;
    m.progress_permille = 500;  // the recording's own half-full arc
    m.timer_permille = 1000;    // the timer would fill the band amber
    p.ui.render(m);
    CHECK_EQ(p.at(456, 233), kGreen);
    CHECK_EQ(p.at(10, 233), kGreenDim);
  }
  {
    Panel p(466, 466);
    hg::UiModel m = ring_model(Screen::Updating, 3);  // the update wins
    m.progress_permille = 300;  // its arc ends at 108 degrees
    m.timer_permille = 900;     // the timer's would end at 324
    p.ui.render(m);
    CHECK_EQ(p.at(456, 233), kAccent);    // 3 o'clock: on both arcs
    CHECK_EQ(p.at(233, 456), kAccentDim);  // 6 o'clock: past the update's end, on its dim track
  }
}

TEST("ring: the clock's grey ticks cross the band, the hours 2s+1 px wide, and the dot is the only amber") {
  Panel p(466, 466);
  hg::UiModel m;
  m.screen = Screen::Clock;
  m.clock_time = "09:43";
  m.clock_date = "SAT OCT 24";
  m.clock_minute = 43;  // the burn-in shift's dx is 0: the ticks sit on whole 6 degrees
  m.clock_second = 7;   // the dot at 42 degrees, clear of the ticks checked below

  p.ui.render(m);
  for (int deg : {0, 30}) {  // the hours: the date's grey, across the whole band
    const auto [x, y] = p.at_angle(deg, p.mid_radius());
    CHECK(p.in_band(x, y));
    CHECK_EQ(p.at(x, y), kDim);
  }
  // The 12 o'clock hour tick is 2s + 1 px across: kDim at s px from its line (x = 231 and
  // 235 at s = 2), the background at s + 1 (x = 230 and 236).
  CHECK_EQ(p.at(233 - p.scale, 10), kDim);
  CHECK_EQ(p.at(233 + p.scale, 10), kDim);
  CHECK(p.in_band(233 - p.scale - 1, 10) && p.in_band(233 + p.scale + 1, 10));
  CHECK_EQ(p.at(233 - p.scale - 1, 10), p.bg());
  CHECK_EQ(p.at(233 + p.scale + 1, 10), p.bg());
  {  // a minute tick: grey too, in the band's outer half only
    const auto [x, y] = p.at_angle(6, 225);
    CHECK(p.in_band(x, y));
    CHECK_EQ(p.at(x, y), kDim);
  }
  {  // ...and the band's inner half stays the background
    const auto [x, y] = p.at_angle(6, 221);
    CHECK(p.in_band(x, y));
    CHECK_EQ(p.at(x, y), p.bg());
  }
  // The seconds dot: the only amber on the panel, at the second, about twice as wide as
  // the band (it reaches past the band on both sides), inside the clock ring's annulus.
  const std::vector<std::pair<int, int>> dot = pixels_of(p, kAccent);
  CHECK(dot.size() >= size_t(10));  // a disc, not a tick
  bool inside = false, beyond = false;
  int r_min = p.w, r_max = 0;
  for (const auto& [x, y] : dot) {
    const double a = angle_deg(x, y, p.cx, p.cy);
    CHECK(a >= 37.0 && a <= 47.0);  // at the second: 7 * 6 degrees
    CHECK(p.in_clock_ring(x, y));
    const int dx = x - p.cx, dy = y - p.cy, d2 = dx * dx + dy * dy;
    const int r = static_cast<int>(std::lround(std::sqrt(static_cast<double>(d2))));
    r_min = std::min(r_min, r);
    r_max = std::max(r_max, r);
    if (p.in_band(x, y)) continue;
    inside |= d2 <= p.in * p.in;  // the dot crosses the band's inner edge
    beyond |= d2 > p.out * p.out;  // ...and its outer edge
  }
  CHECK(inside && beyond);
  CHECK(r_max - r_min + 1 >= 2 * (p.out - p.in));  // 16 px across on the 466, the band 8
  for (int y = 0; y < p.h; ++y)  // the rest of the panel holds no amber
    for (int x = 0; x < p.w; ++x)
      if (!p.in_clock_ring(x, y)) CHECK(p.at(x, y) != kAccent);
}

TEST("ring: the clock's next second moves only the dot, sends few rows, and touches nothing outside the clock ring") {
  Panel p(466, 466);
  hg::UiModel m;
  m.screen = Screen::Clock;
  m.clock_time = "09:43";
  m.clock_date = "SAT OCT 24";
  m.clock_minute = 43;  // no turn: the dot at whole 6 degrees
  m.clock_second = 7;
  p.ui.render(m);
  const std::vector<uint16_t> before = p.display.fb;
  p.display.flushes = p.display.flushed_rows = 0;
  p.display.ranges.clear();

  m.clock_second = 8;  // the same minute: the face stays, the dot moves to 48 degrees
  p.ui.render(m);
  CHECK(p.display.flushes > 0);
  CHECK(4 * p.display.flushed_rows < p.h);  // fewer than a quarter of the panel's rows
  for (size_t i = 0; i < before.size(); ++i)
    if (before[i] != p.display.fb[i]) {
      const int x = static_cast<int>(i % p.w), y = static_cast<int>(i / p.w);
      CHECK(p.in_clock_ring(x, y));  // the clock's face was not drawn again
    }
  const std::vector<std::pair<int, int>> dot = pixels_of(p, kAccent);
  CHECK(dot.size() >= size_t(10));
  for (const auto& [x, y] : dot) {
    const double a = angle_deg(x, y, p.cx, p.cy);
    CHECK(a >= 43.0 && a <= 53.0);
  }
}

TEST("ring: second by second and across minutes, the clock's update leaves exactly what a full paint draws") {
  // Within a minute the update rewrites only the rows of the dot's old and new places
  // (draw_frame); a new minute turns the ticks and repaints the face. Every place round the
  // dial, at both dot sizes, must end as a fresh paint of the same second does.
  for (const int size : {240, 466}) {
    Panel p(size, size, 1, false, true);  // byte-swapped, as the AMOLED panels are
    hg::UiModel m;
    m.screen = Screen::Clock;
    m.clock_date = "SAT OCT 24";
    int wrong = 0;
    for (int t = 0; t <= 120; ++t) {  // 09:41:00 to 09:43:00
      const int minute = 41 + t / 60;
      m.clock_minute = static_cast<uint8_t>(minute);
      m.clock_time = "09:" + std::to_string(minute);
      m.clock_second = static_cast<uint8_t>(t % 60);
      p.ui.render(m);
      Panel fresh(size, size, 1, false, true);
      fresh.ui.render(m);
      if (p.display.fb != fresh.display.fb) ++wrong;
    }
    CHECK_EQ(wrong, 0);
  }
}

TEST("ring: each minute the clock's dot turns with the face's burn-in shift, and no tick pixel stays lit") {
  // The face moves dx = (minute * 7) % 9 - 4 px each minute (draw_clock); the ticks and the
  // dot turn half a degree per pixel of it. On the 466 (an AMOLED, byte-swapped), each tick pixel
  // is lit in at most 4 of the cycle's 9 minutes; without the turn, in all 9.
  hg::UiModel m;
  m.screen = Screen::Clock;
  m.clock_date = "SAT OCT 24";
  m.clock_second = 30;  // the dot at 6 o'clock
  std::vector<int> lit(static_cast<size_t>(466 * 466), 0);
  int most = 0;
  for (int minute = 0; minute < 9; ++minute) {
    Panel p(466, 466, 1, true, true);
    m.clock_minute = static_cast<uint8_t>(minute);
    m.clock_time = "09:0" + std::to_string(minute);
    p.ui.render(m);
    double dot = 0;
    int dot_n = 0;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x) {
        const uint16_t c = p.at(x, y);
        if (c == kAccent) {
          dot += angle_deg(x, y, p.cx, p.cy);
          ++dot_n;
        }
        if (p.in_band(x, y) && c == kDim) most = std::max(most, ++lit[static_cast<size_t>(y * p.w + x)]);
      }
    CHECK(dot_n > 0);
    CHECK(std::fabs(dot / dot_n - 180.0 - ((minute * 7) % 9 - 4) * 0.5) < 0.1);  // on its turned tick
  }
  CHECK(most <= 4);
}

TEST("ring: on every round size and at every turn, the clock's ticks follow their rule around the turned lines") {
  // An exact model of the ticks: lines every 6 degrees, turned dx / 2 degrees with the burn-in
  // shift; the hours lit within s + 1 px of their line, across the band, the minutes within s / 2
  // px, in its outer half. Pixels within rounding of a tick's edge, or of the line halfway to the
  // next tick, are left out; every other band pixel of the top half (the dot is at 6 o'clock) must
  // match. At the full turn a wide tick reaches more than 3 degrees from its unturned place, so
  // this also shows each pixel goes to the turned tick nearest it.
  struct Size {
    int size, font_scale;
  };
  const Size sizes[] = {{240, 1}, {360, 1}, {360, 2}, {412, 1}, {466, 1}, {466, 2}, {480, 1}, {800, 1}, {800, 3}};
  for (const Size& z : sizes)
    for (int minute = 0; minute < 9; ++minute) {
      Panel p(z.size, z.size, z.font_scale);
      hg::UiModel m;
      m.screen = Screen::Clock;
      m.clock_time = "09:0" + std::to_string(minute);
      m.clock_date = "SAT OCT 24";
      m.clock_minute = static_cast<uint8_t>(minute);
      m.clock_second = 30;
      p.ui.render(m);
      const double turn = ((minute * 7) % 9 - 4) * 0.5;
      const int s = p.scale, mid2 = p.in + p.out + 1;
      int wrong = 0;
      for (int y = 0; y < p.cy; ++y)
        for (int x = 0; x < p.w; ++x) {
          if (!p.in_band(x, y)) continue;
          const int dx = x - p.cx, dy = y - p.cy;
          const double a = std::atan2(dx, -dy) * 180.0 / kPi;
          const long k = std::lround((a - turn) / 6.0);
          const double rel = a - turn - 6.0 * static_cast<double>(k);  // degrees from that tick's line
          const double line = (6.0 * static_cast<double>(k) + turn) * kPi / 180.0;
          const double dist = std::fabs(dx * std::cos(line) + dy * std::sin(line));
          const bool hour = (k % 60 + 60) % 60 % 5 == 0;
          if (std::fabs(dist - (hour ? s + 1.0 : s / 2.0)) < 0.1 || std::fabs(std::fabs(rel) - 3.0) < 0.3) continue;
          const bool tick = hour ? dist < s + 1 : (4 * (dx * dx + dy * dy) > mid2 * mid2 && 2 * dist <= s);
          wrong += (p.at(x, y) == kDim) != tick;
        }
      CHECK_EQ(wrong, 0);
    }
}

TEST("ring: leaving the clock clears the dot's reach past the band, to Ready and to the alarm") {
  const auto clock_at = [](int second) {
    hg::UiModel m;
    m.screen = Screen::Clock;
    m.clock_time = "09:41";
    m.clock_date = "SAT OCT 24";
    m.clock_minute = 41;
    m.clock_second = static_cast<uint8_t>(second);
    return m;
  };
  {  // Clock, then Ready with no timer: the whole repaint takes the dot's reach away
    Panel p(466, 466);
    p.ui.render(clock_at(7));
    const std::vector<std::pair<int, int>> dot = pixels_of(p, kAccent);
    CHECK(dot.size() >= size_t(10));
    int past = 0;  // the dot's pixels outside the band: what leaving the clock must clear
    for (const auto& [x, y] : dot) past += !p.in_band(x, y);
    CHECK(past >= 5);

    hg::UiModel ready;
    ready.screen = Screen::Ready;
    ready.headline = "Hi, I'm Hermes";
    ready.detail = "Ask me anything";
    p.ui.render(ready);
    const int ox = p.square_origin(), side = p.ui.area().width;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x) {
        if (p.in_band(x, y)) continue;  // the bands own their corners again
        if (x >= ox && x < ox + side && y >= ox && y < ox + side) continue;  // the UI's own
        CHECK_EQ(p.at(x, y), p.bg());  // the dot's reach outside the band is background
      }
  }
  {  // Clock, then the alarm: it owns the whole panel and draws no amber anywhere
    Panel p(466, 466);
    p.ui.render(clock_at(7));
    CHECK(pixels_of(p, kAccent).size() >= size_t(10));
    hg::UiModel alarm;
    alarm.screen = Screen::Alarm;
    alarm.headline = "TIMER DONE";
    alarm.hint = "Tap to stop";
    p.ui.render(alarm);
    CHECK(pixels_of(p, kAccent).empty());
  }
}

TEST("ring: two listening frames that differ only in the ring change nothing outside the band") {
  Panel p(466, 466);
  hg::UiModel m = ring_model(Screen::Listening, 3);  // frame 3: no blink (frames 0 and 1)
  m.level = 40;
  m.headline = "Listening";
  m.detail = "Release to send";
  m.hint = "CANCEL to discard";
  m.progress_permille = 400;
  p.ui.render(m);
  const std::vector<uint16_t> before = p.display.fb;

  m.frame = 6;  // the same frame % 3, so the level bars and the mascot hold still
  m.progress_permille = 500;
  p.ui.render(m);
  size_t changed = 0;
  for (size_t i = 0; i < before.size(); ++i)
    if (before[i] != p.display.fb[i]) {
      ++changed;
      const int x = static_cast<int>(i % p.w), y = static_cast<int>(i / p.w);
      CHECK(p.in_band(x, y));  // only the arc grew
    }
  CHECK(changed > 20);
}

TEST("ring: without a ring no band pixel outside the UI square holds a ring colour, also right after one") {
  {
    Panel p(466, 466);  // Ready, no timer: no ring from the start
    hg::UiModel m = ring_model(Screen::Ready, 3);
    m.headline = "Hi, I'm Hermes";
    m.detail = "Ask me anything";
    p.ui.render(m);
    const int ox = p.square_origin(), side = p.ui.area().width;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x)
        if (p.in_band(x, y) && (x < ox || x >= ox + side)) CHECK_EQ(p.at(x, y), p.bg());
  }
  {
    Panel p(466, 466);  // a comet, then Ready again: the ring's pixels must all go
    hg::UiModel m = ring_model(Screen::Thinking, 6);
    m.headline = "Thinking";
    p.ui.render(m);
    int lit = 0;
    const int ox = p.square_origin(), side = p.ui.area().width;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x)
        if (p.in_band(x, y) && p.at(x, y) != p.bg()) ++lit;
    CHECK(lit > 100);  // the comet was there

    m.screen = Screen::Ready;
    m.frame = 7;
    m.headline = "Hi, I'm Hermes";
    m.detail = "Ask me anything";
    p.ui.render(m);
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x)
        if (p.in_band(x, y) && (x < ox || x >= ox + side)) CHECK_EQ(p.at(x, y), p.bg());
  }
}

TEST("ring: the alarm and the battery faces own the glass with no ring, even with a timer's arc in the model") {
  for (const Screen face : {Screen::Alarm, Screen::LowBattery, Screen::Charging, Screen::Charged}) {
    Panel p(466, 466);
    hg::UiModel m = ring_model(Screen::Ready, 3);  // a timer's arc first
    m.timer_permille = 500;
    m.headline = "Hi, I'm Hermes";
    m.detail = "Timer 0:30";
    p.ui.render(m);
    CHECK_EQ(p.at(456, 233), kAccent);  // the arc is there

    m = ring_model(face, 4);
    m.hero = false;
    m.timer_permille = 500;  // the face comes first, whatever else the model holds
    m.headline = face == Screen::Alarm ? "TIMER DONE" : "8%";
    m.detail = face == Screen::Alarm ? "Tea" : "Low battery\nConnect USB to charge";
    m.hint = face == Screen::Alarm ? "Tap the screen to stop" : "";
    m.face_battery = 8;
    for (const bool flash : {false, true}) {  // the alarm flashes; the battery faces ignore it
      m.flash = flash;
      p.ui.render(m);
      const uint16_t glass = face == Screen::Alarm && flash ? kGreen : p.bg();
      for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x) {
          if (!p.in_band(x, y)) continue;
          CHECK(p.at(x, y) != kAccent && p.at(x, y) != kAccentDim);  // no arc over the face
          if (y < 40) CHECK_EQ(p.at(x, y), glass);  // the glass's own colour at the top
        }
    }
  }
}

TEST("ring: a comet step sends only the rows it crossed, and a repeated frame sends nothing") {
  // Byte-swapped, as the AMOLED and GC9A01 panels are: the changed-pixel check compares
  // what the framebuffer holds.
  Panel p(466, 466, 1, false, true);
  hg::UiModel m = ring_model(Screen::Thinking, 6);  // frames 6 and 7: the mascot holds still
  m.headline = "Thinking";
  p.ui.render(m);

  p.display.flushes = p.display.flushed_rows = 0;
  p.display.ranges.clear();
  m.frame = 7;  // the head moves 15 degrees: a ring-only change
  p.ui.render(m);
  CHECK(p.display.flushes > 0);
  CHECK(p.display.flushed_rows > 0);
  CHECK(p.display.flushed_rows < 2 * p.h / 3);  // the tail's rows (20 to 105 degrees), not the panel's
  for (const auto& [y0, y1] : p.display.ranges) {
    CHECK(y0 >= 0 && y1 <= p.h);
    CHECK(y1 > y0);
  }

  p.display.flushes = p.display.flushed_rows = 0;
  p.ui.render(m);  // the same model again: nothing to send
  CHECK_EQ(p.display.flushes, 0);
  CHECK_EQ(p.display.flushed_rows, 0);
}

TEST("ring: step by step on every round size, each comet's update leaves exactly what a full paint draws") {
  // A comet's step computes only the rectangle that holds what it lights where it was and
  // where it is (comet_box). Every head each comet takes, a lap's wrap past 12 o'clock, the
  // changes of colour and of lap, a recording's arc before and after a comet, and skipped
  // frames must all end as a fresh paint of the same model does.
  const struct {
    int size, font_scale;
    bool emissive;
  } panels[] = {{240, 1, false}, {360, 1, false}, {360, 2, false}, {412, 1, false},
                {466, 2, true},  {480, 2, false}, {800, 3, false}};
  for (const auto& t : panels) {
    Panel p(t.size, t.size, t.font_scale, t.emissive, true);  // byte-swapped, as the AMOLED panels are
    int wrong = 0;
    auto show = [&](Screen screen, uint32_t frame, bool speaking = false, int progress = -1) {
      hg::UiModel m = ring_model(screen, frame);
      m.speaking = speaking;
      m.progress_permille = static_cast<int16_t>(progress);
      p.ui.render(m);
      Panel fresh(t.size, t.size, t.font_scale, t.emissive, true);
      fresh.ui.render(m);
      if (p.display.fb != fresh.display.fb) ++wrong;
    };
    uint32_t f = 0;
    for (; f <= 25; ++f) show(Screen::Boot, f);  // amber, 15 degrees a frame, past 12 o'clock
    for (; f <= 28; ++f) show(Screen::Connecting, f);
    for (; f <= 30; ++f) show(Screen::Listening, f, false, 100 + 30 * static_cast<int>(f));  // the level arc
    for (; f <= 56; ++f) show(Screen::Thinking, f);  // blue: a new colour, a lap and more
    for (; f <= 98; ++f) show(Screen::Responding, f, true);  // amber, 9 degrees a frame: the head jumps
    for (; f <= 120; f += 3) show(Screen::Responding, f, true);  // skipped frames: 27 degrees a step
    show(Screen::Responding, f, false);  // nothing spoken: the ring goes
    show(Screen::Thinking, ++f);
    show(Screen::Listening, ++f, false, 500);  // a talk-over: the comet gives way to the arc
    CHECK_EQ(wrong, 0);
  }
}

TEST("ring: a comet's step leaves the band away from both of its places alone") {
  // update_ring writes a band pixel only where the colour it computes differs, so a pixel it
  // never computes keeps whatever the framebuffer held: here a colour no comet uses, on the
  // ring's far side. From frame 6 to 7 the head moves from 90 to 105 degrees, so the two
  // comets light 20 to 106 degrees between them.
  Panel p(466, 466, 2, true);  // the AMOLED-1.75's size, scale and glass
  hg::UiModel m = ring_model(Screen::Thinking, 6);  // frames 6 and 7: the mascot holds still
  m.headline = "Thinking";
  p.ui.render(m);
  constexpr uint16_t kMark = hg::rgb565(255, 0, 255);
  int marked = 0;
  for (int y = 0; y < p.h; ++y)
    for (int x = 0; x < p.w; ++x) {
      const double a = angle_deg(x, y, p.cx, p.cy);
      if (!p.in_band(x, y) || a < 200.0 || a > 320.0) continue;  // 94 degrees clear of each end
      p.display.fb[static_cast<size_t>(y * p.w + x)] = kMark;
      ++marked;
    }
  CHECK(marked > 1000);

  m.frame = 7;
  p.ui.render(m);
  const double head = brightest_angle(p);
  CHECK(head >= 102.0 && head <= 108.0);  // the comet moved
  CHECK_EQ(static_cast<int>(pixels_of(p, kMark).size()), marked);  // and nothing far off was computed
}

TEST("ring: on every round size, every state's lit pixels outside the UI square lie in the band") {
  const struct {
    int size, font_scale;
    bool emissive;
  } panels[] = {{240, 1, false}, {360, 1, false}, {360, 2, false}, {412, 1, false},
                {466, 2, true},  {480, 2, false}, {800, 3, false}};
  for (const auto& t : panels) {
    for (const bool swap : {false, true}) {  // byte-swapped too, as most SPI and QSPI panels are
      Panel p(t.size, t.size, t.font_scale, t.emissive, swap);
      const int ox = p.square_origin(), side = p.ui.area().width;
      auto state = [&](hg::UiModel m) {
        const bool face = m.screen == Screen::Clock;  // the clock owns the whole panel
        p.ui.render(m);
        for (int y = 0; y < p.h; ++y)
          for (int x = 0; x < p.w; ++x) {
            if (p.at(x, y) == p.bg()) continue;
            if (x >= ox && x < ox + side && y >= ox && y < ox + side) continue;  // the UI's own
            if (p.in_band(x, y) || (face && p.in_clock_ring(x, y))) continue;  // the ring's own
            const int dx = x - p.cx, dy = y - p.cy;
            if (face && dx * dx + dy * dy <= (p.in - 4) * (p.in - 4)) continue;  // its face's ink
            check::fail(__FILE__, __LINE__,
                        std::string("lit pixel outside the band at ") + std::to_string(x) + "," +
                            std::to_string(y) + " on " + hg::screen_name(m.screen) + " (" +
                            std::to_string(t.size) + "px)");
          }
        // The same model on a fresh Ui draws exactly the same pixels.
        Panel fresh(t.size, t.size, t.font_scale, t.emissive, swap);
        fresh.ui.render(m);
        CHECK(fresh.display.fb == p.display.fb);
      };
      hg::UiModel m = ring_model(Screen::Listening, 3);
      m.level = 40;
      m.progress_permille = 400;
      m.headline = "Listening";
      m.detail = "Release to send";
      m.hint = "Swipe down to discard";
      state(m);
      m = ring_model(Screen::Updating, 3);
      m.progress_permille = 600;
      m.headline = "Updating";
      m.detail = "firmware 9.9.9: 60% of 40 KB";
      m.hint = "Keep power on";
      state(m);
      m = ring_model(Screen::Ready, 3);  // no ring of its own: the timer's arc
      m.timer_permille = 700;
      m.headline = "Hi, I'm Hermes";
      m.detail = "Timer 1:14";
      state(m);
      m = ring_model(Screen::Thinking, 4);
      m.headline = "Thinking";
      state(m);
      m = ring_model(Screen::Responding, 5);
      m.speaking = true;
      m.headline = "Speaking";
      state(m);
      m = ring_model(Screen::Boot, 2);
      m.headline = "Hermes";
      m.detail = "Starting firmware 1.2.3";
      state(m);
      m = ring_model(Screen::Connecting, 3);
      m.headline = "Connecting to Hermes";
      m.detail = "ws://hermes.local:8765/gadget";
      state(m);
      m = ring_model(Screen::Clock, 0);
      m.hero = false;
      m.clock_time = "09:41";
      m.clock_date = "SAT OCT 24";
      m.clock_minute = 41;
      m.clock_second = 30;
      state(m);
    }
  }
}

TEST("ring: the battery strip sits below the band's inner edge, in the same rows ring or no ring") {
  auto strip = [](const Panel& p) {
    // The ink above the UI square and inside the band's inner circle: the strip.
    std::vector<std::pair<int, int>> ink;
    const int oy = p.square_origin();
    for (int y = 0; y < oy; ++y)
      for (int x = 0; x < p.w; ++x) {
        if (p.at(x, y) == p.bg()) continue;
        const int dx = x - p.cx, dy = y - p.cy;
        if (dx * dx + dy * dy <= p.in * p.in) ink.emplace_back(x, y);
      }
    return ink;
  };
  Panel p(466, 466);
  hg::UiModel m = ring_model(Screen::Ready, 3);  // no ring
  m.battery = 65;
  m.headline = "Hi, I'm Hermes";
  m.detail = "Ask me anything";
  p.ui.render(m);
  // No ring shows, so nothing above the UI square may draw into the band: the strip
  // starts strictly below the band's inner edge, at its top corners too.
  const int oy = p.square_origin();
  for (int y = 0; y < oy; ++y)
    for (int x = 0; x < p.w; ++x) {
      if (p.at(x, y) == p.bg()) continue;
      const int dx = x - p.cx, dy = y - p.cy;
      CHECK(dx * dx + dy * dy <= p.in * p.in);
    }
  const std::vector<std::pair<int, int>> plain = strip(p);
  CHECK(plain.size() > 30);
  int min_y = p.h;
  for (const auto& [x, y] : plain) min_y = std::min(min_y, y);
  CHECK(min_y >= p.cy - p.in);  // no part of it reaches into the band

  m.screen = Screen::Thinking;  // a ring shows: the comet crosses these rows
  m.frame = 6;
  m.headline = "Thinking";
  p.ui.render(m);
  CHECK(strip(p) == plain);  // the strip did not move
}
