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

// The edge arc's angles are integers: sine and cosine times kArcScale, so the per-pixel
// test is a sign check and one cross product. The sweep is computed once per frame.
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
  if (m.screen == Screen::Responding && m.speaking) {
    return (m.frame / 2) % 2 ? mascot::Frame::Talk : mascot::Frame::Idle;
  }
  return m.frame % 45 < 2 ? mascot::Frame::Blink : mascot::Frame::Idle;  // blink every ~4.5 s
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
    // The edge arc's band (draw_frame): a ring 2s + 2 px in from the glass, 3s + 2 px thick.
    const int R = std::min(panel_.width, panel_.height) / 2;
    band_out_ = R - (2 * s + 2);
    band_in_ = std::max(0, band_out_ - (3 * s + 2));
    const int cx = panel_.width / 2, cy = panel_.height / 2;
    band_rows_.assign(static_cast<size_t>(panel_.height), BandRow{});
    for (int y = 0; y < panel_.height; ++y) {
      BandRow& row = band_rows_[static_cast<size_t>(y)];
      const int dy = y - cy;
      int run = -1;  // the run being filled: a row holds at most two
      bool inside = false;
      for (int x = 0; x < panel_.width; ++x) {
        const int dx = x - cx, d2 = dx * dx + dy * dy;
        const bool in = d2 <= band_out_ * band_out_ && d2 > band_in_ * band_in_;
        if (in && !inside) {
          run = std::min(run + 1, 1);
          row.x0[run] = static_cast<int16_t>(x);
        }
        if (in) row.x1[run] = static_cast<int16_t>(x + 1);
        inside = in;
      }
    }
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
      // framebuffer keeps from the full repaint (push then paints the arc).
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
  if (band_.permille >= 0) paint_band(y0, y1);  // the arc goes over whatever these rows drew
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
  if (m.screen == Screen::Alarm) {
    draw_alarm(m);
    return;
  }
  if (m.screen == Screen::Clock) {
    draw_clock(m);
    return;
  }
  if (full_face_ != FullFace::None) {  // a face painted the whole panel: everything comes back
    full_face_ = FullFace::None;
    valid_ = false;
  }
  const int h = info_.height;
  // The edge arc (round panels) appears and goes with the timer; either repaints the panel once.
  const bool arc = panel_.round && m.timer_permille >= 0;
  if (arc != band_shown_) {
    band_shown_ = arc;
    valid_ = false;
  }
  const ArcSweep before = band_;
  band_ = arc ? arc_sweep(m.timer_permille) : ArcSweep{};
  const bool swept = valid_ && arc && before.permille >= 0 && before.permille != band_.permille;
  // A banded framebuffer no longer holds the rest of the rows the sweep crossed,
  // so it repaints the whole panel instead.
  if (swept && display_.framebuffer_rows() > 0) valid_ = false;
  if (!valid_ && (ox_ || oy_)) {
    // Round panel: everything outside the UI area stays the background colour.
    // Each push paints the arc too.
    each_window(0, panel_.height, [&](int, int) { panel_canvas().fill_rect(0, 0, panel_.width, panel_.height, bg_); });
  }
  if (panel_.round) draw_round_battery(m);
  // Only the sweep moved: repaint the rows it crossed (the rest of the panel is unchanged).
  if (swept && valid_) push_band_change(before, band_);
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
    return;
  }
  hero_valid_ = false;

  struct Band {
    int y0, y1;
    void (Ui::*draw)(Canvas&, const UiModel&);
  };
  const Band bands[4] = {
      {0, y_header, &Ui::draw_top},
      {y_header, y_content, &Ui::draw_header},
      {y_content, y_bottom, &Ui::draw_content},
      {y_bottom, h, &Ui::draw_bottom},
  };
  for (int i = 0; i < 4; ++i) {
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

// Round panels: the battery is centred at the very top of the panel, above the UI
// square, as high as the circle leaves it room plus a small margin.
void Ui::draw_round_battery(const UiModel& m) {
  const int s = layout_.scale;
  const int bh = 7 * s;  // the taller of the icon (5s + 1) and the text (7s)
  const int hw = (m.battery >= 0 ? battery_width(m) : 0) / 2 + 2 * s;
  // The glass centre's row, and the circle the strip's top corners must lie in: the glass
  // edge, or while the arc shows, the band's inner edge (the strip starts below the arc).
  const int half = panel_.width / 2;
  const int radius = band_.permille >= 0 ? band_in_ : half;
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
  if (m.screen == Screen::Profiles) {
    draw_profiles(c, m);
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
  const int s = layout_.scale, lh = Canvas::line_height(s);
  const HeroGeom g = hero_geom(m);
  // The headline and the cue under it, padded to a finger's height.
  return {2 * s, g.caption_y - s, info_.width - 4 * s, g.name_h};
}

int Ui::name_marker_x() const {
  const int s = layout_.scale;
  return info_.width - 2 * s - 3 * s - Canvas::text_width("v", s);  // name_rect's right edge, less 3s
}

bool Ui::name_hit(const UiModel& m, int x, int y) const { return name_rect(m).has(x - ox_, y - oy_); }

// Rows at least a finger tall. When they do not all fit, a page shows what does, starting at
// item m.scroll, and its last row is More.
Ui::ProfilePage Ui::profile_page(const UiModel& m) const {
  ProfilePage p;
  const int s = layout_.scale, y0 = layout_.top_h + layout_.header_h, y1 = info_.height - layout_.bottom_h;
  const int n = static_cast<int>(m.dash.size()), band = y1 - y0 - 2 * s;
  if (m.screen != Screen::Profiles || n == 0) return p;
  const int fit = std::max(2, band / (std::max(touch_px_, Canvas::line_height(s) + 2 * s) + s));  // + the gap
  p.more = n > fit;
  p.first = p.more ? std::max(0, std::min(m.scroll, n - 1)) : 0;
  p.count = p.more ? std::min(fit - 1, n - p.first) : n;
  p.row = band / (p.more ? fit : std::max(n, std::min(4, fit)));
  p.top = y0 + 2 * s;
  return p;
}

Ui::Rect Ui::ProfilePage::slot(const Ui& ui, int i) const {
  const int s = ui.layout_.scale;
  return {2 * s, top + i * row, ui.info_.width - 4 * s, row - s};
}

int Ui::profile_row_hit(const UiModel& m, int x, int y) const {
  const ProfilePage p = profile_page(m);
  for (int i = 0; i < p.count + p.more; ++i)
    if (p.slot(*this, i).has(x - ox_, y - oy_)) return i < p.count ? p.first + i : kMoreRow;
  return -1;
}

int Ui::profile_next_page(const UiModel& m) const {
  const ProfilePage p = profile_page(m);
  return p.first + p.count < static_cast<int>(m.dash.size()) ? p.first + p.count : 0;
}

void Ui::draw_profiles(Canvas& c, const UiModel& m) {
  const int s = layout_.scale, glyph_h = font::kGlyphHeight * s;
  const ProfilePage p = profile_page(m);
  for (int i = 0; i < p.count + p.more; ++i) {
    const Rect r = p.slot(*this, i);
    const bool more = i == p.count;
    const DashRow row = more ? DashRow{m.detail, ">"} : m.dash[static_cast<size_t>(p.first + i)];
    c.fill_round_rect(r.x, r.y, r.w, r.h, 3 * s, kBar);
    const int ty = r.y + (r.h - glyph_h) / 2, right = r.x + r.w - 3 * s;
    const int value_w = Canvas::text_width(row.value, s);
    c.text(right - value_w, ty, row.value, s, tone_color(row.tone));
    c.text(r.x + 3 * s, ty, fit(row.label, cols_for(right - value_w - r.x - 5 * s, s)), s, more ? kDim : kText);
  }
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
  // Reserve one detail line even when there is none, so the mascot keeps its size. A name
  // panel (headline and first detail line) grows to a finger's height.
  g.name_h = m.name_target ? std::max(2 * lh + s, touch_px_) : 0;
  const int ch = caption_h(std::max<int>(1, static_cast<int>(g.detail.size()))) + std::max(0, g.name_h - (2 * lh + s));
  const int avail = std::min(w - 8 * s, layout_.main_h - ch - buttons_h - 4 * s);
  g.size = mascot::pick(avail, mascot::Frame::Idle)->size;
  const int block = g.size + 2 * s + ch + buttons_h;
  g.x = (w - g.size) / 2;
  g.y = layout_.main_y + std::max(s, (layout_.main_h - block) / 2);
  g.caption_y = g.y + g.size + 2 * s;
  g.buttons_y = g.caption_y + ch + 2 * s;
  return g;
}

void Ui::hero_anim_rows(const UiModel& m, int& y0, int& y1) const {
  const HeroGeom g = hero_geom(m);
  const int size = g.size, my = g.y;
  const mascot::Anchors& a = mascot::anchors(m.face);
  int top = my + size * a.eyes_y0 / 1000, bottom = my + size * a.eyes_y1 / 1000;
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
      add(my + size * a.mouth_y0 / 1000, my + size * a.mouth_y1 / 1000);
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
  c.bitmap1(mx, my, b.size, b.size, b.bits, kText);
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

void Ui::paint_band(int y0, int y1) {
  if (band_rows_.empty()) return;
  Canvas panel = panel_canvas();  // push() runs inside the window that holds these rows
  const int cx = panel_.width / 2, cy = panel_.height / 2;
  for (int y = std::max(0, y0); y < std::min<int>(panel_.height, y1); ++y) {
    const BandRow& row = band_rows_[static_cast<size_t>(y)];
    for (int run = 0; run < 2; ++run) {
      for (int x = row.x0[run]; x < row.x1[run]; ++x) {
        panel.pixel(x, y, in_arc(x - cx, y - cy, band_) ? kAccent : kAccentDim);
      }
    }
  }
}

// Sends the panel rows the arc crossed when its end moved from `from` to `to`. The rows lie
// between the two ends at both radii, widened to the top or bottom of the band when the
// sweep passed those points.
void Ui::push_band_change(const ArcSweep& from, const ArcSweep& to) {
  const int cy = panel_.height / 2;
  int lo = panel_.height, hi = 0;
  for (const ArcSweep* s : {&from, &to}) {
    for (int r : {band_in_, band_out_}) {
      const int y = cy + static_cast<int>(static_cast<int64_t>(r) * s->ey / kArcScale);
      lo = std::min(lo, y);
      hi = std::max(hi, y + 1);
    }
  }
  if (in_arc(0, -band_out_, from) != in_arc(0, -band_out_, to)) lo = std::min(lo, cy - band_out_);
  if (in_arc(0, band_out_, from) != in_arc(0, band_out_, to)) hi = std::max(hi, cy + band_out_ + 1);
  lo = std::max(0, lo - 1);
  hi = std::min<int>(panel_.height, hi + 1);
  if (lo < hi) push(lo, hi);
}

// TIMER DONE. The whole panel: green while `flash`, else the background. The hint sits in the
// bottom band, where the bar would be. Above it, the headline is as large as fits (at most 2/5
// of the panel), the mascot sits over it when there is room, and the timer's label under it:
// the group is centred on the glass and kept clear of the hint. Round panels fit every line to
// the chord at its own rows.
void Ui::draw_alarm(const UiModel& m) {
  band_ = ArcSweep{};  // no arc over the alarm
  band_shown_ = false;
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
      panel.bitmap1(cx - face_size / 2, y, face.size, face.size, face.bits, ink);
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

// The resting clock face (App::clock_tick). The whole panel: the time's middle row
// on the glass centre, the date below it, and the mascot above like a watch logo
// when the chord there holds it; a rectangular panel centres the group and drops
// the mascot first. Every line keeps clear of the glass at its own rows, also where
// the per-minute shift can move it. The face changes once a minute: the burn-in
// shift keys the hash on the minute too.
void Ui::draw_clock(const UiModel& m) {
  band_ = ArcSweep{};  // no arc over the clock
  band_shown_ = false;
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
  const int dx = (m.clock_minute * 7) % 9 - 4, dy = (m.clock_minute * 5) % 9 - 4;
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

    if (face_y >= 0) panel.bitmap1(cx + dx - face.size / 2, face_y + dy, face.size, face.size, face.bits, kText);
    panel.text(cx + dx - Canvas::text_width(m.clock_time, k) / 2, time_y + dy, m.clock_time, k, kText);
    if (Canvas::text_width(m.clock_date, ds) <= avail(date_y, date_y + Canvas::line_height(ds)))
      panel.text(cx + dx - Canvas::text_width(m.clock_date, ds) / 2, date_y + dy, m.clock_date, ds, kDim);
  });
}

}  // namespace hg
