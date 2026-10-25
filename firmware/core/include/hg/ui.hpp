// Screen model and renderer.
//
// The app fills a UiModel; Ui renders it into the framebuffer in horizontal
// bands and flushes only the bands whose inputs changed. The renderer is
// deterministic (no clocks, no randomness): the same model always produces the
// same pixels, which is what the simulator's screenshot tests rely on. Frame
// timing (measure) reads a clock around the drawing but never changes it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/frame_stats.hpp"
#include "hg/hal.hpp"
#include "hg/i18n.hpp"
#include "hg/mascot.hpp"
#include "hg/qr.hpp"

namespace hg {

enum class Screen : uint8_t {
  Boot,
  Offline,
  Connecting,
  Pairing,
  Ready,
  Listening,
  Thinking,
  Responding,
  Card,
  Image,
  Error,
  Prompt,  // a yes/no question from Hermes, answered with the buttons
  Updating,  // installing a firmware update
  Settings,
  Setup,
  Dashboard,  // Hermes's sessions and the user's status tiles (docs/protocol.md#dashboard)
  Profiles,   // the Hermes profiles to choose from: rows in `dash` from `scroll`; `detail` labels More
  Alarm,      // TIMER DONE: the whole panel until dismissed (App::start_alarm)
  Clock,      // the resting clock face: the time, the date and the mascot (App::clock_tick)
};

enum class Link : uint8_t { Offline, Network, Connecting, Online };

const char* screen_name(Screen s);

// One line of the dashboard: a coloured dot, a label and a right-aligned value.
// A heading has only a label and starts a section.
struct DashRow {
  enum class Tone : uint8_t { Dim, Busy, Waiting, Good, Warn, Bad };
  std::string label, value;
  Tone tone = Tone::Dim;
  bool heading = false;
};

struct UiModel {
  Screen screen = Screen::Boot;
  Link link = Link::Offline;
  std::string title;     // top bar (device name)
  i18n::Lang lang = i18n::Lang::En;  // for the words the UI draws itself (top bar)
  std::string headline;  // header band, next to the indicator
  std::string detail;    // secondary line (status phrase, URL, error)
  std::string body;      // main text (reply, card body)
  std::string code;      // pairing code
  int scroll = -1;       // first visible body line; -1 pins to the end
  uint8_t level = 0;     // microphone level 0..100
  bool speaking = false;
  bool color_test = false;
  bool settings_hold = true;  // round panels draw the settings target only while holding it works
  // Battery level, in the top bar (round panels: centred at the top edge, above
  // the UI square): percent, or -1 to show none. Charging fills
  // it green; low (and not on USB) fills it red.
  int8_t battery = -1;
  bool battery_icon = true, battery_text = true;  // which parts to draw
  bool battery_charging = false;
  bool battery_low = false;
  uint32_t frame = 0;    // animation frame, advanced by the app
  std::string hint;      // bottom bar
  std::string yes, no;   // answer buttons under the hero caption (Prompt screen)
  // Show the mascot as large as fits, with headline/detail as a caption,
  // instead of the header + text layout.
  bool hero = false;
  uint8_t caption_lines = 1;  // hero: lines the detail may wrap to (then "..")
  bool name_target = false;   // hero: the caption sits in a panel that a tap opens the profile list from
  const mascot::Face* face = nullptr;  // drawn instead of the mascot; borrowed like `qr`
  // Wi-Fi setup: a scannable code for the temporary network, drawn centred in
  // the content band. Null on every other screen. Borrowed, not owned: the app
  // keeps the matrix so it is encoded once, not on every model update.
  const qr::Code* qr = nullptr;
  // Dashboard screen: its rows, drawn from `scroll` (0 = the top).
  std::vector<DashRow> dash;
  // A running timer: thousandths of its length still to go, or -1 for none. The round
  // panel's edge arc and the rectangular top bar's line show it.
  int16_t timer_permille = -1;
  bool flash = false;  // Screen::Alarm: the green half of a beep cycle
  // Screen::Clock: the time and the date as the app formats them, and the minute
  // that picks the burn-in shift. No seconds: the face changes once a minute.
  std::string clock_time, clock_date;
  uint8_t clock_minute = 0;
};

struct UiLayout {
  int scale = 1;  // base text scale
  int top_h = 0, header_h = 0, bottom_h = 0;
  int title_w = 0;  // width of the settings hold target in the top band
  int body_cols = 0, body_rows = 0;
  int main_y = 0, main_h = 0;  // area between the bars (used for images)
  int hero_cols = 0;  // characters per hero caption line
  int name_cols = 0;  // hero headline characters in a name panel, clear of its marker
};

// The edge arc's sweep: the end direction, clockwise from 12 o'clock, as (sin, -cos) times
// a fixed scale, plus the fraction it covers. permille -1 means no arc.
struct ArcSweep {
  int permille = -1;
  int32_t ex = 0, ey = 0;
};

class Ui {
 public:
  explicit Ui(Display& display);

  // Renders bands whose inputs changed since the last call and flushes them.
  void render(const UiModel& model);
  // Forces a full redraw on the next render (after something else drew).
  void invalidate();
  // Frame timing: while `stats` is set, every render is timed with `clock` and
  // recorded there. Null (the default) turns it off and leaves the clock unread.
  void measure(FrameStats* stats, System* clock);
  // An input arrived: the next render that sends anything records its latency.
  void mark_input();
  const UiLayout& layout() const { return layout_; }
  // The area the UI draws in: the whole panel, or on a round panel the square
  // inscribed in it. Layout coordinates are relative to this area.
  const DisplayInfo& area() const { return info_; }
  // A canvas over that area, and a flush of its rows [y0, y1).
  Canvas canvas();
  void flush(int y0, int y1);
  // Body text rows visible on a text screen for this model (after detail lines).
  int body_rows(const UiModel& m) const;
  // Dashboard rows that fit the content band.
  int dash_rows() const;
  // Touch targets (the name panel, list rows) are at least this many pixels tall.
  void set_touch_px(int px) { touch_px_ = px; }
  // Panel coordinates: the hero caption's panel (name_target), and the item of the Profiles row
  // there (-1: none, kMoreRow: the next page). profile_next_page gives that page's first item.
  static constexpr int kMoreRow = -2;
  bool name_hit(const UiModel& m, int x, int y) const;
  int profile_row_hit(const UiModel& m, int x, int y) const;
  int profile_next_page(const UiModel& m) const;
  bool title_hit(int x, int y) const {
    const int left = ox_ + (info_.width - layout_.title_w) / 2;
    return x >= left && x < left + layout_.title_w && y >= oy_ && y < oy_ + layout_.top_h;
  }

 private:
  void draw_frame(const UiModel& m);
  // Draws band rows [y0, y1) and flushes them. On a display that cannot hold the
  // whole panel (Display::framebuffer_rows) the band is drawn once per window it
  // spans, so the caller needs no idea of the windowing.
  void draw_bands(int y0, int y1, void (Ui::*draw)(Canvas&, const UiModel&), const UiModel& m);
  // Calls draw(a, b) for each framebuffer window panel rows [y0, y1) cross, with
  // the window mapped and [a, b) the panel rows of the range inside it, then
  // pushes those rows. One call for the whole range when the framebuffer holds
  // the panel.
  template <typename Draw>
  void each_window(int y0, int y1, Draw&& draw);
  // A canvas over the whole panel (not just the UI area) for the current window.
  Canvas panel_canvas();
  // Sends panel rows [y0, y1) to the display, timed while measuring.
  void push(int y0, int y1);
  void draw_top(Canvas& c, const UiModel& m);
  void draw_header(Canvas& c, const UiModel& m);
  void draw_content(Canvas& c, const UiModel& m);
  bool draw_qr(Canvas& c, const UiModel& m, int y0, int y1);
  void draw_dashboard(Canvas& c, const UiModel& m, int y0, int y1);
  void draw_profiles(Canvas& c, const UiModel& m);
  struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool has(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  // Area coordinates. The name panel is empty unless the model has one.
  Rect name_rect(const UiModel& m) const;
  // The left edge of the name panel's "v" marker, at the panel's right end.
  int name_marker_x() const;
  struct ProfilePage {
    int first = 0, count = 0, row = 0, top = 0;
    bool more = false;
    Rect slot(const Ui& ui, int i) const;
  };
  ProfilePage profile_page(const UiModel& m) const;
  int touch_px_ = 0;
  void draw_bottom(Canvas& c, const UiModel& m);
  void draw_indicator(Canvas& c, const UiModel& m, int cx, int cy, int r);
  // Draws the battery icon and percent from x, vertically centred on cy.
  void draw_battery(Canvas& c, const UiModel& m, int x, int cy);
  int battery_width(const UiModel& m) const;
  void draw_round_battery(const UiModel& m);
  struct HeroGeom {
    int size = 0, x = 0, y = 0;  // mascot
    int caption_y = 0, buttons_y = 0;
    int name_h = 0;  // the name panel around the headline and first detail line (0: none)
    std::vector<std::string> detail;  // wrapped, already truncated
  };
  HeroGeom hero_geom(const UiModel& m) const;
  void draw_hero(Canvas& c, const UiModel& m);
  // Rows [y0, y1) that hero animations may touch for this screen.
  void hero_anim_rows(const UiModel& m, int& y0, int& y1) const;
  // Round panels: the edge arc, and the whole-panel faces (draw_alarm, draw_clock).
  void paint_band(int y0, int y1);
  void push_band_change(const ArcSweep& from, const ArcSweep& to);
  void draw_alarm(const UiModel& m);
  void draw_clock(const UiModel& m);
  Display& display_;
  DisplayInfo panel_;
  DisplayInfo info_;
  uint16_t bg_;  // background colour (see DisplayInfo::emissive)
  int ox_ = 0, oy_ = 0;  // area origin on the panel
  // First panel row the framebuffer covers right now (see draw_bands); 0 unless
  // the display holds only a band of rows.
  int frame_origin_ = 0;
  UiLayout layout_;
  uint32_t hash_[4] = {0, 0, 0, 0};
  uint32_t hero_static_ = 0, hero_anim_ = 0;
  uint32_t battery_hash_ = 0;
  bool hero_valid_ = false;
  bool valid_ = false;
  // frame timing (measure)
  FrameStats* stats_ = nullptr;
  System* clock_ = nullptr;
  uint64_t flush_us_ = 0;
  uint32_t bands_ = 0;
  uint64_t input_at_ = 0;
  bool input_pending_ = false;
  // Round panels: the edge arc's band per panel row (up to two runs of pixels), the radii it
  // spans, and the sweep push() paints it with. band_ is -1 while no arc shows.
  struct BandRow {
    int16_t x0[2] = {0, 0}, x1[2] = {0, 0};
  };
  std::vector<BandRow> band_rows_;
  int band_out_ = 0, band_in_ = 0;
  ArcSweep band_;
  bool band_shown_ = false;  // the arc was on the last frame
  // The screen that owns the whole panel right now, and the hash of what it drew.
  // Leaving it repaints everything (draw_frame); each face repaints when the flag
  // is not itself, so Clock -> Alarm -> Clock in one minute still redraws.
  enum class FullFace : uint8_t { None, Alarm, Clock };
  FullFace full_face_ = FullFace::None;
  uint32_t face_hash_ = 0;
};

}  // namespace hg
