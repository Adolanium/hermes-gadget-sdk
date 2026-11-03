#include "hg/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "hg/font.hpp"

namespace hg {
namespace {

// The background on backlit panels; emissive panels use true black (Ui::bg_).
constexpr uint16_t kBg = rgb565(10, 14, 20);
constexpr uint16_t kBar = rgb565(24, 31, 42);
constexpr uint16_t kText = rgb565(232, 238, 242);
constexpr uint16_t kDim = rgb565(132, 146, 160);
constexpr uint16_t kFaint = rgb565(52, 62, 76);
constexpr uint16_t kAccent = rgb565(242, 179, 61);
constexpr uint16_t kGreen = rgb565(61, 214, 140);
constexpr uint16_t kBlue = rgb565(90, 169, 242);
constexpr uint16_t kRed = rgb565(242, 95, 92);
constexpr uint16_t kYellow = rgb565(242, 201, 76);
constexpr uint16_t kGreenDim = rgb565(28, 92, 62);
constexpr uint16_t kBlueDim = rgb565(52, 96, 140);
constexpr uint16_t kAccentDim = rgb565(110, 82, 30);
constexpr uint16_t kRedDim = rgb565(122, 44, 44);

class Hash {
 public:
  Hash& add(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
      h_ ^= p[i];
      h_ *= 16777619u;
    }
    return *this;
  }
  Hash& add(const std::string& s) {
    add(s.data(), s.size());
    uint8_t sep = 0xFF;
    return add(&sep, 1);
  }
  template <typename T>
  Hash& val(T v) {
    return add(&v, sizeof(v));
  }
  uint32_t get() const { return h_; }

 private:
  uint32_t h_ = 2166136261u;
};

// Columns are code points (font::length), and cuts fall between them.
std::string fit(std::string_view s, int max_chars) {
  if (max_chars <= 0) return {};
  if (static_cast<int>(font::length(s)) <= max_chars) return std::string(s);
  if (max_chars <= 2) return std::string(s.substr(0, font::prefix(s, static_cast<size_t>(max_chars))));
  return std::string(s.substr(0, font::prefix(s, static_cast<size_t>(max_chars - 2)))) + "..";
}

int cols_for(int width_px, int scale) { return width_px / (font::kCellWidth * scale); }

// The edge ring's level arcs use integer angles: sine and cosine times kArcScale, so the
// per-pixel test is a sign check and one cross product. The sweep is computed once per frame.
constexpr int32_t kArcScale = 4096;
constexpr double kTau = 6.28318530717958647692;

// The sweep that ends `permille` thousandths of a turn clockwise from 12 o'clock. An empty
// arc (permille 0) ends at 12 o'clock too; in_arc() tests it as empty.
ArcSweep arc_sweep(int permille) {
  ArcSweep s;
  s.permille = permille;
  if (permille <= 0) {
    s.ey = -kArcScale;
    return s;
  }
  const double theta = kTau * permille / 1000.0;
  s.ex = static_cast<int32_t>(std::lround(std::sin(theta) * kArcScale));
  s.ey = static_cast<int32_t>(std::lround(-std::cos(theta) * kArcScale));
  return s;
}

// Whether the pixel at (dx, dy) from the centre lies on the arc from 12 o'clock clockwise to the
// sweep's end. The sign of dx is the sign of the angle's sine (the right half); a cross product
// with the end direction says which side of the end the pixel is on. An end past 180 degrees
// takes the whole right half and what comes before the end; otherwise only the right half
// before the end.
bool in_arc(int dx, int dy, const ArcSweep& s) {
  if (s.permille <= 0) return false;
  if (dx == 0 && dy < 0) return true;  // 12 o'clock starts every arc that has any length
  const int32_t before_end = dx * s.ey - dy * s.ex;  // sin(end - angle) times the lengths
  return s.permille > 500 ? (dx >= 0 || before_end > 0) : (dx >= 0 && before_end > 0);
}

// floor(sqrt(v)), exact; 0 for v <= 0.
int isqrt(int64_t v) {
  int r = 0;
  while (static_cast<int64_t>(r + 1) * (r + 1) <= v) ++r;
  return r;
}

// The edge ring's angles are in 1/64 degree: a whole turn, and the comet's tail.
constexpr int32_t kTurn64 = 360 * 64;
constexpr int32_t kTail64 = 70 * 64;
// Listening turns amber in the last sixth of the recording limit: its last 5 of 30 s.
constexpr int16_t kLateListening = 834;

// The resting clock's burn-in shift: each minute the face moves (dx, dy) px, each -4 to 4,
// through 9 places in a 9-minute cycle (draw_clock). The ring's ticks and seconds dot turn
// with it by kTickTurn64 per px of dx: half a degree, so up to 2 degrees either way and
// mostly a degree from one minute to the next (edge_ring).
struct BurnInShift {
  int dx, dy;
};
BurnInShift burn_in_shift(int minute) { return {(minute * 7) % 9 - 4, (minute * 5) % 9 - 4}; }
constexpr int32_t kTickTurn64 = 32;

// atan(n / d) for 0 <= n <= d, d > 0, in 1/64 degree: (pi/4) t + 0.273 t (1 - t) radians
// for t = n / d, within a quarter of a degree.
int32_t atan64(int32_t n, int32_t d) {
  const int32_t t = (n << 14) / d;  // 0..16384
  return (2880 * t + 1001 * ((t * (16384 - t)) >> 14)) >> 14;
}

// The angle of (dx, dy) from the centre, clockwise from 12 o'clock, in 1/64 degree:
// [0, kTurn64). Integer arithmetic only, so every port draws the same ring.
int32_t ring_angle64(int dx, int dy) {
  const int32_t up = -dy, au = std::abs(up), ar = std::abs(dx);
  if (!au && !ar) return 0;
  const int32_t q = ar <= au ? atan64(ar, au) : 90 * 64 - atan64(au, ar);  // within the quadrant
  if (dx >= 0) return up >= 0 ? q : 180 * 64 - q;
  return up < 0 ? 180 * 64 + q : (kTurn64 - q) % kTurn64;
}

// An RGB565 colour's channels at 8 bits.
void channels(uint16_t c, uint8_t out[3]) {
  out[0] = static_cast<uint8_t>((((c >> 11) & 31) * 527 + 23) >> 6);
  out[1] = static_cast<uint8_t>((((c >> 5) & 63) * 259 + 33) >> 6);
  out[2] = static_cast<uint8_t>(((c & 31) * 527 + 23) >> 6);
}

// What a round panel's edge ring shows for this model; the first match wins. A recording or
// an update takes the ring from a running timer, and the timer keeps it while Hermes thinks
// or speaks. The alarm and the battery faces have none; the resting clock has its ticks,
// turned with the face's burn-in shift. Comets lap the ring once per 24 frames (2.4 s at the
// app's 10 frames a second), or 40 while Hermes speaks.
EdgeRing edge_ring(const UiModel& m) {
  EdgeRing r;
  const auto level = [&r](int permille, uint16_t colour, uint16_t dim) {
    r.kind = EdgeRing::Kind::Level;
    r.permille = static_cast<int16_t>(std::clamp(permille, 0, 1000));
    r.colour = colour;
    r.dim = dim;
  };
  const auto comet = [&r, &m](uint32_t lap, uint16_t colour) {
    r.kind = EdgeRing::Kind::Comet;
    r.colour = colour;
    r.head = static_cast<uint16_t>(m.frame % lap * 360 / lap);
  };
  switch (m.screen) {
    case Screen::Alarm:
    case Screen::LowBattery:
    case Screen::Charging:
    case Screen::Charged:
      return r;
    case Screen::Clock:
      r.kind = EdgeRing::Kind::Ticks;
      r.colour = kAccent;
      r.second = static_cast<uint8_t>(m.clock_second % 60);
      r.turn64 = static_cast<int16_t>(burn_in_shift(m.clock_minute).dx * kTickTurn64);
      return r;
    case Screen::Listening:
      if (m.progress_permille >= 0) {
        const bool late = m.progress_permille >= kLateListening;
        level(m.progress_permille, late ? kAccent : kGreen, late ? kAccentDim : kGreenDim);
        return r;
      }
      break;
    case Screen::Updating:
      if (m.progress_permille >= 0) {
        level(m.progress_permille, kAccent, kAccentDim);
        return r;
      }
      break;
    default:
      break;
  }
  if (m.timer_permille >= 0) {
    level(m.timer_permille, kAccent, kAccentDim);
    return r;
  }
  switch (m.screen) {
    case Screen::Thinking: comet(24, kBlue); break;
    case Screen::Responding:
      if (m.speaking) comet(40, kAccent);
      break;
    case Screen::Boot:
    case Screen::Connecting: comet(24, kAccent); break;
    default: break;
  }
  return r;
}

// A headline in at most two lines: split at the space nearest the middle.
std::vector<std::string> headline_lines(const std::string& text) {
  const size_t npos = std::string::npos, mid = text.size() / 2;
  size_t best = npos;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != ' ') continue;
    const size_t d = i > mid ? i - mid : mid - i;
    const size_t bd = best == npos ? 0 : (best > mid ? best - mid : mid - best);
    if (best == npos || d < bd) best = i;
  }
  if (best == npos) return {text};
  return {text.substr(0, best), text.substr(best + 1)};
}

bool animated(Screen s) {
  switch (s) {
    case Screen::Boot:
    case Screen::Connecting:
    case Screen::Updating:
    case Screen::Ready:
    case Screen::Listening:
    case Screen::Thinking:
    case Screen::Responding:
      return true;
    default:
      return false;
  }
}

// Triangle wave in [0, period/2] for gentle pulsing.
int tri(uint32_t frame, int period) {
  int p = static_cast<int>(frame % static_cast<uint32_t>(period));
  return p < period / 2 ? p : period - p;
}

mascot::Frame mascot_frame(const UiModel& m) {
  mascot::Frame f = m.frame % 45 < 2 ? mascot::Frame::Blink : mascot::Frame::Idle;  // blink every ~4.5 s
  if (m.screen == Screen::Responding && m.speaking) f = (m.frame / 2) % 2 ? mascot::Frame::Talk : mascot::Frame::Idle;
  const bool still = m.face && m.face->rgb565 && !m.face->band(f).rows;  // a colour face without that frame
  return still ? mascot::Frame::Idle : f;
}

// `b` (from mascot::pick) in `ink`, or the colour face at b's size, in b's frame.
void draw_face(Canvas& c, int x, int y, const mascot::Bitmap& b, const mascot::Face* face, uint16_t ink) {
  if (face && face->rgb565) {
    const mascot::Band& band = face->band(b.frame);
    c.picture(x, y, b.size, face->rgb565, face->px, band.rgb565, band.y, band.rows);
  } else {
    c.bitmap1(x, y, b.size, b.size, b.bits, ink);
  }
}

// Everything that changes hero pixels between frames, and nothing else, so
// a still mascot never triggers a redraw.
uint32_t hero_anim_key(const UiModel& m) {
  Hash h;
  h.val(mascot_frame(m));
  switch (m.screen) {
    case Screen::Listening: h.val(m.frame % 3).val(m.level / 20); break;
    case Screen::Thinking:
    case Screen::Connecting:
    case Screen::Updating: h.val((m.frame / 2) % 4); break;
    case Screen::Responding: h.val((m.frame / 2) % 3); break;
    default: break;
  }
  return h.get();
}

uint16_t tone_color(DashRow::Tone t) {
  switch (t) {
    case DashRow::Tone::Busy: return kBlue;
    case DashRow::Tone::Waiting: return kAccent;
    case DashRow::Tone::Good: return kGreen;
    case DashRow::Tone::Warn: return kYellow;
    case DashRow::Tone::Bad: return kRed;
    case DashRow::Tone::Dim: break;
  }
  return kDim;
}

uint16_t caption_color(Screen s) {
  switch (s) {
    case Screen::Listening: return kGreen;
    case Screen::Thinking: return kBlue;
    case Screen::Responding:
    case Screen::Connecting:
    case Screen::Updating:
    case Screen::Pairing:
    case Screen::Prompt: return kAccent;
    case Screen::Offline:
    case Screen::Error: return kRed;
    default: return kText;
  }
}

}  // namespace

const char* screen_name(Screen s) {
  switch (s) {
    case Screen::Boot: return "boot";
    case Screen::Offline: return "offline";
    case Screen::Connecting: return "connecting";
    case Screen::Pairing: return "pairing";
    case Screen::Ready: return "ready";
    case Screen::Listening: return "listening";
    case Screen::Thinking: return "thinking";
    case Screen::Responding: return "responding";
    case Screen::Card: return "card";
    case Screen::Image: return "image";
    case Screen::Error: return "error";
    case Screen::Prompt: return "prompt";
    case Screen::Updating: return "updating";
    case Screen::Settings: return "settings";
    case Screen::Setup: return "setup";
    case Screen::Dashboard: return "dashboard";
    case Screen::Profiles: return "profiles";
    case Screen::Alarm: return "alarm";
    case Screen::Clock: return "clock";
    case Screen::LowBattery: return "low_battery";
    case Screen::Charging: return "charging";
    case Screen::Charged: return "charged";
  }
  return "unknown";
}

Ui::Ui(Display& display)
    : display_(display), panel_(display.info()), info_(panel_), bg_(panel_.emissive ? 0 : kBg) {
  if (panel_.round) {
    // The largest square inside the circle; the corners of the panel do not exist.
    const int side = std::min(panel_.width, panel_.height) * 707 / 1000;
    ox_ = (panel_.width - side) / 2;
    oy_ = (panel_.height - side) / 2;
    info_.width = info_.height = static_cast<uint16_t>(side);
  }
  int w = info_.width, h = info_.height;
  int s = std::max(1, std::min(4, std::min(w / 160, h / 120)));
  // A board can ask for larger text (font_scale multiplies the automatic pick);
  // clamp to the same 1..4 range every layout below assumes.
  s = std::min(4, s * std::max<uint8_t>(1, panel_.font_scale));
  layout_.scale = s;
  const int bar_h = Canvas::line_height(s) + 2 * s;
  // A round panel's top band holds the link dot and one line of the settings
  // label: 2r + 14s, and never under 36 px so the hold target stays finger-sized.
  const int dot_r = std::max(2, 3 * s / 2 + 1);
  layout_.top_h = panel_.round ? std::max(36, 2 * dot_r + 14 * s) : bar_h;
  layout_.bottom_h = bar_h;
  layout_.title_w = panel_.round ? std::min(w, 120 * s) : w;
  layout_.header_h = Canvas::line_height(s + 1) + 4 * s;
  layout_.main_y = layout_.top_h;
  layout_.main_h = h - layout_.top_h - layout_.bottom_h;
  int margin = 4 * s;
  int content_h = h - layout_.top_h - layout_.header_h - layout_.bottom_h;
  layout_.body_cols = cols_for(w - 2 * margin, s);
  layout_.body_rows = std::max(1, (content_h - margin) / Canvas::line_height(s));

  // Hero layout: the mascot is sized per screen (see hero_geom).
  layout_.hero_cols = cols_for(w - 4 * s, s);
  // The headline is centred, so keeping clear of the marker on the right costs the same on the left.
  layout_.name_cols = cols_for(2 * (name_marker_x() - s) - w, s);
  if (panel_.round) {
    // The edge ring's band (draw_frame): a ring 2s + 2 px in from the glass, 3s + 2 px thick.
    const int R = std::min(panel_.width, panel_.height) / 2;
    band_out_ = R - (2 * s + 2);
    band_in_ = std::max(0, band_out_ - (3 * s + 2));
    const int cx = panel_.width / 2, cy = panel_.height / 2;
    // Each row's runs (at most two) of the pixels `in` takes.
    const auto runs = [&](std::vector<BandRow>& rows, auto in) {
      rows.assign(static_cast<size_t>(panel_.height), BandRow{});
      for (int y = 0; y < panel_.height; ++y) {
        BandRow& row = rows[static_cast<size_t>(y)];
        const int dy = y - cy;
        int run = -1;  // the run being filled
        bool inside = false;
        for (int x = 0; x < panel_.width; ++x) {
          const int dx = x - cx;
          const bool here = in(dx * dx + dy * dy);
          if (here && !inside) {
            run = std::min(run + 1, 1);
            row.x0[run] = static_cast<int16_t>(x);
          }
          if (here) row.x1[run] = static_cast<int16_t>(x + 1);
          inside = here;
        }
      }
    };
    runs(band_rows_, [&](int d2) { return d2 <= band_out_ * band_out_ && d2 > band_in_ * band_in_; });
    // Each degree's rectangle of band pixels, by the angle ring_pixel reads (comet_box).
    const RingBox none{static_cast<int16_t>(panel_.width), static_cast<int16_t>(panel_.height), 0, 0};
    ring_degrees_.assign(360, none);
    for (int y = 0; y < panel_.height; ++y) {
      const BandRow& row = band_rows_[static_cast<size_t>(y)];
      for (int run = 0; run < 2; ++run)
        for (int x = row.x0[run]; x < row.x1[run]; ++x) {
          RingBox& b = ring_degrees_[static_cast<size_t>(ring_angle64(x - cx, y - cy) / 64)];
          b.x0 = std::min(b.x0, static_cast<int16_t>(x));
          b.y0 = std::min(b.y0, static_cast<int16_t>(y));
          b.x1 = std::max(b.x1, static_cast<int16_t>(x + 1));
          b.y1 = std::max(b.y1, static_cast<int16_t>(y + 1));
        }
    }
    // The resting clock's ring takes the band and the seconds dot's reach, which is wider:
    // radii within the dot's radius, 4s, of the band's middle, plus a pixel. In half pixels,
    // so every bound is a whole number: the reach is 8s plus 2. Past the band, ring_pixel
    // paints only the dot (or the background), so the ticks stay inside it.
    const int mid2 = band_in_ + band_out_ + 1, reach2 = 8 * s + 2;
    runs(clock_rows_, [&](int d2) {
      return 4 * d2 >= (mid2 - reach2) * (mid2 - reach2) && 4 * d2 <= (mid2 + reach2) * (mid2 + reach2);
    });
    turn_ticks(0);  // the resting clock's ticks, until a minute turns them (set_ring)
  }
}

void Ui::invalidate() { valid_ = false; }

Canvas Ui::canvas() {
  // UI row y is panel row y + oy_. The framebuffer holds panel rows starting at
  // frame_origin_, so row y lives at buffer row y + oy_ - frame_origin_, which is
  // what base_y_ = frame_origin_ - oy_ makes Canvas index arithmetic produce.
  const int rows = display_.framebuffer_rows();
  return Canvas(display_.framebuffer() + ox_, info_.width, info_.height, info_.swap_bytes, panel_.width,
                frame_origin_ - oy_, rows > 0 ? rows : panel_.height);
}

void Ui::flush(int y0, int y1) { push(y0 + oy_, y1 + oy_); }

Canvas Ui::panel_canvas() {
  const int rows = display_.framebuffer_rows();
  return Canvas(display_.framebuffer(), panel_.width, panel_.height, panel_.swap_bytes, 0, frame_origin_,
                rows > 0 ? rows : panel_.height);
}

template <typename Draw>
void Ui::each_window(int y0, int y1, Draw&& draw) {
  // Windows tile the panel, `step` rows at a time (the whole panel when the
  // framebuffer holds it).
  const int win = display_.framebuffer_rows();
  const int step = win > 0 ? win : panel_.height;
  for (int top = 0; top < panel_.height; top += step) {
    const int a = std::max(y0, top), b = std::min(y1, top + step);
    if (a >= b) continue;
    frame_origin_ = top;
    display_.set_frame_origin(frame_origin_);
    draw(a, b);
    push(a, b);
  }
}

void Ui::draw_bands(int y0, int y1, void (Ui::*draw)(Canvas&, const UiModel&), const UiModel& m) {
  // UI row y sits at panel row y + oy_.
  const bool banded = display_.framebuffer_rows() > 0;
  y1 = std::min<int>(y1, info_.height);
  each_window(y0 + oy_, y1 + oy_, [&](int a, int b) {
    if (banded && ox_) {
      // The rows go out full width, and this window last held other rows: put
      // back the background beside the UI square, which a whole-panel
      // framebuffer keeps from the full repaint (push then paints the ring).
      Canvas panel = panel_canvas();
      panel.fill_rect(0, a, ox_, b - a, bg_);
      panel.fill_rect(ox_ + info_.width, a, panel_.width - ox_ - info_.width, b - a, bg_);
    }
    Canvas c = canvas();
    c.set_clip_rows(a - oy_, b - oy_);  // the canvas drops anything past the window as well
    (this->*draw)(c, m);
  });
}

void Ui::push(int y0, int y1) {
  paint_ring(y0, y1);  // the ring goes over whatever these rows drew
  send(y0, y1);
}

void Ui::send(int y0, int y1) {
  if (!stats_) {
    display_.flush(static_cast<uint16_t>(y0), static_cast<uint16_t>(y1));
    return;
  }
  const uint64_t t0 = clock_->now_us();
  display_.flush(static_cast<uint16_t>(y0), static_cast<uint16_t>(y1));
  flush_us_ += clock_->now_us() - t0;
  ++bands_;
}

void Ui::measure(FrameStats* stats, System* clock) {
  stats_ = clock ? stats : nullptr;
  clock_ = clock;
  input_pending_ = false;
}

void Ui::mark_input() {
  if (!stats_ || input_pending_) return;  // the first input waiting is the one timed
  input_at_ = clock_->now_us();
  input_pending_ = true;
}

void Ui::render(const UiModel& m) {
  if (!stats_) {
    draw_frame(m);
    return;
  }
  flush_us_ = 0;
  bands_ = 0;
  const uint64_t t0 = clock_->now_us();
  draw_frame(m);
  const uint64_t t1 = clock_->now_us();
  auto us = [](uint64_t d) { return static_cast<uint32_t>(std::min<uint64_t>(d, UINT32_MAX)); };
  stats_->add_frame(us(t1 - t0), us(flush_us_), bands_);
  if (input_pending_ && bands_) {
    stats_->add_input(us(t1 - input_at_));
    input_pending_ = false;
  }
}

void Ui::draw_frame(const UiModel& m) {
  // A display that holds only a band of rows cannot resend rows it missed, so it
  // asks for the whole screen again rather than leave a stale band on the panel.
  if (display_.take_redraw_request()) valid_ = false;
  // The edge ring (round panels). One that appears or goes repaints the panel once: without a
  // ring the bands own its pixels in the UI square's corners. A banded framebuffer no longer
  // holds the rows a change would resend, so any change repaints it. Otherwise only the
  // band's pixels that changed go out, after whatever this frame draws (update_ring), and a
  // comet's step computes only the pixels it lights where it was or where it is now.
  const EdgeRing ring = panel_.round ? edge_ring(m) : EdgeRing{};
  if ((ring.kind == EdgeRing::Kind::None) != (ring_.kind == EdgeRing::Kind::None)) valid_ = false;
  if (ring != ring_ && display_.framebuffer_rows() > 0) valid_ = false;
  bool ring_moved = valid_ && ring != ring_;
  const RingBox moved = ring_moved && ring_.kind == EdgeRing::Kind::Comet && ring.kind == EdgeRing::Kind::Comet
                            ? comet_box(ring_.head, ring.head)
                            : RingBox{0, 0, static_cast<int16_t>(panel_.width), static_cast<int16_t>(panel_.height)};
  // On the clock only the seconds dot moves from one second to the next: where it was. A new
  // minute turns the ticks too, but then draw_clock repaints the whole panel, ring included
  // (its hash keys on the minute), and the dot's rows hold nothing more to change.
  const bool dot_moved = ring_moved && ring_.kind == EdgeRing::Kind::Ticks && ring.kind == EdgeRing::Kind::Ticks;
  const int32_t dot_was = ring_dot_y16_;
  set_ring(ring);
  if (m.screen == Screen::Alarm) {
    draw_alarm(m);
    return;
  }
  if (m.screen == Screen::Clock) {
    draw_clock(m);  // the face stays within a minute
    if (dot_moved) {
      // Only the rows of the dot's old and new places change: its radius, whole pixels,
      // either side of each centre's row.
      const int cy = panel_.height / 2, reach = isqrt(ring_dot_r2_) / 16;
      const int32_t top = std::min(dot_was, ring_dot_y16_), bottom = std::max(dot_was, ring_dot_y16_);
      update_ring({0, static_cast<int16_t>(cy + top / 16 - reach), static_cast<int16_t>(panel_.width),
                   static_cast<int16_t>(cy + bottom / 16 + reach + 1)});
    } else if (ring_moved) {
      update_ring(moved);
    }
    return;
  }
  if (m.screen == Screen::LowBattery || m.screen == Screen::Charging || m.screen == Screen::Charged) {
    draw_battery_face(m);
    return;
  }
  if (full_face_ != FullFace::None) {  // a face painted the whole panel: everything comes back
    full_face_ = FullFace::None;
    valid_ = false;
    ring_moved = false;
  }
  const int h = info_.height;
  if (!valid_ && (ox_ || oy_)) {
    // Round panel: everything outside the UI area stays the background colour.
    // Each push paints the ring too.
    each_window(0, panel_.height, [&](int, int) { panel_canvas().fill_rect(0, 0, panel_.width, panel_.height, bg_); });
  }
  if (panel_.round) draw_round_battery(m);
  const int y_header = layout_.top_h;
  const int y_content = y_header + layout_.header_h;
  const int y_bottom = h - layout_.bottom_h;

  // A rectangular top bar's line fills with the timer: its width in pixels, or -1 with no timer.
  const int top_fill = !panel_.round && m.timer_permille >= 0 ? info_.width * m.timer_permille / 1000 : -1;
  uint32_t hashes[4];
  hashes[0] = Hash().add(m.title).val(m.lang).val(m.link).val(panel_.round && m.screen == Screen::Settings).val(panel_.round && m.settings_hold)
                  .val(panel_.round ? int8_t(-1) : m.battery).val(!panel_.round && m.battery_charging)
                  .val(!panel_.round && m.battery_icon).val(!panel_.round && m.battery_text)
                  .val(!panel_.round && m.battery_low).val(top_fill).get();
  hashes[1] = Hash()
                  .val(m.screen)
                  .add(m.headline)
                  .val(animated(m.screen) ? m.frame : 0u)
                  .val(m.screen == Screen::Listening ? m.level : uint8_t(0))
                  .val(m.speaking)
                  .get();
  Hash content;
  content.val(m.screen).add(m.detail).add(m.body).add(m.code).val(m.scroll).val(m.color_test)
      .val(m.qr ? m.qr->size : 0);
  for (const DashRow& row : m.dash) content.add(row.label).add(row.value).val(row.tone).val(row.heading);
  if (m.screen == Screen::Settings) {
    content.val(m.settings_sel).val(m.settings_scroll);
    for (const SettingRow& row : m.settings) content.add(row.label).add(row.value).val(row.icon).val(row.kind).val(row.level);
  }
  if (m.screen == Screen::Profiles)
    content.val(m.grid).val(m.face_row).val(m.mascot_row).val(m.settings_row).add(m.face ? m.face->hash : std::string())
        .val(m.thumbs_rev).val(m.thumbs.size());
  hashes[2] = content.get();
  hashes[3] = Hash().add(m.hint).get();

  if (m.hero) {
    // Hero mode: top bar, one mascot band, bottom bar.
    for (int i : {0, 3}) {
      if (valid_ && hash_[i] == hashes[i]) continue;
      int y0 = i == 0 ? 0 : y_bottom, y1 = i == 0 ? y_header : h;
      draw_bands(y0, y1, i == 0 ? &Ui::draw_top : &Ui::draw_bottom, m);
      hash_[i] = hashes[i];
    }
    uint32_t stat = Hash().val(m.screen).add(m.headline).add(m.detail).add(m.yes).add(m.no).val(m.caption_lines)
                        .val(m.name_target).add(m.face ? m.face->hash : std::string()).get();
    uint32_t anim = hero_anim_key(m);
    int y0 = y_header, y1 = y_bottom;
    bool redraw = !valid_ || !hero_valid_ || stat != hero_static_;
    if (!redraw && anim != hero_anim_) {
      hero_anim_rows(m, y0, y1);  // only what can move
      redraw = true;
    }
    if (redraw) draw_bands(y0, y1, &Ui::draw_hero, m);
    hero_static_ = stat;
    hero_anim_ = anim;
    hero_valid_ = true;
    hash_[1] = hash_[2] = 0;  // header and content must repaint when hero mode ends
    valid_ = true;
    if (ring_moved) update_ring(moved);
    return;
  }
  hero_valid_ = false;

  struct Band {
    int y0, y1;
    void (Ui::*draw)(Canvas&, const UiModel&);
  };
  // The Profiles grid owns the header's rows too: it draws them with the content.
  const bool grid = m.screen == Screen::Profiles;
  if (grid) hashes[1] = Hash().val(hashes[1]).val(hashes[2]).get();
  const Band bands[4] = {
      {0, y_header, &Ui::draw_top},
      {y_header, grid ? y_bottom : y_content, grid ? &Ui::draw_profiles : &Ui::draw_header},
      {y_content, y_bottom, &Ui::draw_content},
      {y_bottom, h, &Ui::draw_bottom},
  };
  for (int i = 0; i < 4; ++i) {
    if (grid && i == 2) {
      hash_[i] = 0;
      continue;
    }
    // An image owns the area between the bars; the app draws and flushes it.
    if (m.screen == Screen::Image && (i == 1 || i == 2)) {
      hash_[i] = 0;
      continue;
    }
    if (valid_ && hash_[i] == hashes[i]) continue;
    draw_bands(bands[i].y0, bands[i].y1, bands[i].draw, m);
    hash_[i] = hashes[i];
  }
  valid_ = true;
  if (ring_moved) update_ring(moved);
}

void Ui::draw_top(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int w = info_.width;
  c.fill_rect(0, 0, w, layout_.top_h, panel_.round ? bg_ : kBar);
  uint16_t dot = kRed;
  i18n::Msg word = i18n::Msg::LinkOffline;
  switch (m.link) {
    case Link::Offline: dot = kRed; word = i18n::Msg::LinkNoNet; break;
    case Link::Network: dot = kYellow; word = i18n::Msg::LinkNet; break;
    case Link::Connecting: dot = kYellow; word = i18n::Msg::LinkConnecting; break;
    case Link::Online: dot = kGreen; word = i18n::Msg::LinkOnline; break;
  }
  const char* label = i18n::text(m.lang, word);
  int ty = s;
  if (panel_.round) {
    // The settings hold target: a dim label under the link dot, with no bar
    // behind it. The label line (2r + 4s .. 2r + 13s) fits the band.
    int r = std::max(2, 3 * s / 2 + 1);
    if (m.settings_hold) {
      const char* control =
          i18n::text(m.lang, m.screen == Screen::Settings ? i18n::Msg::BackToHermesTarget : i18n::Msg::SettingsTarget);
      c.text((w - Canvas::text_width(control, s)) / 2, 2 * r + 4 * s, control, s, kDim);
    }
    c.fill_circle(w / 2, s + r, r, dot);
    return;
  }
  // The timer's line along the bottom of the bar: the time left in kAccent, the rest dim.
  if (m.timer_permille >= 0) {
    const int t = s + 1, filled = w * m.timer_permille / 1000;
    c.fill_rect(0, layout_.top_h - t, filled, t, kAccent);
    c.fill_rect(filled, layout_.top_h - t, w - filled, t, kAccentDim);
  }
  const int pad = 3 * s + panel_.corner_inset;
  int label_w = Canvas::text_width(label, s);
  int label_x = w - pad - label_w;
  c.text(label_x, ty, label, s, kDim);
  int r = std::max(2, 3 * s / 2 + 1);
  const int dot_x = label_x - 3 * s - r;
  c.fill_circle(dot_x, layout_.top_h / 2, r, dot);
  int title_end = dot_x - r;
  if (m.battery >= 0) {
    title_end -= 4 * s + battery_width(m);
    draw_battery(c, m, title_end, layout_.top_h / 2);
  }
  int title_cols = cols_for(title_end - 3 * s - pad, s);
  c.text(pad, ty, fit(m.title, title_cols), s, kText);
}

// Round panels: the battery is centred at the top of the panel, above the UI square,
// just below the edge ring's band plus a small margin, whether a ring shows or not.
void Ui::draw_round_battery(const UiModel& m) {
  const int s = layout_.scale;
  const int bh = 7 * s;  // the taller of the icon (5s + 1) and the text (7s)
  const int hw = (m.battery >= 0 ? battery_width(m) : 0) / 2 + 2 * s;
  // The glass centre's row, and the circle the strip's top corners must lie in: the band's
  // inner edge, so the strip stays put as the ring comes and goes.
  const int half = panel_.width / 2;
  const int radius = band_in_;
  int y0 = 0;
  // The first row where the strip's top corners lie inside that circle.
  while (y0 < oy_ && (half - y0) * (half - y0) + hw * hw > radius * radius) ++y0;
  y0 = std::min(y0 + 3 * s, std::max(0, oy_ - bh));
  const int y1 = std::min<int>(oy_, y0 + bh);
  const uint32_t key = Hash().val(m.battery).val(m.battery_charging).val(m.battery_low).val(m.battery_icon)
                           .val(m.battery_text).val(y0).get();
  if (valid_ && key == battery_hash_) return;
  battery_hash_ = key;
  each_window(0, oy_, [&](int, int) {
    Canvas panel = panel_canvas();
    // Clear the band the strip can occupy at any level, so a shorter or hidden strip leaves nothing behind.
    panel.fill_rect(0, 0, panel_.width, oy_, bg_);
    if (m.battery >= 0) draw_battery(panel, m, (panel_.width - battery_width(m)) / 2, (y0 + y1) / 2);
  });
}

int Ui::battery_width(const UiModel& m) const {
  const int s = layout_.scale;
  const int icon = m.battery_icon ? 10 * s : 0;
  const int text = m.battery_text ? Canvas::text_width(std::to_string(m.battery) + "%", s) : 0;
  return icon + text + (icon && text ? 2 * s : 0);
}

// A battery outline with a terminal nub, filled to the level, then the percent.
void Ui::draw_battery(Canvas& c, const UiModel& m, int x, int cy) {
  const int s = layout_.scale;
  if (m.battery_icon) {
    const int bw = 9 * s, bh = 5 * s + 1, t = std::max(1, s / 2);
    const int y = cy - bh / 2;
    // Charging: a dim green fill under a bright bolt, so it shows at any level.
    const uint16_t fill = m.battery_charging ? kGreenDim : m.battery_low ? kRed : kText;
    c.fill_rect(x, y, bw, bh, kDim);
    c.fill_rect(x + t, y + t, bw - 2 * t, bh - 2 * t, panel_.round ? bg_ : kBar);
    c.fill_rect(x + bw, cy - s, s, 2 * s, kDim);
    const int inner = bw - 4 * t, inner_h = bh - 4 * t;
    const int level = std::max(m.battery > 0 ? 1 : 0, inner * std::min<int>(m.battery, 100) / 100);
    c.fill_rect(x + 2 * t, y + 2 * t, level, inner_h, fill);
    if (m.battery_charging) {
      static const char* const kBolt[] = {"....##.", "...##..", ".#####.", "..##...", ".##...."};
      const int cw = inner / 7, ch = std::max(1, inner_h / 5);
      if (cw > 0) {
        const int bx = x + 2 * t + (inner - 7 * cw) / 2, by = y + 2 * t + (inner_h - 5 * ch) / 2;
        for (int r = 0; r < 5; ++r)
          for (int col = 0; col < 7; ++col)
            if (kBolt[r][col] == '#') c.fill_rect(bx + col * cw, by + r * ch, cw, ch, kGreen);
      } else {
        c.fill_rect(x + 2 * t, y + 2 * t, inner, inner_h, kGreen);  // too small for a bolt
      }
    }
    x += 12 * s;
  }
  if (m.battery_text) {
    // Without the icon, the text itself turns green while charging.
    const uint16_t col = m.battery_charging ? (m.battery_icon ? kDim : kGreen) : m.battery_low ? kRed : kDim;
    c.text(x, cy - (font::kGlyphHeight * s) / 2, std::to_string(m.battery) + "%", s, col);
  }
}

void Ui::draw_indicator(Canvas& c, const UiModel& m, int cx, int cy, int r) {
  const int s = layout_.scale;
  auto spinner = [&](uint16_t color) {
    int dot_r = std::max(1, r / 5);
    // Eight positions around the circle; precomputed unit vectors * 100.
    static const int kDx[8] = {0, 71, 100, 71, 0, -71, -100, -71};
    static const int kDy[8] = {-100, -71, 0, 71, 100, 71, 0, -71};
    int ring_r = r - dot_r;
    int head = static_cast<int>(m.frame % 8);
    for (int i = 0; i < 8; ++i) {
      int age = (head - i + 8) % 8;
      uint16_t col = age == 0 ? color : (age < 3 ? kDim : kFaint);
      c.fill_circle(cx + kDx[i] * ring_r / 100, cy + kDy[i] * ring_r / 100, dot_r, col);
    }
  };
  auto bars = [&](uint16_t color, int level_pct) {
    const int n = 5;
    int bw = std::max(3, (2 * r) / (2 * n - 1));
    int x0 = cx - (n * bw + (n - 1) * bw) / 2;
    for (int i = 0; i < n; ++i) {
      int jitter = static_cast<int>((m.frame * 3 + static_cast<uint32_t>(i) * 5) % 7);
      int pct = std::max(10, level_pct * (4 + jitter) / 10);
      int bh = std::max(2 * s, std::min(2 * r, 2 * r * pct / 100));
      c.fill_rect(x0 + i * 2 * bw, cy - bh / 2, bw, bh, color);
    }
  };

  switch (m.screen) {
    case Screen::Boot:
    case Screen::Connecting:
    case Screen::Updating:
      spinner(kAccent);
      break;
    case Screen::Thinking:
      spinner(kBlue);
      break;
    case Screen::Offline:
    case Screen::Error: {
      c.fill_circle(cx, cy, r, kRed);
      int tw = Canvas::text_width("!", s);
      c.text(cx - tw / 2, cy - (7 * s) / 2, "!", s, bg_);
      break;
    }
    case Screen::Pairing:
      c.ring(cx, cy, r, s + 1, kAccent);
      c.fill_circle(cx, cy, r / 3, kAccent);
      break;
    case Screen::Ready: {
      int pulse = tri(m.frame, 24);  // 0..12
      c.fill_circle(cx, cy, r * (8 + pulse / 3) / 12, kAccent);
      break;
    }
    case Screen::Listening:
      bars(kGreen, std::max<int>(m.level, 8));
      break;
    case Screen::Responding:
      if (m.speaking) {
        bars(kAccent, 40 + tri(m.frame, 8) * 10);
      } else {
        c.fill_circle(cx, cy, r * 2 / 3, kAccent);
      }
      break;
    case Screen::Card:
      c.fill_round_rect(cx - r, cy - r, 2 * r, 2 * r, r / 3, kAccent);
      break;
    case Screen::Prompt: {
      c.fill_circle(cx, cy, r, kAccent);
      int tw = Canvas::text_width("?", s);
      c.text(cx - tw / 2, cy - (7 * s) / 2, "?", s, bg_);
      break;
    }
    case Screen::Dashboard: {
      // A 2x2 grid of tiles.
      const int g = std::max(1, r / 6), t = r - g / 2;
      c.fill_round_rect(cx - r, cy - r, t, t, s, kAccent);
      c.fill_round_rect(cx + g / 2 + (g & 1), cy - r, t, t, s, kAccent);
      c.fill_round_rect(cx - r, cy + g / 2 + (g & 1), t, t, s, kAccent);
      c.fill_round_rect(cx + g / 2 + (g & 1), cy + g / 2 + (g & 1), t, t, s, kAccent);
      break;
    }
    case Screen::Image:
    case Screen::Settings:
    case Screen::Setup:
    case Screen::Profiles:
    case Screen::Alarm:
    case Screen::Clock:
    case Screen::LowBattery:
    case Screen::Charging:
    case Screen::Charged:
      break;
  }
}

void Ui::draw_header(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int y0 = layout_.top_h;
  const int hh = layout_.header_h;
  c.fill_rect(0, y0, info_.width, hh, bg_);
  int r = hh / 2 - 2 * s;
  int cx = 4 * s + r;
  int cy = y0 + hh / 2;
  draw_indicator(c, m, cx, cy, r);
  int tx = cx + r + 4 * s;
  int hs = s + 1;
  int cols = cols_for(info_.width - tx - 2 * s, hs);
  c.text(tx, cy - (font::kGlyphHeight * hs) / 2, fit(m.headline, cols), hs, kText);
  c.fill_rect(4 * s, y0 + hh - 1, info_.width - 8 * s, 1, kFaint);
}

int Ui::body_rows(const UiModel& m) const {
  int rows = layout_.body_rows;
  if (m.detail.empty()) return rows;
  int detail = static_cast<int>(wrap_text(m.detail, layout_.body_cols).size());
  int max_detail = m.body.empty() ? rows : std::min(2, rows - 1);
  return rows - std::max(0, std::min(detail, max_detail));
}

void Ui::draw_content(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int w = info_.width;
  const int y0 = layout_.top_h + layout_.header_h;
  const int y1 = info_.height - layout_.bottom_h;
  const int margin = 4 * s;
  const int lh = Canvas::line_height(s);
  c.fill_rect(0, y0, w, y1 - y0, bg_);
  int y = y0 + margin;

  if (m.color_test) {
    const uint16_t colors[] = {rgb565(255, 0, 0), rgb565(0, 255, 0), rgb565(0, 0, 255),
                               rgb565(255, 255, 255), rgb565(0, 0, 0)};
    for (int i = 0; i < 5; ++i) {
      const int left = w * i / 5, right = w * (i + 1) / 5;
      c.fill_rect(left, y0, right - left, y1 - y0, colors[i]);
    }
    return;
  }

  if (m.qr && m.qr->ok() && draw_qr(c, m, y0, y1)) return;
  if (m.screen == Screen::Dashboard) {
    draw_dashboard(c, m, y0, y1);
    return;
  }
  if (m.screen == Screen::Settings && !m.settings.empty()) {
    draw_settings(c, m, y0, y1);
    return;
  }

  if (m.screen == Screen::Boot) {
    int big = s + 2;
    const char* name = "HERMES";
    int cy = (y0 + y1) / 2 - Canvas::line_height(big) / 2;
    c.text((w - Canvas::text_width(name, big)) / 2, cy, name, big, kAccent);
    int sy = cy + Canvas::line_height(big);
    c.text((w - Canvas::text_width("gadget", s)) / 2, sy, "gadget", s, kDim);
    if (!m.detail.empty()) {
      std::string d = fit(m.detail, layout_.body_cols);
      c.text((w - Canvas::text_width(d, s)) / 2, y1 - lh, d, s, kFaint);
    }
    return;
  }

  int rows = body_rows(m);
  if (!m.detail.empty()) {
    uint16_t dcol = m.screen == Screen::Error || m.screen == Screen::Offline ? kRed : kDim;
    if (m.screen == Screen::Thinking) dcol = kBlue;
    auto detail_lines = wrap_text(m.detail, layout_.body_cols);
    int shown = layout_.body_rows - rows;
    for (int i = 0; i < shown; ++i) {
      c.text(margin, y, detail_lines[static_cast<size_t>(i)], s, dcol);
      y += lh;
    }
  }
  if (m.body.empty() || rows <= 0) return;

  auto lines = wrap_text(m.body, layout_.body_cols);
  int total = static_cast<int>(lines.size());
  int max_first = std::max(0, total - rows);
  int first = m.scroll < 0 ? max_first : std::min(m.scroll, max_first);
  uint16_t col = m.screen == Screen::Ready ? kDim : kText;
  for (int i = 0; i < rows && first + i < total; ++i) {
    c.text(margin, y + i * lh, lines[static_cast<size_t>(first + i)], s, col);
  }
  if (total > rows) {
    // Scroll indicator along the right edge.
    int track_y = y, track_h = rows * lh;
    int thumb_h = std::max(2 * s, track_h * rows / total);
    int thumb_y = track_y + (track_h - thumb_h) * first / std::max(1, max_first);
    c.fill_rect(w - 2 * s, track_y, s, track_h, kFaint);
    c.fill_rect(w - 2 * s, thumb_y, s, thumb_h, kDim);
  }
}

int Ui::dash_rows() const {
  const int s = layout_.scale;
  const int content_h = info_.height - layout_.top_h - layout_.header_h - layout_.bottom_h;
  return std::max(1, (content_h - 4 * s) / (Canvas::line_height(s) + 2 * s));
}

// The dashboard: one row per session or tile, a dot in its state's colour, the
// label on the left and the value on the right. Headings start a section.
void Ui::draw_dashboard(Canvas& c, const UiModel& m, int y0, int y1) {
  const int s = layout_.scale;
  const int w = info_.width;
  const int margin = 4 * s;
  const int row_h = Canvas::line_height(s) + 2 * s;
  const int glyph_h = font::kGlyphHeight * s;
  const int rows = dash_rows();
  const int total = static_cast<int>(m.dash.size());
  const int max_first = std::max(0, total - rows);
  const int first = std::max(0, std::min(m.scroll, max_first));
  const bool scrolls = total > rows;
  const int right = w - margin - (scrolls ? 2 * s : 0);
  const int r = std::max(2, glyph_h / 3);
  int y = y0 + margin;
  for (int i = 0; i < rows && first + i < total; ++i, y += row_h) {
    const DashRow& row = m.dash[static_cast<size_t>(first + i)];
    const int ty = y + (row_h - glyph_h) / 2;
    if (row.heading) {
      c.text(margin, ty, fit(row.label, cols_for(right - margin, s)), s, kDim);
      c.fill_rect(margin, y + row_h - 1, right - margin, 1, kFaint);
      continue;
    }
    c.fill_circle(margin + r, ty + glyph_h / 2, r, tone_color(row.tone));
    const int lx = margin + 2 * r + 3 * s;
    const int cols = cols_for(right - lx, s);
    // The value keeps up to half the row; the label gets the rest, less a space.
    const std::string value = fit(row.value, std::min<int>(static_cast<int>(font::length(row.value)), cols / 2));
    const int value_cols = static_cast<int>(font::length(value));
    c.text(lx, ty, fit(row.label, cols - value_cols - (value_cols ? 1 : 0)), s, kText);
    if (value_cols) c.text(right - Canvas::text_width(value, s), ty, value, s, tone_color(row.tone));
  }
  if (scrolls) {
    const int track_y = y0 + margin, track_h = std::min(rows * row_h, y1 - track_y);
    const int thumb_h = std::max(2 * s, track_h * rows / total);
    const int thumb_y = track_y + (track_h - thumb_h) * first / std::max(1, max_first);
    c.fill_rect(w - 2 * s, track_y, s, track_h, kFaint);
    c.fill_rect(w - 2 * s, thumb_y, s, thumb_h, kDim);
  }
}

Ui::Rect Ui::name_rect(const UiModel& m) const {
  if (!m.name_target || !m.hero) return {};
  const int s = layout_.scale;
  const HeroGeom g = hero_geom(m);
  // The headline and the cue under it, padded to a finger's height.
  return {2 * s, g.caption_y - s, info_.width - 4 * s, g.name_h};
}

int Ui::name_marker_x() const {
  const int s = layout_.scale;
  return info_.width - 2 * s - 3 * s - Canvas::text_width("v", s);  // name_rect's right edge, less 3s
}

bool Ui::name_hit(const UiModel& m, int x, int y) const { return name_rect(m).has(x - ox_, y - oy_); }

namespace {
// An initial's colour, picked by the profile id so a profile keeps it on every board and page.
uint16_t initial_color(std::string_view id) {
  static constexpr uint16_t kColors[] = {rgb565(214, 90, 90),  rgb565(219, 129, 57), rgb565(176, 148, 40),
                                         rgb565(76, 160, 90),  rgb565(48, 150, 160), rgb565(70, 120, 210),
                                         rgb565(130, 100, 210), rgb565(200, 90, 160)};
  uint32_t h = 2166136261u;  // FNV-1a
  for (unsigned char ch : id) h = (h ^ ch) * 16777619u;
  return kColors[h % (sizeof(kColors) / sizeof(kColors[0]))];
}

// The first letter of a name, upper case when it is ASCII.
// A gear in a size*size square at (x, y): eight teeth round a wheel with a hole, on a muted disc.
void gear_icon(Canvas& c, int x, int y, int size) {
  const int cx = x + size / 2, cy = y + size / 2;
  c.fill_circle(cx, cy, size / 2, kFaint);
  static constexpr int kDir[8][2] = {{1000, 0}, {707, 707}, {0, 1000}, {-707, 707},
                                     {-1000, 0}, {-707, -707}, {0, -1000}, {707, -707}};
  const int wheel = size * 26 / 100, tooth = std::max(2, size * 8 / 100), reach = size * 30 / 100;
  for (const auto& d : kDir) c.fill_circle(cx + d[0] * reach / 1000, cy + d[1] * reach / 1000, tooth, kText);
  c.fill_circle(cx, cy, wheel, kText);
  c.fill_circle(cx, cy, std::max(2, size * 10 / 100), kFaint);
}

// `b` scaled to a size*size square by nearest pixel, its ink in `ink`.
void bitmap_scaled(Canvas& c, int x, int y, int size, const mascot::Bitmap& b, uint16_t ink) {
  if (size == b.size) {
    c.bitmap1(x, y, size, size, b.bits, ink);
    return;
  }
  for (int py = 0; py < size; ++py) {
    const int sy = py * b.size / size;
    for (int px = 0; px < size; ++px)
      if (b.ink(px * b.size / size, sy)) c.pixel(x + px, y + py, ink);
  }
}

// A name in at most two lines of `cols` characters; what does not fit ends the second line with "..".
std::vector<std::string> name_lines(const std::string& name, int cols) {
  std::vector<std::string> lines = wrap_text(name, cols);
  if (lines.size() > 2) {
    std::string rest = lines[1];
    for (size_t i = 2; i < lines.size(); ++i) rest += " " + lines[i];
    lines.resize(2);
    lines[1] = fit(rest, cols);  // greedy wrapping left more than a line: fit() cuts it with ".."
  }
  return lines;
}
}  // namespace

std::string profile_initial(std::string_view name) {
  size_t start = 0;
  while (start < name.size() && name[start] == ' ') ++start;
  name.remove_prefix(start);
  if (name.empty()) return "?";
  const size_t n = std::max<size_t>(1, font::prefix(name, 1));
  std::string out(name.substr(0, n));
  const auto b0 = static_cast<unsigned char>(out[0]);
  if (out.size() == 1 && b0 >= 'a' && b0 <= 'z') {
    out[0] = static_cast<char>(b0 - 'a' + 'A');
  } else if (out.size() == 2 && b0 == 0xC3) {  // Latin-1 small letters U+00E0..U+00FE, but not U+00F7
    const auto b1 = static_cast<unsigned char>(out[1]);
    if (b1 >= 0xA0 && b1 <= 0xBE && b1 != 0xB7) out[1] = static_cast<char>(b1 - 0x20);
  }
  return out;
}

// Cells at least a finger tall and wide, each with an icon and two lines of name under it. The page
// holds the grid the model asks for, or with few profiles the smallest grid that holds them all
// (bigger icons); a panel too small for it gets the largest smaller grid that fits.
Ui::ProfileGrid Ui::profile_grid(const UiModel& m) const {
  ProfileGrid g;
  const int n = static_cast<int>(m.dash.size());
  if (m.screen != Screen::Profiles || n == 0) return g;
  const int s = layout_.scale, y0 = layout_.top_h, y1 = info_.height - layout_.bottom_h;
  const int dots_h = 6 * s;
  const int band_w = info_.width - 4 * s, band_h = y1 - y0 - dots_h - s;
  const bool wide = band_w >= band_h;
  const bool landscape = 2 * band_w >= 3 * band_h;  // clearly wider than tall: 6 is three across
  const int min_icon = std::max(24, 12 * s);
  static constexpr int kGrids[] = {16, 9, 6, 4, 2, 1};
  int want = m.grid == 16 || m.grid == 6 || m.grid == 4 ? m.grid : 9;
  for (int k : {4, 6, 9})  // few profiles: the smallest grid that holds them all
    if (n <= k && k < want) want = k;
  for (int k : kGrids) {
    if (k > want) continue;
    int cols = k == 16 ? 4 : k == 9 ? 3 : k == 6 ? (landscape ? 3 : 2) : k == 4 ? 2 : k == 2 ? (wide ? 2 : 1) : 1;
    if (k == 4 && band_w >= 2 * band_h) cols = 4;  // a wide, short panel: one row of four
    const int rows = (k + cols - 1) / cols;
    const int cw = band_w / cols, ch = band_h / rows;
    {  // names keep the screen's text size: a grid that needs smaller letters does not fit
      const int ls = s;
      const int icon = std::min(cw - 4 * s, ch - 2 * Canvas::line_height(ls) - 3 * s);
      const bool ok = (cw >= touch_px_ && ch >= touch_px_ && icon >= min_icon && cols_for(cw - 4 * s, ls) >= 5) || k == 1;
      if (!ok) continue;
      g.cols = cols, g.rows = rows, g.cw = cw, g.ch = ch, g.label_scale = ls;
      g.icon = std::max(8, icon);
      g.x0 = (info_.width - cols * cw) / 2;
      g.y0 = y0 + s;
      g.dots_y = y1 - dots_h / 2;
      const int per = cols * rows;
      g.pages = (n + per - 1) / per;
      g.first = std::max(0, std::min(m.scroll, n - 1)) / per * per;
      g.count = std::min(per, n - g.first);
      return g;
    }
  }
  return g;
}

int Ui::profile_thumb_px(int cap) const {
  UiModel m;
  m.screen = Screen::Profiles;
  m.grid = 4;
  m.dash.resize(4);
  return std::min(cap, profile_grid(m).icon);
}

int Ui::profile_per_page(const UiModel& m) const {
  const ProfileGrid g = profile_grid(m);
  return g.cols * g.rows;
}

int Ui::profile_row_hit(const UiModel& m, int x, int y) const {
  const ProfileGrid g = profile_grid(m);
  for (int i = 0; i < g.count; ++i)
    if (g.cell(i).has(x - ox_, y - oy_)) return g.first + i;
  return -1;
}

void Ui::draw_profiles(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  c.fill_rect(0, layout_.top_h, info_.width, info_.height - layout_.bottom_h - layout_.top_h, bg_);
  const ProfileGrid g = profile_grid(m);
  const int ls = g.label_scale, lh = Canvas::line_height(ls), glyph_h = font::kGlyphHeight * ls;
  for (int i = 0; i < g.count; ++i) {
    const Rect r = g.cell(i);
    const int item = g.first + i;
    const DashRow& row = m.dash[static_cast<size_t>(item)];
    const bool current = row.tone == DashRow::Tone::Waiting;
    // A margin of two dots each side, so the names of cells side by side never touch.
    const std::vector<std::string> lines = name_lines(row.label, cols_for(r.w - 4 * s, ls));
    const int block = g.icon + 2 * s + static_cast<int>(lines.size()) * lh;
    const int ix = r.x + (r.w - g.icon) / 2, iy = r.y + std::max(s, (r.h - block) / 2);
    const int cx = ix + g.icon / 2, cy = iy + g.icon / 2;
    const uint16_t* thumb =
        static_cast<size_t>(item) < m.thumbs.size() && m.thumb_px ? m.thumbs[static_cast<size_t>(item)] : nullptr;
    if (item == m.settings_row) {
      gear_icon(c, ix, iy, g.icon);
    } else if (item == m.face_row && m.face && m.face->rgb565) {
      c.picture(ix, iy, g.icon, m.face->rgb565, m.face->px, nullptr, 0, 0);
    } else if (thumb && item != m.face_row) {  // the profile's own face or avatar, from Hermes
      c.picture(ix, iy, g.icon, thumb, m.thumb_px, nullptr, 0, 0);
    } else if (item == m.face_row || item == m.mascot_row) {
      const mascot::Face* face = item == m.face_row ? m.face : nullptr;
      bitmap_scaled(c, ix, iy, g.icon, mascot::pick(g.icon, mascot::Frame::Idle, face), kText);
    } else {
      c.fill_circle(cx, cy, g.icon / 2, initial_color(row.value));
      const std::string letter = profile_initial(row.label);
      const int big = std::max(1, g.icon * 9 / 20 / font::kGlyphHeight);
      c.text(cx - Canvas::text_width(letter, big) / 2, cy - font::kGlyphHeight * big / 2, letter, big, kText);
    }
    if (current) c.ring(cx, cy, g.icon / 2 + s + 1, std::max(2, s), kAccent);
    int ty = iy + g.icon + 2 * s;
    for (const std::string& line : lines) {
      c.text(r.x + (r.w - Canvas::text_width(line, ls)) / 2, ty + (lh - glyph_h) / 2, line, ls, current ? kAccent : kText);
      ty += lh;
    }
  }
  if (g.pages > 1) {  // which page this is: one dot per page
    const int dr = std::max(2, s + 1), step = 4 * dr, x = (info_.width - (g.pages - 1) * step) / 2;
    for (int p = 0; p < g.pages; ++p)
      c.fill_circle(x + p * step, g.dots_y, dr, p == g.first / (g.cols * g.rows) ? kAccent : kFaint);
  }
}

// Settings icons: 8x8, one byte per row, most significant bit on the left, in
// SettingRow::Icon order. Drawn at the text scale, so they match the font's pixels.
constexpr uint8_t kSettingIcons[][8] = {
    {0x08, 0x1A, 0xF1, 0xF5, 0xF5, 0xF1, 0x1A, 0x08},  // Volume
    {0x10, 0x44, 0x38, 0xBA, 0x38, 0x44, 0x10, 0x00},  // Sun
    {0x7E, 0x81, 0xA9, 0x81, 0x7E, 0x20, 0x40, 0x00},  // Talk
    {0x18, 0x3C, 0x3C, 0x18, 0x00, 0x7E, 0xFF, 0xFF},  // Person
    {0x18, 0x3C, 0x3C, 0x3C, 0xBD, 0x42, 0x3C, 0x18},  // Mic
    {0x1E, 0x12, 0x12, 0x12, 0x76, 0xFF, 0x66, 0x00},  // Note
    {0xFF, 0x81, 0x81, 0x81, 0xFF, 0x18, 0x7E, 0x00},  // Screen
    {0x7E, 0x81, 0xA5, 0x81, 0x99, 0x81, 0x7E, 0x00},  // Keys
    {0x3C, 0x42, 0x99, 0x81, 0x99, 0x99, 0x42, 0x3C},  // Info
    {0x3C, 0x7E, 0x42, 0x42, 0x7E, 0x7E, 0x7E, 0x7E},  // Battery
    {0xFF, 0x42, 0x24, 0x18, 0x18, 0x3C, 0x7E, 0xFF},  // Timer
    {0x10, 0x28, 0x28, 0x2E, 0xEB, 0x80, 0x81, 0x7E},  // Touch
    {0x3C, 0x46, 0x8C, 0x8C, 0x86, 0x42, 0x3C, 0x00},  // FaceDown
    {0x3C, 0x52, 0x91, 0x9D, 0x81, 0x42, 0x3C, 0x00},  // Clock
    {0x10, 0x54, 0x92, 0x92, 0x82, 0x44, 0x38, 0x00},  // Power
    {0x00, 0x7E, 0x81, 0x3C, 0x42, 0x18, 0x18, 0x00},  // Wifi
    {0x7C, 0x44, 0x44, 0x44, 0x44, 0x6C, 0x7C, 0x00},  // Phone
    {0x3C, 0x5A, 0x99, 0xFF, 0x99, 0x5A, 0x3C, 0x00},  // Globe
    {0x10, 0x20, 0x40, 0xFF, 0x40, 0x20, 0x10, 0x00},  // Back
    {0x18, 0x3C, 0x7E, 0x7E, 0x7E, 0xFF, 0x00, 0x18},  // Bell
    {0x0C, 0x18, 0x30, 0x7E, 0x0C, 0x18, 0x30, 0x00},  // Bolt
    {0x00, 0x01, 0x03, 0x86, 0xCC, 0x78, 0x30, 0x00},  // Check
    {0xDB, 0xDB, 0x00, 0xDB, 0xDB, 0x00, 0xDB, 0xDB},  // Grid
};

void draw_icon(Canvas& c, int x, int y, SettingRow::Icon icon, int scale, uint16_t color) {
  const size_t i = static_cast<size_t>(icon);
  if (i >= sizeof(kSettingIcons) / sizeof(kSettingIcons[0])) return;
  for (int row = 0; row < 8; ++row) {
    const uint8_t bits = kSettingIcons[i][row];
    for (int col = 0; col < 8; ++col)
      if ((bits >> (7 - col)) & 1) c.fill_rect(x + col * scale, y + row * scale, scale, scale, color);
  }
}

// The settings list: rows of a fixed height, except the selected one, which also
// holds its description. Coordinates are relative to the list's top.
int settings_row_base(int s, int touch_px) { return std::max(Canvas::line_height(s) + 6 * s, touch_px * 2 / 3); }

// The columns a Value row's text gets at the right: what the label leaves, at least half the row.
int settings_value_cols(const SettingRow& row, int row_cols) {
  const int room = row_cols - static_cast<int>(font::length(row.label)) - 1;
  return std::max(std::min(room, static_cast<int>(font::length(row.value))), row_cols / 2);
}

std::vector<std::string> Ui::settings_desc(const UiModel& m) const {
  if (m.settings_sel < 0 || m.settings_sel >= static_cast<int>(m.settings.size())) return {};
  const SettingRow& row = m.settings[static_cast<size_t>(m.settings_sel)];
  const int s = layout_.scale;
  std::string_view body = m.body;
  // The value already shows at the right: drop the body's first line when it repeats it.
  const size_t nl = body.find('\n');
  // A slider's first line is its percentage, which may be a slide's preview. A value shortened
  // to fit its row keeps its full line here (the row's width as drawn with a scroll bar).
  const int row_cols = cols_for(info_.width - 15 * s - 7 * s, s);
  const bool whole = static_cast<int>(font::length(row.value)) <= settings_value_cols(row, row_cols);
  if (row.kind == SettingRow::Kind::Slider || (!row.value.empty() && whole && body.substr(0, nl) == row.value))
    body = nl == std::string_view::npos ? "" : body.substr(nl + 1);
  return wrap_text(std::string(body), cols_for(info_.width - 22 * s, s));  // the text column, beside a scroll bar
}

int Ui::settings_view_h() const { return info_.height - layout_.bottom_h - layout_.top_h - layout_.header_h; }

int Ui::settings_row_h(const UiModel& m, int i) const {
  const int s = layout_.scale, base = settings_row_base(s, touch_px_);
  if (i != m.settings_sel) return base;
  const int lines = static_cast<int>(settings_desc(m).size()) +
                    (m.settings[static_cast<size_t>(i)].kind == SettingRow::Kind::Slider);
  return base + (lines ? lines * Canvas::line_height(s) + 2 * s : 0);
}

int Ui::settings_row_top(const UiModel& m, int i) const {
  const int s = layout_.scale, base = settings_row_base(s, touch_px_);
  // Rows above the selected one are all the base height.
  int top = 2 * s + i * (base + s);
  if (i > m.settings_sel && m.settings_sel >= 0) top += settings_row_h(m, m.settings_sel) - base;
  return top;
}

int Ui::settings_max_scroll(const UiModel& m) const {
  const int n = static_cast<int>(m.settings.size());
  if (n == 0) return 0;
  return std::max(0, settings_row_top(m, n - 1) + settings_row_h(m, n - 1) + 2 * layout_.scale - settings_view_h());
}

int Ui::settings_scroll_for(const UiModel& m, int scroll) const {
  if (m.settings_sel < 0 || m.settings_sel >= static_cast<int>(m.settings.size())) return 0;
  const int s = layout_.scale, view = settings_view_h();
  const int base = settings_row_base(s, touch_px_);
  // The row and one more row of context each way, when there is room.
  const int top = settings_row_top(m, m.settings_sel) - 2 * s - (m.settings_sel > 0 ? base / 2 : 0);
  const int bottom = settings_row_top(m, m.settings_sel) + settings_row_h(m, m.settings_sel) + 2 * s +
                     (m.settings_sel + 1 < static_cast<int>(m.settings.size()) ? base / 2 : 0);
  if (bottom - scroll > view) scroll = bottom - view;
  if (top < scroll) scroll = top;
  const int max_scroll = settings_max_scroll(m);
  if (scroll <= 0 || scroll >= max_scroll) return std::max(0, std::min(scroll, max_scroll));
  // At rest the top row shows whole: round down the list to the next row's top.
  // The selected row's own top is one, so it stays in view.
  for (int i = 0; i < static_cast<int>(m.settings.size()); ++i) {
    const int edge = settings_row_top(m, i) - 2 * s;
    if (edge >= scroll) return std::min(edge, max_scroll);
  }
  return max_scroll;
}

int Ui::settings_row_hit(const UiModel& m, int x, int y) const {
  x -= ox_;
  y -= oy_ + layout_.top_h + layout_.header_h;
  if (m.screen != Screen::Settings || x < 0 || x >= info_.width || y < 0 || y >= settings_view_h()) return -1;
  y += m.settings_scroll;
  for (int i = 0; i < static_cast<int>(m.settings.size()); ++i) {
    const int top = settings_row_top(m, i);
    if (y >= top && y < top + settings_row_h(m, i)) return i;
  }
  return -1;
}

// The selected slider row's bar, in area coordinates: x and w span it, y is its centre line.
Ui::Rect Ui::settings_bar(const UiModel& m) const {
  if (m.screen != Screen::Settings || m.settings_sel < 0 || m.settings_sel >= static_cast<int>(m.settings.size()) ||
      m.settings[static_cast<size_t>(m.settings_sel)].kind != SettingRow::Kind::Slider)
    return {};
  const int s = layout_.scale, lh = Canvas::line_height(s), base = settings_row_base(s, touch_px_);
  const int bar_w = settings_max_scroll(m) > 0 ? 3 * s : 0;
  const int bx = 18 * s, right = info_.width - 7 * s - bar_w;  // inset 3s from the text columns (draw_settings)
  const int top = layout_.top_h + layout_.header_h + settings_row_top(m, m.settings_sel) -
                  std::max(0, std::min(m.settings_scroll, settings_max_scroll(m)));
  return {bx, top + base - 2 * s + lh / 2, right - bx, 0};
}

int Ui::settings_slider_at(const UiModel& m, int x, int y, bool anywhere) const {
  const Rect bar = settings_bar(m);
  if (bar.w <= 0) return -1;
  x -= ox_;
  y -= oy_;
  // A line's height around the bar, within the list's band; once a slide has begun, any height.
  const int reach = Canvas::line_height(layout_.scale);
  const int top = layout_.top_h + layout_.header_h, bottom = top + settings_view_h();
  if (!anywhere && (bar.y < top || bar.y >= bottom || y < std::max(top, bar.y - reach) ||
                    y > std::min(bottom - 1, bar.y + reach) || x < bar.x - reach || x >= bar.x + bar.w + reach))
    return -1;
  return std::max(0, std::min(100, (x - bar.x) * 100 / std::max(1, bar.w)));
}

void Ui::draw_settings(Canvas& c, const UiModel& m, int y0, int y1) {
  const int s = layout_.scale, w = info_.width, lh = Canvas::line_height(s);
  const int glyph_h = font::kGlyphHeight * s, base = settings_row_base(s, touch_px_);
  const int max_scroll = settings_max_scroll(m);
  const int scroll = std::max(0, std::min(m.settings_scroll, max_scroll));
  const int bar_w = max_scroll > 0 ? 3 * s : 0;  // room for the scroll indicator
  const int icon_x = 4 * s, text_x = icon_x + 11 * s, right = w - 4 * s - bar_w;
  // Rows scrolled part way out of the band must not touch the bars around it.
  const int clip0 = c.clip_y0(), clip1 = c.clip_y1();
  c.set_clip_rows(std::max(clip0, y0), std::min(clip1, y1));
  // Only the rows that cross the band; the rest cost nothing.
  for (int i = 0; i < static_cast<int>(m.settings.size()); ++i) {
    const int top = y0 + settings_row_top(m, i) - scroll, h = settings_row_h(m, i);
    if (top >= y1) break;
    if (top + h <= y0) continue;
    const SettingRow& row = m.settings[static_cast<size_t>(i)];
    const bool sel = i == m.settings_sel;
    if (sel) c.fill_round_rect(2 * s, top, w - 4 * s - bar_w, h, 3 * s, kBar);
    const int cy = top + base / 2;
    draw_icon(c, icon_x, cy - 4 * s, row.icon, s, sel ? kAccent : kDim);
    const int ty = cy - glyph_h / 2;
    // The control at the right edge, then the label in what is left.
    int ctrl_x = right;
    switch (row.kind) {
      case SettingRow::Kind::Toggle: {
        const int ph = 8 * s, pw = 15 * s, on = row.level != 0;
        ctrl_x = right - pw;
        c.fill_round_rect(ctrl_x, cy - ph / 2, pw, ph, ph / 2, on ? kGreen : kFaint);
        c.fill_circle(on ? right - ph / 2 : ctrl_x + ph / 2, cy, ph / 2 - s, on ? kText : kDim);
        break;
      }
      case SettingRow::Kind::Slider:  // the percentage; the selected row draws its bar underneath
        ctrl_x = right - Canvas::text_width(row.value, s);
        c.text(ctrl_x, ty, row.value, s, sel ? kAccent : kDim);
        break;
      case SettingRow::Kind::Value: {
        const std::string v = fit(row.value, settings_value_cols(row, cols_for(right - text_x, s)));
        ctrl_x = right - Canvas::text_width(v, s);
        c.text(ctrl_x, ty, v, s, sel ? kAccent : kDim);
        break;
      }
      case SettingRow::Kind::Action:
        ctrl_x = right - Canvas::text_width(">", s);
        c.text(ctrl_x, ty, ">", s, sel ? kAccent : kFaint);
        break;
    }
    c.text(text_x, ty, fit(row.label, cols_for(ctrl_x - 3 * s - text_x, s)), s, kText);
    if (sel) {
      int dy = top + base - 2 * s;
      if (row.kind == SettingRow::Kind::Slider) {
        const Rect bar = settings_bar(m);
        const int bx = bar.x, bw = bar.w, by = bar.y;
        const int fill = bw * std::min<int>(100, row.level) / 100;
        c.fill_round_rect(bx, by - s, bw, 2 * s, s, kFaint);
        if (fill > 0) c.fill_round_rect(bx, by - s, fill, 2 * s, s, kAccent);
        c.fill_circle(bx + fill, by, 3 * s, kText);
        dy += lh + s;
      }
      for (const std::string& line : settings_desc(m)) {
        c.text(text_x, dy, line, s, kDim);
        dy += lh;
      }
    }
  }
  if (max_scroll > 0) {
    const int view = y1 - y0, total = view + max_scroll;
    const int thumb_h = std::max(4 * s, view * view / total);
    const int thumb_y = y0 + (view - thumb_h) * scroll / max_scroll;
    c.fill_rect(w - 2 * s, y0, s, view, kFaint);
    c.fill_rect(w - 2 * s, thumb_y, s, thumb_h, kDim);
  }
  c.set_clip_rows(clip0, clip1);
}

// A Wi-Fi setup code: a white panel with a four-module quiet zone, one square
// per module, at the largest integer scale that fits the content band. A phone
// camera needs the quiet zone and dark-on-light contrast, so neither is optional.
// Returns false, drawing nothing, when the code does not fit beside the text.
bool Ui::draw_qr(Canvas& c, const UiModel& m, int y0, int y1) {
  const int n = m.qr->size;
  const int w = info_.width;
  const int s = layout_.scale;
  const int lh = Canvas::line_height(s);
  const int margin = 4 * s;
  const int pad = 4;                     // quiet zone, in modules
  const int cells = n + 2 * pad;         // the code plus its quiet zone
  const int avail_h = y1 - y0 - 2 * margin;

  // The code only joins the network: the phone still needs the address, and a
  // phone that cannot scan needs the name and password. So every line of the
  // instructions stays on screen, and the code is shown only where it fits
  // beside them at kMinScale or larger, below which it is not reliably
  // scannable. Otherwise the caller draws the text-only screen.
  constexpr int kMinScale = 3;
  const std::vector<std::string> lines = wrap_text(m.body, layout_.body_cols);
  const int text_rows = static_cast<int>(lines.size());
  const int reserve = text_rows ? text_rows * lh + 2 * s : 0;
  const int scale = std::min((w - 2 * margin) / cells, (avail_h - reserve) / cells);
  if (scale < kMinScale) return false;

  const int side = cells * scale;
  const int x0 = (w - side) / 2;
  const int block = side + reserve;
  const int py0 = y0 + margin + std::max(0, (avail_h - block) / 2);
  c.fill_rect(x0, py0, side, side, rgb565(255, 255, 255));
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      if (m.qr->at(x, y)) {
        c.fill_rect(x0 + (x + pad) * scale, py0 + (y + pad) * scale, scale, scale, rgb565(0, 0, 0));
      }
    }
  }
  int ty = py0 + side + 2 * s;
  for (int i = 0; i < text_rows; ++i) {
    c.text((w - Canvas::text_width(lines[static_cast<size_t>(i)], s)) / 2, ty, lines[static_cast<size_t>(i)], s, kDim);
    ty += lh;
  }
  return true;
}

Ui::HeroGeom Ui::hero_geom(const UiModel& m) const {
  HeroGeom g;
  const int s = layout_.scale, w = info_.width, lh = Canvas::line_height(s);
  const int cols = layout_.hero_cols;
  if (!m.detail.empty()) g.detail = wrap_text(m.detail, cols);
  const int buttons_h = m.yes.empty() && m.no.empty() ? 0 : lh + 6 * s;
  auto caption_h = [&](int lines) { return (1 + lines) * lh + s; };
  // Never shrink the mascot below the smallest bitmap: drop caption lines instead.
  const int smallest = mascot::pick(0, mascot::Frame::Idle)->size;
  int max_lines = std::max(1, static_cast<int>(m.caption_lines));
  while (max_lines > 1 && layout_.main_h - caption_h(max_lines) - buttons_h - 4 * s < smallest) --max_lines;
  if (static_cast<int>(g.detail.size()) > max_lines) {
    g.detail.resize(static_cast<size_t>(max_lines));
    std::string& last = g.detail.back();
    // Columns are letters, not bytes: an accented line must not be cut early or mid-letter.
    if (static_cast<int>(font::length(last)) + 2 > cols)
      last.resize(font::prefix(last, static_cast<size_t>(std::max(0, cols - 2))));
    last += "..";
  }
  // Reserve one detail line even when there is none, so the mascot keeps its size; a spoken reply's
  // caption reserves all its lines, so the face keeps its size as the text arrives. A name
  // panel (headline and first detail line) grows to a finger's height.
  g.name_h = m.name_target ? std::max(2 * lh + s, touch_px_) : 0;
  const int reserved = m.screen == Screen::Responding ? max_lines : 1;
  const int ch = caption_h(std::max<int>(reserved, static_cast<int>(g.detail.size()))) + std::max(0, g.name_h - (2 * lh + s));
  const int avail = std::min(w - 8 * s, layout_.main_h - ch - buttons_h - 4 * s);
  g.size = mascot::pick(avail, mascot::Frame::Idle)->size;
  const int block = g.size + 2 * s + ch + buttons_h;
  g.x = (w - g.size) / 2;
  g.y = layout_.main_y + std::max(s, (layout_.main_h - block) / 2);
  g.caption_y = g.y + g.size + 2 * s;
  g.buttons_y = g.caption_y + ch + 2 * s;
  return g;
}

bool Ui::talk_caption_fits() const {
  UiModel m;
  m.screen = Screen::Responding;
  m.caption_lines = 2;
  return hero_geom(m).size > mascot::pick(0, mascot::Frame::Idle)->size;
}

void Ui::hero_anim_rows(const UiModel& m, int& y0, int& y1) const {
  const HeroGeom g = hero_geom(m);
  const int size = g.size, my = g.y;
  const mascot::Anchors& a = mascot::anchors(m.face);
  // A colour face's band, as the rows Canvas::picture draws it on at `size`.
  auto band_rows = [&](mascot::Frame frame, int& lo, int& hi) {
    if (!m.face || !m.face->rgb565 || !m.face->band(frame).rows) return;
    const mascot::Band& b = m.face->band(frame);
    const int px = m.face->px;
    lo = my + b.y * size / px;
    hi = my + ((b.y + b.rows) * size + px - 1) / px;
  };
  int top = my + size * a.eyes_y0 / 1000, bottom = my + size * a.eyes_y1 / 1000;
  band_rows(mascot::Frame::Blink, top, bottom);
  auto add = [&](int lo, int hi) {
    top = std::min(top, lo);
    bottom = std::max(bottom, hi);
  };
  const int reach = size * 280 / 1000;  // outermost wave radius + stroke
  switch (m.screen) {
    case Screen::Listening: {
      int ey = my + size * a.ear_cup.y / 1000;
      add(ey - reach, ey + reach);
      break;
    }
    case Screen::Responding: {
      int mouth = my + size * a.mouth.y / 1000;
      int lo = my + size * a.mouth_y0 / 1000, hi = my + size * a.mouth_y1 / 1000;
      band_rows(mascot::Frame::Talk, lo, hi);
      add(lo, hi);
      add(mouth - reach, mouth + reach);
      break;
    }
    case Screen::Thinking:
    case Screen::Connecting:
    case Screen::Updating: {
      int ty = my + size * a.head_top_right.y / 1000;
      add(ty - size / 6, ty + size / 12);
      break;
    }
    default: break;
  }
  y0 = std::max(top - 1, layout_.top_h);
  y1 = std::min(bottom + 2, static_cast<int>(info_.height) - layout_.bottom_h);
}

void Ui::draw_hero(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int w = info_.width;
  const int y0 = layout_.top_h;
  const int y1 = info_.height - layout_.bottom_h;
  const int lh = Canvas::line_height(s);
  c.fill_rect(0, y0, w, y1 - y0, bg_);

  const HeroGeom g = hero_geom(m);
  const int size = g.size, mx = g.x, my = g.y;
  const mascot::Anchors& a = mascot::anchors(m.face);
  const mascot::Bitmap b = mascot::pick(size, mascot_frame(m), m.face);
  draw_face(c, mx, my, b, m.face, kText);
  const int stroke = std::max(2, size / 48);
  auto waves = [&](int cx, int cy, int dir, int lit, int phase, uint16_t on, uint16_t dim, int r0) {
    for (int i = 0; i < 3; ++i) {
      int r = size * (r0 + 60 * i) / 1000;
      uint16_t col = i >= lit ? kFaint : (i == phase ? on : dim);
      c.arc(cx, cy, r, stroke, dir, col);
    }
  };
  auto dots = [&](uint16_t on, uint16_t dim) {
    int r = std::max(2, size / 30);
    int gap = 3 * r;
    int tx = mx + size * a.head_top_right.x / 1000;
    int ty = my + size * a.head_top_right.y / 1000;
    int active = static_cast<int>((m.frame / 2) % 4);
    for (int i = 0; i < 3; ++i) {
      c.fill_circle(tx + i * gap, ty - i * gap / 2, r, i == active ? on : dim);
    }
  };
  switch (m.screen) {
    case Screen::Listening: {
      int lit = 1 + std::min(2, m.level / 34);
      waves(mx + size * a.ear_cup.x / 1000, my + size * a.ear_cup.y / 1000, +1, lit,
            static_cast<int>(m.frame % 3), kGreen, kGreenDim, 90);
      break;
    }
    case Screen::Responding:
      if (m.speaking) {
        // Start outside the face outline so the waves read as sound, not as lines on her cheek.
        waves(mx + size * a.mouth.x / 1000, my + size * a.mouth.y / 1000, -1, 3,
              static_cast<int>((m.frame / 2) % 3), kAccent, kAccentDim, 130);
      }
      break;
    case Screen::Thinking: dots(kBlue, kBlueDim); break;
    case Screen::Connecting:
    case Screen::Updating: dots(kAccent, kAccentDim); break;
    default: break;
  }

  if (const Rect r = name_rect(m); r.w) {
    c.fill_round_rect(r.x, r.y, r.w, r.h, 3 * s, kBar);
    c.text(name_marker_x(), r.y + s, "v", s, kAccent);
  }
  // Caption: headline, then the detail lines, centred.
  const int cols = layout_.hero_cols;
  std::string head = fit(m.headline, m.name_target ? layout_.name_cols : cols);
  int cy = g.caption_y + std::max(0, g.name_h - (2 * lh + s)) / 2;
  c.text((w - Canvas::text_width(head, s)) / 2, cy, head, s, caption_color(m.screen));
  for (const auto& line : g.detail) {
    cy += lh;
    c.text((w - Canvas::text_width(line, s)) / 2, cy, line, s, kDim);
  }

  // Answer buttons (Prompt screen): TALK on the left, CANCEL on the right.
  if (!m.yes.empty() || !m.no.empty()) {
    const int bh = lh + 4 * s, gap = 4 * s, margin = 4 * s;
    const int bw = (w - 2 * margin - gap) / 2;
    auto button = [&](int x, const std::string& label, uint16_t fill) {
      c.fill_round_rect(x, g.buttons_y, bw, bh, 3 * s, fill);
      std::string t = fit(label, cols_for(bw - 2 * s, s));
      c.text(x + (bw - Canvas::text_width(t, s)) / 2, g.buttons_y + 2 * s + s / 2, t, s, kText);
    };
    if (!m.yes.empty()) button(margin, m.yes, kGreenDim);
    if (!m.no.empty()) button(margin + bw + gap, m.no, kRedDim);
  }
}

void Ui::draw_bottom(Canvas& c, const UiModel& m) {
  const int s = layout_.scale;
  const int y0 = info_.height - layout_.bottom_h;
  c.fill_rect(0, y0, info_.width, layout_.bottom_h, panel_.round ? bg_ : kBar);
  std::string hint = fit(m.hint, cols_for(info_.width - 4 * s, s));
  c.text((info_.width - Canvas::text_width(hint, s)) / 2, y0 + s, hint, s, kDim);
}

// What this frame's ring needs per pixel, worked out once: the level's sweep, the comet's
// head and colours, the seconds dot's centre on the band's middle radius and its radius,
// 4s px: about twice the band's width, so it stands out from the ticks. The dot turns with
// the ticks, so it stays on the tick of its second.
void Ui::set_ring(const EdgeRing& ring) {
  ring_ = ring;
  switch (ring.kind) {
    case EdgeRing::Kind::Level:
      ring_sweep_ = arc_sweep(ring.permille);
      break;
    case EdgeRing::Kind::Comet:
      ring_head64_ = static_cast<int32_t>(ring.head % 360) * 64;
      channels(ring.colour, ring_fg_);
      channels(bg_, ring_bg_);
      break;
    case EdgeRing::Kind::Ticks: {
      if (ring.turn64 != tick_turn64_) turn_ticks(ring.turn64);  // a new minute's shift
      const double mid = (band_in_ + band_out_ + 1) / 2.0;
      const double theta = kTau * (ring.second * 6 * 64 + ring.turn64) / kTurn64;
      ring_dot_x16_ = static_cast<int32_t>(std::lround(mid * std::sin(theta) * 16));
      ring_dot_y16_ = static_cast<int32_t>(std::lround(-mid * std::cos(theta) * 16));
      const int32_t r16 = 64 * layout_.scale;
      ring_dot_r2_ = r16 * r16;
      break;
    }
    case EdgeRing::Kind::None:
      break;
  }
}

// The 60 ticks' directions: every 6 degrees clockwise from 12 o'clock, turned by turn64.
void Ui::turn_ticks(int32_t turn64) {
  tick_turn64_ = turn64;
  for (int k = 0; k < 60; ++k) {
    const double theta = kTau * (k * 6 * 64 + turn64) / kTurn64;
    tick_cos_[k] = static_cast<int16_t>(std::lround(std::cos(theta) * kArcScale));
    tick_sin_[k] = static_cast<int16_t>(std::lround(std::sin(theta) * kArcScale));
  }
}

// The colour of the ring's pixel at (dx, dy) from the centre, for this frame's ring.
uint16_t Ui::ring_pixel(int dx, int dy) const {
  switch (ring_.kind) {
    case EdgeRing::Kind::Level:
      return in_arc(dx, dy, ring_sweep_) ? ring_.colour : ring_.dim;
    case EdgeRing::Kind::Comet: {
      // Whole at the head, fading as (1 - t / 70)^2 over the t degrees behind it, with a
      // one-degree ramp ahead of it; blended over the background.
      const int32_t behind = (ring_head64_ - ring_angle64(dx, dy) + kTurn64) % kTurn64;
      int32_t a = 0;  // of 256
      if (behind <= kTail64) {
        const int32_t u = kTail64 - behind;
        a = u * u / (kTail64 * kTail64 / 256);  // kTail64 squared is 256 * 78400
      } else if (kTurn64 - behind < 64) {
        a = (64 - (kTurn64 - behind)) * 4;
      }
      if (a <= 0) return bg_;
      if (a >= 256) return ring_.colour;
      const auto mix = [a](uint8_t f, uint8_t b) { return static_cast<uint8_t>((f * a + b * (256 - a) + 128) >> 8); };
      return rgb565(mix(ring_fg_[0], ring_bg_[0]), mix(ring_fg_[1], ring_bg_[1]), mix(ring_fg_[2], ring_bg_[2]));
    }
    case EdgeRing::Kind::Ticks: {
      const int32_t ex = dx * 16 - ring_dot_x16_, ey = dy * 16 - ring_dot_y16_;
      if (ex * ex + ey * ey <= ring_dot_r2_) return ring_.colour;  // the seconds dot, over the ticks
      const int32_t d2 = dx * dx + dy * dy;
      if (d2 <= band_in_ * band_in_ || d2 > band_out_ * band_out_) return bg_;  // the dot's reach: only the dot
      // The nearest tick, and the distance from its centre line (times kArcScale). The ticks
      // are the date's grey, so they show at the clock's dimmed brightness; the dot alone is amber.
      const int k = static_cast<int>((ring_angle64(dx, dy) - ring_.turn64 + kTurn64 + 192) / 384 % 60);
      const int32_t d = std::abs(dx * tick_cos_[k] + dy * tick_sin_[k]);
      const int s = layout_.scale;
      if (k % 5 == 0) return d < (s + 1) * kArcScale ? kDim : bg_;  // the hours: 2s + 1 px, across the band
      const int32_t mid2 = band_in_ + band_out_ + 1;  // twice the band's middle radius
      return 4 * d2 > mid2 * mid2 && 2 * d <= s * kArcScale ? kDim : bg_;  // the minutes: thinner, its outer half
    }
    case EdgeRing::Kind::None:
      break;
  }
  return bg_;
}

// The rows the ring owns: the band, or on the resting clock the band and the dot's reach.
const std::vector<Ui::BandRow>& Ui::ring_rows() const {
  return ring_.kind == EdgeRing::Kind::Ticks ? clock_rows_ : band_rows_;
}

// Paints the ring over panel rows [y0, y1) (push(), inside the window that holds them).
void Ui::paint_ring(int y0, int y1) {
  if (ring_.kind == EdgeRing::Kind::None || band_rows_.empty()) return;
  Canvas panel = panel_canvas();
  const int cx = panel_.width / 2, cy = panel_.height / 2;
  const std::vector<BandRow>& rows = ring_rows();
  for (int y = std::max(0, y0); y < std::min<int>(panel_.height, y1); ++y) {
    const BandRow& row = rows[static_cast<size_t>(y)];
    for (int run = 0; run < 2; ++run)
      for (int x = row.x0[run]; x < row.x1[run]; ++x) panel.pixel(x, y, ring_pixel(x - cx, y - cy));
  }
}

// The rectangle that holds every band pixel a comet lights with its head at `was` or at
// `now`: ring_pixel lights the 70 degrees behind the head and the one ahead of it, so the
// whole degrees from 70 behind each head to the head (ring_degrees_).
Ui::RingBox Ui::comet_box(uint16_t was, uint16_t now) const {
  RingBox box{static_cast<int16_t>(panel_.width), static_cast<int16_t>(panel_.height), 0, 0};
  for (const uint16_t head : {was, now})
    for (int back = 0; back <= kTail64 / 64; ++back) {
      const RingBox& d = ring_degrees_[static_cast<size_t>((head % 360 + 360 - back) % 360)];
      box.x0 = std::min(box.x0, d.x0);
      box.y0 = std::min(box.y0, d.y0);
      box.x1 = std::max(box.x1, d.x1);
      box.y1 = std::max(box.y1, d.y1);
    }
  return box;
}

// Only the ring changed on a panel that stays: writes the ring's pixels inside `box` that
// differ and sends their rows, changed rows at most 8 apart as one run. A framebuffer that
// holds the whole panel only (draw_frame repaints a banded one).
void Ui::update_ring(const RingBox& box) {
  if (ring_.kind == EdgeRing::Kind::None || band_rows_.empty() || display_.framebuffer_rows() > 0) return;
  uint16_t* fb = display_.framebuffer();
  const int w = panel_.width, cx = w / 2, cy = panel_.height / 2;
  const std::vector<BandRow>& rows = ring_rows();
  int first = -1, last = -1;  // the run of changed rows not sent yet
  for (int y = std::max<int>(0, box.y0); y < std::min<int>(panel_.height, box.y1); ++y) {
    const BandRow& row = rows[static_cast<size_t>(y)];
    uint16_t* line = fb + static_cast<size_t>(y) * static_cast<size_t>(w);
    bool changed = false;
    for (int run = 0; run < 2; ++run) {
      const int x1 = std::min<int>(row.x1[run], box.x1);
      for (int x = std::max<int>(row.x0[run], box.x0); x < x1; ++x) {
        uint16_t c = ring_pixel(x - cx, y - cy);
        if (panel_.swap_bytes) c = static_cast<uint16_t>((c >> 8) | (c << 8));
        if (line[x] == c) continue;
        line[x] = c;
        changed = true;
      }
    }
    if (!changed) continue;
    if (first >= 0 && y - last > 8) {
      send(first, last + 1);
      first = -1;
    }
    if (first < 0) first = y;
    last = y;
  }
  if (first >= 0) send(first, last + 1);
}

// TIMER DONE. The whole panel: green while `flash`, else the background. The hint sits in the
// bottom band, where the bar would be. Above it, the headline is as large as fits (at most 2/5
// of the panel), the mascot sits over it when there is room, and the timer's label under it:
// the group is centred on the glass and kept clear of the hint. Round panels fit every line to
// the chord at its own rows.
void Ui::draw_alarm(const UiModel& m) {
  // No ring over the alarm (edge_ring): it owns the whole panel.
  const uint32_t key = Hash().val(m.flash).add(m.headline).add(m.detail).add(m.hint).val(m.lang)
                           .add(m.face ? m.face->hash : std::string()).get();
  if (valid_ && full_face_ == FullFace::Alarm && key == face_hash_) return;  // invalidate() repaints it too
  full_face_ = FullFace::Alarm;
  face_hash_ = key;
  valid_ = true;  // leaving the alarm repaints everything anyway (draw_frame)

  const int s = layout_.scale;
  const int W = panel_.width, H = panel_.height;
  const int cx = W / 2, cy = H / 2, R = std::min(W, H) / 2;
  const uint16_t ink = m.flash ? rgb565(4, 40, 24) : kGreen;
  // The whole panel, once per framebuffer window (just once when it holds the panel).
  each_window(0, H, [&](int, int) {
    Canvas panel = panel_canvas();
    panel.fill_rect(0, 0, W, H, m.flash ? kGreen : bg_);

    // The width a line may use at rows [y0, y1): the glass's chord there (round panels), less a margin.
    auto avail = [&](int y0, int y1) {
      int w = W;
      if (panel_.round) {
        for (int y : {y0, y1 - 1}) {
          const int dy = y - cy;
          w = std::min(w, 2 * isqrt(static_cast<int64_t>(R) * R - static_cast<int64_t>(dy) * dy));
        }
      }
      return std::max(0, w - 8 * s);
    };

    // The hint, in the bottom band where the bar would be.
    const int hy = oy_ + info_.height - layout_.bottom_h;
    const int hcols = (avail(hy, hy + Canvas::line_height(s)) + s) / (font::kCellWidth * s);
    const std::string hint = fit(m.hint, hcols);
    panel.text(cx - Canvas::text_width(hint, s) / 2, hy + s, hint, s, ink);

    const std::vector<std::string> words = headline_lines(m.headline);
    const int n = static_cast<int>(words.size());
    const int gap = 3 * s;
    const int ls = s + 1;  // the label's scale
    const std::string label = m.detail.substr(0, m.detail.find('\n'));
    const int label_h = label.empty() ? 0 : Canvas::line_height(ls);
    const mascot::Bitmap face = mascot::pick(H / 3, mascot::Frame::Idle, m.face);
    const int face_size = face.size <= H / 3 ? face.size : 0;
    const int lo = 4 * s, hi = hy - gap;  // the group's rows: clear of the top edge and of the hint

    // The group's first row at headline scale k, with or without the mascot, or -1 when it does not
    // fit: taller than [lo, hi), or a line (or the mascot) wider than the glass at its own rows.
    auto place = [&](int k, bool with_face) {
      const int lh = Canvas::line_height(k);
      const int group = (with_face ? face_size + gap : 0) + n * lh + (label_h ? gap + label_h : 0);
      if (group > hi - lo) return -1;
      const int first = std::clamp(cy - group / 2, lo, hi - group);
      int y = first;
      if (with_face) {
        if (face_size > avail(y, y + face_size)) return -1;
        y += face_size + gap;
      }
      for (int i = 0; i < n; ++i, y += lh)
        if (Canvas::text_width(words[static_cast<size_t>(i)], k) > avail(y, y + lh)) return -1;
      return first;
    };
    int hs = 0, top = lo;
    for (int k = 8; k >= 1 && !hs; --k) {
      if (n * Canvas::line_height(k) > H * 2 / 5) continue;
      const int first = place(k, false);
      if (first >= 0) {
        hs = k;
        top = first;
      }
    }
    bool with_face = false;
    if (!hs) {
      hs = 1;  // nothing fits whole: the smallest scale, and the lines are cut below
    } else if (face_size) {
      const int first = place(hs, true);
      if (first >= 0) {
        with_face = true;
        top = first;
      }
    }

    const int lh = Canvas::line_height(hs);
    int y = top;
    if (with_face) {
      draw_face(panel, cx - face_size / 2, y, face, m.face, ink);
      y += face_size + gap;
    }
    for (int i = 0; i < n; ++i, y += lh) {
      const int cols = (avail(y, y + lh) + hs) / (font::kCellWidth * hs);
      const std::string line = fit(words[static_cast<size_t>(i)], cols);
      panel.text(cx - Canvas::text_width(line, hs) / 2, y, line, hs, ink);
    }
    if (label_h) {
      y += gap;
      const int cols = (avail(y, y + label_h) + ls) / (font::kCellWidth * ls);
      const std::string shown = fit(label, cols);
      panel.text(cx - Canvas::text_width(shown, ls) / 2, y, shown, ls, ink);
    }
  });
}

// The battery faces: the low battery alert (red, a "!"), plugging in (green, the level and
// a bolt) and a full battery (green, full). The whole panel: a battery outline with its
// terminal, the charge as a bar from the left, the mark in a gap cut out of the bar so the
// icon reads by shape as well as colour; the percentage (`headline`) large below it, then
// the one or two lines of `detail`. On a round panel the group keeps to the square inside
// the glass. `face_battery` below 0 draws no bar, and an empty headline no percentage.
void Ui::draw_battery_face(const UiModel& m) {
  // No ring over the battery faces (edge_ring): they own the whole panel.
  const uint32_t key = Hash().val(static_cast<int>(m.screen)).add(m.headline).add(m.detail).add(m.hint)
                           .val(m.face_battery).val(m.lang).get();
  if (valid_ && full_face_ == FullFace::Battery && key == face_hash_) return;
  full_face_ = FullFace::Battery;
  face_hash_ = key;
  valid_ = true;  // leaving it repaints everything anyway (draw_frame)

  enum class Mark { Alert, Bolt, None };
  const Mark mark = m.screen == Screen::LowBattery ? Mark::Alert : m.screen == Screen::Charging ? Mark::Bolt : Mark::None;
  const uint16_t ink = m.screen == Screen::LowBattery ? kRed : kGreen;

  const int s = layout_.scale;
  const int W = panel_.width, H = panel_.height;
  const int cx = W / 2, cy = H / 2;
  const int hy = oy_ + info_.height - layout_.bottom_h;  // the hint's band
  // The box the group keeps to: the inscribed square on a round panel.
  const int box_w = panel_.round ? W * 70 / 100 : W - 8 * s;
  const int lo = panel_.round ? cy - box_w / 2 : 4 * s;
  const int hi = panel_.round ? std::min(cy + box_w / 2, hy) : hy - 2 * s;

  std::string line1 = m.detail, line2;
  if (const size_t nl = m.detail.find('\n'); nl != std::string::npos) {
    line1 = m.detail.substr(0, nl);
    line2 = m.detail.substr(nl + 1);
  }
  const int ls1 = s + 1, ls2 = s;  // the first line a step above the base scale, the second at it
  const int gap = 5 * s;
  const int text_h = Canvas::line_height(ls1) + (line2.empty() ? 0 : Canvas::line_height(ls2));
  const bool number = !m.headline.empty();

  // The largest percentage scale, and the icon sized from the box, that fit the box's height.
  int ps = 1, iw = 0, ih = 0;
  auto group_h = [&](int k, int h) { return h + gap + (number ? Canvas::line_height(k) + gap : 0) + text_h; };
  for (int k = 12; k >= 1; --k) {
    if (number && Canvas::text_width(m.headline, k) > box_w * 3 / 4) continue;
    const int w = std::min(box_w * 3 / 4, (hi - lo) * 9 / 10);
    if (group_h(k, w * 9 / 20) <= hi - lo || k == 1 || !number) {
      ps = k;
      iw = w;
      ih = iw * 9 / 20;
      while (iw > 16 && group_h(k, ih) > hi - lo) {  // shrink the icon until the group fits
        iw = iw * 9 / 10;
        ih = iw * 9 / 20;
      }
      break;
    }
  }
  const int ph = number ? Canvas::line_height(ps) : 0;
  const int group = group_h(ps, ih);
  const int top = std::clamp(cy - group / 2, lo, std::max(lo, hi - group));

  each_window(0, H, [&](int, int) {
    Canvas panel = panel_canvas();
    panel.fill_rect(0, 0, W, H, bg_);

    // The icon: an outline (stroke t), the terminal on the right, the charge bar inside.
    const int t = std::max(2, iw / 22);
    const int nub_w = std::max(2, iw / 14), nub_h = ih * 2 / 5;
    const int body_w = iw - nub_w;
    const int x0 = cx - iw / 2, y0 = top;
    const int r = std::max(1, ih / 6);
    panel.fill_round_rect(x0, y0, body_w, ih, r, ink);
    panel.fill_round_rect(x0 + t, y0 + t, body_w - 2 * t, ih - 2 * t, std::max(1, r - t), bg_);
    panel.fill_rect(x0 + body_w, y0 + (ih - nub_h) / 2, nub_w, nub_h, ink);
    const int pad = std::max(1, t);
    const int in_x = x0 + t + pad, in_y = y0 + t + pad, in_w = body_w - 2 * (t + pad), in_h = ih - 2 * (t + pad);
    if (m.face_battery >= 0)
      panel.fill_rect(in_x, in_y, std::max(t, in_w * std::min<int>(m.face_battery, 100) / 100), in_h, ink);
    const int mid = x0 + body_w / 2;
    if (mark == Mark::Alert) {
      // A stem and a dot, in a gap cut out of the bar.
      const int ex_w = std::max(2, in_h / 5), stem_h = in_h * 11 / 20, dot_h = ex_w;
      const int ex_x = mid - ex_w / 2;
      const int ex_y = in_y + (in_h - (stem_h + ex_w / 2 + dot_h)) / 2;
      panel.fill_rect(ex_x - pad, in_y, ex_w + 2 * pad, in_h, bg_);
      panel.fill_rect(ex_x, ex_y, ex_w, stem_h, ink);
      panel.fill_rect(ex_x, ex_y + stem_h + ex_w / 2, ex_w, dot_h, ink);
    } else if (mark == Mark::Bolt) {
      // A lightning bolt, row by row: a band falling to the left, a crossbar, a band falling
      // to a point. Drawn grown by `pad` in the background first, then in the text colour.
      const int bh = in_h * 9 / 10, bw = bh * 4 / 5;
      const int by = in_y + (in_h - bh) / 2;
      auto bolt = [&](int grow, uint16_t col) {
        for (int row = -grow; row < bh + grow; ++row) {
          const int q = std::clamp(row, 0, bh - 1) * 1000 / std::max(1, bh - 1);  // 0..1000 down the bolt
          int xa, xb;
          if (q < 450) {  // the upper band, from the top right towards the middle
            const int c = bw * 20 / 100 - bw * 32 / 100 * q / 450;
            xa = c - bw * 19 / 100;
            xb = c + bw * 19 / 100;
          } else if (q < 560) {  // the crossbar
            xa = -bw * 30 / 100;
            xb = bw * 30 / 100;
          } else {  // the lower band, narrowing to a point at the bottom left
            const int f = (q - 560) * 1000 / 440;
            const int c = bw * 12 / 100 - bw * 32 / 100 * f / 1000;
            const int half = bw * 19 / 100 * (1000 - f) / 1000;
            xa = c - half;
            xb = c + half;
          }
          panel.fill_rect(mid + xa - grow, by + row, xb - xa + 1 + 2 * grow, 1, col);
        }
      };
      bolt(pad, bg_);
      bolt(0, kText);
    }

    // The percentage, then the detail's lines.
    int y = top + ih + gap;
    if (number) {
      panel.text(cx - Canvas::text_width(m.headline, ps) / 2, y, m.headline, ps, ink);
      y += ph + gap;
    }
    const std::string a = fit(line1, (box_w + ls1) / (font::kCellWidth * ls1));
    panel.text(cx - Canvas::text_width(a, ls1) / 2, y, a, ls1, kText);
    y += Canvas::line_height(ls1);
    if (!line2.empty()) {
      const std::string b = fit(line2, (box_w + ls2) / (font::kCellWidth * ls2));
      panel.text(cx - Canvas::text_width(b, ls2) / 2, y, b, ls2, kDim);
    }

    // The hint, if any, in the bottom band where the bar would be.
    if (m.hint.empty()) return;
    const int hw = panel_.round ? W * 60 / 100 : W - 8 * s;
    const std::string hint = fit(m.hint, (hw + s) / (font::kCellWidth * s));
    panel.text(cx - Canvas::text_width(hint, s) / 2, hy + s, hint, s, kDim);
  });
}

// The resting clock face (App::clock_tick). The whole panel: the time's middle row
// on the glass centre, the date below it, and the mascot above like a watch logo
// when the chord there holds it; a rectangular panel centres the group and drops
// the mascot first. Every line keeps clear of the glass at its own rows, also where
// the per-minute shift can move it. The face changes once a minute: the burn-in
// shift keys the hash on the minute too.
void Ui::draw_clock(const UiModel& m) {
  // The ring's ticks go on with the clock: each push paints them, turned with this minute's
  // shift (edge_ring), and a new second moves only the dot (draw_frame, update_ring).
  const uint32_t key = Hash().add(m.clock_time).add(m.clock_date).val(m.clock_minute).val(m.lang)
                           .add(m.face ? m.face->hash : std::string()).get();
  if (valid_ && full_face_ == FullFace::Clock && key == face_hash_) return;  // invalidate() repaints it too
  full_face_ = FullFace::Clock;
  face_hash_ = key;
  valid_ = true;  // leaving the face repaints everything anyway (draw_frame)

  const int s = layout_.scale;
  const int W = panel_.width, H = panel_.height;
  const int cx = W / 2, cy = H / 2, R = std::min(W, H) / 2;
  // The burn-in shift moves the whole face a few pixels each minute.
  const BurnInShift shift = burn_in_shift(m.clock_minute);
  const int dx = shift.dx, dy = shift.dy;
  const int gap = 3 * s;
  const int margin = 4;
  // The profile's face where it has one, like the Ready screen and the alarm:
  // the 96 px bitmap where it fits, else the 64 px one, else none.
  auto face_at = [&m](int size) {
    const mascot::Bitmap b = mascot::pick(size, mascot::Frame::Idle, m.face);
    return b.size == size ? b : mascot::Bitmap{};
  };

  // The whole panel, once per framebuffer window (just once when it holds the panel).
  each_window(0, H, [&](int, int) {
    Canvas panel = panel_canvas();
    panel.fill_rect(0, 0, W, H, bg_);

    // The width a line may use at rows [y0, y1): the glass's chord over those rows
    // and over the rows the shift can move it to (round panels), less a margin.
    auto chord = [&](int y) {
      if (!panel_.round) return W;
      const int64_t d = y - cy;
      return static_cast<int>(2 * isqrt(static_cast<int64_t>(R) * R - d * d));
    };
    auto avail = [&](int y0, int y1) {
      int w = W;
      if (panel_.round)
        for (int y = y0 - 4; y <= y1 - 1 + 4; ++y) w = std::min(w, chord(y));
      return std::max(0, w - 2 * margin);
    };
    // The date at the alarm label's scale, falling back to the layout's own.
    auto date_scale_at = [&](int y) {
      for (int ds : {s + 1, s})
        if (Canvas::text_width(m.clock_date, ds) <= avail(y, y + Canvas::line_height(ds))) return ds;
      return 0;
    };

    int k = 0, ds = 0, time_y = 0, date_y = 0, face_y = -1;
    mascot::Bitmap face{};
    if (panel_.round) {
      for (int kk = 10; kk >= 1 && !k; --kk) {
        const int lh = Canvas::line_height(kk);
        const int ty = cy - (7 * kk) / 2;  // the digits span 7k rows: centred on the glass centre
        if (Canvas::text_width(m.clock_time, kk) > avail(ty, ty + lh)) continue;
        const int dyy = ty + lh + gap;
        const int dsk = date_scale_at(dyy);
        if (!dsk) continue;
        k = kk;
        ds = dsk;
        time_y = ty;
        date_y = dyy;
      }
      if (!k) {  // nothing fits whole: the smallest scale, cut if it must be
        k = 1;
        ds = s;
        time_y = cy - 3;
        date_y = time_y + Canvas::line_height(1) + gap;
      } else {
        // Two thirds of the free rows above the time go above the icon and one third
        // below it, so the icon sits nearer the time than the top of the glass.
        for (int size : {96, 64}) {
          const mascot::Bitmap b = face_at(size);
          const int y = 2 * (time_y - size) / 3;
          if (!b.bits || y < margin || time_y - (y + size) < gap || avail(y, y + size) < size) continue;
          face = b;
          face_y = y;
          break;
        }
      }
    } else {
      for (int kk = 10; kk >= 1 && !k; --kk) {
        if (Canvas::text_width(m.clock_time, kk) > W - 2 * margin) continue;
        int dsk = s + 1;
        if (Canvas::text_width(m.clock_date, dsk) > W - 2 * margin) dsk = s;
        if (Canvas::text_width(m.clock_date, dsk) > W - 2 * margin) continue;
        if (Canvas::line_height(kk) + gap + Canvas::line_height(dsk) > H - 2 * margin) continue;
        k = kk;
        ds = dsk;
      }
      if (!k) {  // nothing fits whole: the smallest scale, cut if it must be
        k = 1;
        ds = s;
        time_y = (H - (Canvas::line_height(1) + gap + Canvas::line_height(s))) / 2;
        date_y = time_y + Canvas::line_height(1) + gap;
      } else {
        const int body_h = Canvas::line_height(k) + gap + Canvas::line_height(ds);
        time_y = (H - body_h) / 2;
        // The centred group (icon, gap, time, gap, date): the mascot is dropped
        // first when there is no room for it.
        for (int size : {96, 64}) {
          const mascot::Bitmap b = face_at(size);
          if (!b.bits || size + gap + body_h > H - 2 * margin) continue;
          face = b;
          face_y = (H - (size + gap + body_h)) / 2;
          time_y = face_y + size + gap;
          break;
        }
        date_y = time_y + Canvas::line_height(k) + gap;
      }
    }

    if (face_y >= 0) draw_face(panel, cx + dx - face.size / 2, face_y + dy, face, m.face, kText);
    panel.text(cx + dx - Canvas::text_width(m.clock_time, k) / 2, time_y + dy, m.clock_time, k, kText);
    if (Canvas::text_width(m.clock_date, ds) <= avail(date_y, date_y + Canvas::line_height(ds)))
      panel.text(cx + dx - Canvas::text_width(m.clock_date, ds) / 2, date_y + dy, m.clock_date, ds, kDim);
  });
}

}  // namespace hg
