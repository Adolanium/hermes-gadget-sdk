// The widget face (Ui::draw_widget): each kind draws whole inside the glass or the
// panel on round, small and large screens, also at every resting shift; a state
// colours it; long text is cut, never drawn outside; and it repaints only on a change.
#include <cstdint>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/ui.hpp"

using hg::Screen;
using hg::Widget;

namespace {

// The colours the face uses (ui.cpp).
constexpr uint16_t kText = hg::rgb565(232, 238, 242);
constexpr uint16_t kDim = hg::rgb565(132, 146, 160);
constexpr uint16_t kFaint = hg::rgb565(52, 62, 76);
constexpr uint16_t kAccent = hg::rgb565(242, 179, 61);
constexpr uint16_t kGreen = hg::rgb565(61, 214, 140);
constexpr uint16_t kRed = hg::rgb565(242, 95, 92);

// A panel the test renders on. Emissive, so the background is 0 and any ink shows.
struct WidgetDisplay : hg::Display {
  hg::DisplayInfo d;
  std::vector<uint16_t> fb;
  int flushed = 0;  // panel rows sent to the panel
  WidgetDisplay(int w, int h, bool round) : fb(static_cast<size_t>(w * h)) {
    d.width = static_cast<uint16_t>(w);
    d.height = static_cast<uint16_t>(h);
    d.round = round;
    d.emissive = true;
  }
  hg::DisplayInfo info() const override { return d; }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t y0, uint16_t y1) override { flushed += y1 - y0; }
};

struct Panel {
  WidgetDisplay display;
  hg::Ui ui;
  Panel(int w, int h, bool round) : display(w, h, round), ui(display) {}
  uint16_t at(int x, int y) const { return display.fb[static_cast<size_t>(y * display.d.width + x)]; }
  int width() const { return display.d.width; }
  int height() const { return display.d.height; }
  size_t count(uint16_t color) const {
    size_t n = 0;
    for (uint16_t px : display.fb) n += px == color;
    return n;
  }
  // The columns one colour spans, or -1 when it draws nothing.
  int span(uint16_t color) const {
    int x0 = width(), x1 = -1;
    for (int y = 0; y < height(); ++y)
      for (int x = 0; x < width(); ++x)
        if (at(x, y) == color) {
          x0 = std::min(x0, x);
          x1 = std::max(x1, x);
        }
    return x1 < x0 ? -1 : x1 - x0 + 1;
  }
};

Widget value_widget() {
  Widget w;
  w.kind = Widget::Kind::Value;
  w.label = "Office";
  w.value = "21.5";
  w.unit = "C";
  return w;
}

Widget progress_widget(int percent) {
  Widget w;
  w.kind = Widget::Kind::Progress;
  w.label = "Backup";
  w.percent = static_cast<uint8_t>(percent);
  return w;
}

Widget list_widget() {
  Widget w;
  w.kind = Widget::Kind::List;
  w.label = "Today";
  w.items = {"09:00 Standup", "11:30 Dentist", "14:00 Review", "18:00 Gym"};
  return w;
}

Widget headline_widget() {
  Widget w;
  w.kind = Widget::Kind::Headline;
  w.label = "Now playing";
  w.value = "Blue in Green";
  w.text = "Miles Davis";
  return w;
}

hg::UiModel model(const Widget& w, int index = 0, int count = 4) {
  hg::UiModel m;
  m.screen = Screen::Widget;
  m.widget = w;
  m.widget_index = static_cast<uint8_t>(index);
  m.widget_count = static_cast<uint8_t>(count);
  return m;
}

// Every lit pixel is inside the glass (round) or a pixel clear of the panel's edge.
void check_inside(const Panel& p, bool round, const std::string& what) {
  const int w = p.width(), h = p.height(), cx = w / 2, cy = h / 2, r = std::min(w, h) / 2 - 1;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (p.at(x, y) == 0) continue;
      const int64_t dx = x - cx, dy = y - cy;
      const bool out = round ? dx * dx + dy * dy > static_cast<int64_t>(r) * r
                             : x < 1 || y < 1 || x >= w - 1 || y >= h - 1;
      if (out) {
        check::fail(__FILE__, __LINE__, what + ": ink outside at " + std::to_string(x) + "," + std::to_string(y));
        return;
      }
    }
}

const struct {
  int w, h;
  bool round;
} kPanels[] = {{466, 466, true}, {240, 240, true}, {360, 360, true}, {240, 135, false},
               {320, 172, false}, {320, 240, false}, {368, 448, false}, {800, 480, false}};

}  // namespace

TEST("widget face: every kind draws whole on round, small and large panels") {
  const Widget widgets[] = {value_widget(), progress_widget(62), list_widget(), headline_widget()};
  for (const auto& t : kPanels) {
    for (const Widget& w : widgets) {
      Panel p(t.w, t.h, t.round);
      p.ui.render(model(w));
      const std::string what = std::to_string(t.w) + "x" + std::to_string(t.h) + " kind " +
                               std::to_string(static_cast<int>(w.kind));
      check_inside(p, t.round, what);
      CHECK(p.count(kDim) > 0);    // the label
      CHECK(p.count(kText) > 0);   // the content, and the current widget's dot
      CHECK(p.count(kFaint) > 0);  // the other widgets' dots
      CHECK_EQ(p.display.flushed, t.h);  // the whole panel, once
    }
  }
}

TEST("widget face: at rest it moves with the burn-in shift and still stays inside") {
  const Widget widgets[] = {value_widget(), progress_widget(100), list_widget(), headline_widget()};
  for (const auto& t : kPanels) {
    for (const Widget& w : widgets) {
      Panel p(t.w, t.h, t.round);
      std::vector<uint16_t> first;
      bool moved = false;
      for (int minute = 0; minute < 9; ++minute) {  // the shift's whole cycle
        hg::UiModel m = model(w);
        m.widget_rest = true;
        m.clock_minute = static_cast<uint8_t>(minute);
        p.ui.render(m);
        check_inside(p, t.round, std::to_string(t.w) + "x" + std::to_string(t.h) + " minute " + std::to_string(minute));
        if (first.empty()) first = p.display.fb;
        moved = moved || p.display.fb != first;
      }
      CHECK(moved);
    }
  }
}

TEST("widget face: long text is cut to the box instead of leaving it") {
  Widget value = value_widget();
  value.label = "A label far too long for any of these small screens to hold";
  value.value = "1234567890123456789012345678901234567890";
  value.unit = "kilowatt-hours";
  Widget list = list_widget();
  list.items = {std::string(60, 'W'), std::string(60, 'M'), std::string(60, 'W'), std::string(60, 'M')};
  Widget headline = headline_widget();
  headline.value = "A headline that goes on and on well past two whole lines of any screen there is here";
  headline.text = "And a second line that is just as long, and then longer still, to be cut as well as the first";
  for (const auto& t : kPanels) {
    for (const Widget& w : {value, list, headline}) {
      Panel p(t.w, t.h, t.round);
      p.ui.render(model(w, 7, 8));
      check_inside(p, t.round, std::to_string(t.w) + "x" + std::to_string(t.h) + " long text");
      CHECK(p.count(kText) > 0);
    }
  }
}

TEST("widget face: a state colours the value, the bar and the headline") {
  Panel p(320, 240, false);
  Widget v = value_widget();
  p.ui.render(model(v, 0, 1));
  CHECK(p.count(kGreen) == 0);
  const size_t plain = p.count(kText);
  CHECK(plain > 0);
  v.tone = hg::DashRow::Tone::Good;
  p.ui.render(model(v, 0, 1));
  CHECK_EQ(p.count(kGreen), plain);  // the same digits, in green
  CHECK_EQ(p.count(kText), size_t(0));  // one widget: no dots either

  Widget h = headline_widget();
  h.tone = hg::DashRow::Tone::Bad;
  p.ui.render(model(h, 0, 1));
  CHECK(p.count(kRed) > 0);
  CHECK(p.count(kDim) > 0);  // the label and the second line keep their grey

  // The bar: amber without a state, the state's colour with one; the percentage stays white.
  Widget bar = progress_widget(50);
  p.ui.render(model(bar, 0, 1));
  CHECK(p.count(kAccent) > 0 && p.count(kText) > 0);
  bar.tone = hg::DashRow::Tone::Bad;
  p.ui.render(model(bar, 0, 1));
  CHECK(p.count(kAccent) == 0 && p.count(kRed) > 0 && p.count(kText) > 0);
}

TEST("widget face: the bar fills with the percentage, from nothing to its whole track") {
  for (const auto& t : kPanels) {
    Panel p(t.w, t.h, t.round);
    p.ui.render(model(progress_widget(0), 0, 1));
    CHECK_EQ(p.span(kAccent), -1);
    const int track = p.span(kFaint);
    CHECK(track > t.w / 3);
    int last = 0;
    for (int percent : {10, 25, 50, 75, 100}) {
      p.ui.render(model(progress_widget(percent), 0, 1));
      const int fill = p.span(kAccent);
      CHECK(fill > last);
      last = fill;
    }
    CHECK_EQ(last, track);                // full: the fill covers the track
    CHECK_EQ(p.span(kFaint), -1);
  }
}

TEST("widget face: one dot per widget, the current one lit, and none for a single widget") {
  Panel p(466, 466, true);
  Widget w = list_widget();
  w.items.clear();  // nothing but the label and the dots
  w.label.clear();
  p.ui.render(model(w, 0, 1));
  CHECK_EQ(p.count(kText) + p.count(kFaint), size_t(0));
  p.ui.render(model(w, 2, 5));
  const size_t lit = p.count(kText), rest = p.count(kFaint);
  CHECK(lit > 0);
  CHECK_EQ(rest, 4 * lit);
}

TEST("widget face: it repaints on a change and after another screen, and not otherwise") {
  Panel p(320, 240, false);
  hg::UiModel m = model(value_widget());
  p.ui.render(m);
  CHECK_EQ(p.display.flushed, 240);
  m.frame = 7;  // the app's animation frame moves on: nothing of the widget does
  p.ui.render(m);
  CHECK_EQ(p.display.flushed, 240);
  m.widget.value = "21.6";
  p.ui.render(m);
  CHECK_EQ(p.display.flushed, 480);
  m.widget_index = 1;
  p.ui.render(m);
  CHECK_EQ(p.display.flushed, 720);
  // The Ready screen and back: both repaint the whole panel, with nothing of the other left.
  hg::UiModel ready;
  ready.screen = Screen::Ready;
  ready.hero = true;
  ready.headline = "Hi, I'm Hermes";
  p.ui.render(ready);
  const int after_ready = p.display.flushed;
  CHECK(after_ready >= 960);
  p.ui.render(m);
  CHECK_EQ(p.display.flushed, after_ready + 240);
  Panel fresh(320, 240, false);
  fresh.ui.render(m);
  CHECK(p.display.fb == fresh.display.fb);
}
