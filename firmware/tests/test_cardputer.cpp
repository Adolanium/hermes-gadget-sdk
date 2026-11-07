#include "check.hpp"
#include "cardputer_keys.hpp"
#include "cardputer_compose.hpp"
#include "hg/ui.hpp"

namespace {
/** Encode a physical 4x14 key position as a TCA8418 7x8 FIFO event.
 * Synthetic presses/releases exercise decoding without an I2C device; they
 * do not establish that the real keyboard wiring has been physically checked.
 */
uint8_t event(int row, int col, bool down = true) {
  return (col / 2 * 10 + row + (col % 2) * 4 + 1) | (down ? 0x80 : 0);
}
using Kind = hg::CardputerKeys::Kind;
struct Panel : hg::Display {
  std::vector<uint16_t> pixels = std::vector<uint16_t>(240 * 135);
  int flushes = 0;
  hg::DisplayInfo info() const override { return {240, 135, true, true}; }
  uint16_t* framebuffer() override { return pixels.data(); }
  void flush(uint16_t, uint16_t) override { ++flushes; }
};
}

TEST("Cardputer: FIFO press/release and Shift follow the printed keyboard") {
  hg::CardputerKeys keys;
  CHECK_EQ(keys.feed(event(1, 1)).text, 'q');
  CHECK_EQ(keys.feed(event(1, 1)).kind, Kind::None);
  CHECK_EQ(keys.feed(event(1, 1, false)).kind, Kind::None);
  keys.feed(event(2, 1));  // Shift
  CHECK_EQ(keys.feed(event(1, 1)).text, 'Q');
  keys.feed(event(1, 1, false));
  CHECK_EQ(keys.feed(event(0, 2)).text, '@');
  keys.feed(event(2, 1, false));
  CHECK_EQ(keys.feed(event(3, 13)).text, ' ');
}

TEST("Cardputer: Fn navigation does not become text, reset clears lost modifiers") {
  hg::CardputerKeys keys;
  keys.feed(event(2, 0));
  CHECK_EQ(keys.feed(event(2, 11)).kind, Kind::Up);
  CHECK_EQ(keys.feed(event(3, 11)).kind, Kind::Down);
  CHECK_EQ(keys.feed(event(0, 0)).kind, Kind::Cancel);
  CHECK_EQ(keys.feed(event(2, 13)).kind, Kind::Settings);
  CHECK_EQ(keys.feed(event(1, 2)).kind, Kind::None);
  keys.reset();
  CHECK_EQ(keys.feed(event(1, 2)).text, 'w');
  CHECK_EQ(keys.feed(0).kind, Kind::None);
  CHECK_EQ(keys.feed(0xff).kind, Kind::None);
  CHECK_EQ(keys.feed(0x89).kind, Kind::None);  // unwired scan column 8
}

TEST("Cardputer: every printed character survives its own press and release") {
  hg::CardputerKeys keys;
  int characters = 0;
  for (int row = 0; row < 4; ++row) for (int col = 0; col < 14; ++col) {
    const auto key = keys.feed(event(row, col));
    if (key.kind == Kind::Character) ++characters;
    keys.feed(event(row, col, false));
  }
  CHECK_EQ(characters, 48);  // 56 minus five modifiers, Tab, Enter, Backspace
}

TEST("Cardputer: composing survives core redraw and closing restores the UI") {
  Panel panel;
  hg::CardputerCompose compose(panel);
  hg::Ui ui(compose);
  hg::UiModel model;
  model.screen = hg::Screen::Ready;
  model.title = "Cardputer";
  model.body = "Ready";
  ui.render(model);
  const auto ready = panel.pixels;
  compose.append('h'); compose.append('i');
  CHECK_EQ(compose.draft(), "hi");
  const auto editor = panel.pixels;
  ui.invalidate();
  ui.render(model);
  CHECK(panel.pixels == editor);
  compose.backspace();
  CHECK_EQ(compose.draft(), "h");
  for (int i = 0; i < 300; ++i) compose.append('x');
  CHECK_EQ(compose.draft().size(), hg::CardputerCompose::kLimit);
  compose.close();
  ui.render(model);  // no model change: proxy must still force repaint
  CHECK(panel.pixels == ready);
  CHECK(!compose.active());
  CHECK(compose.draft().empty());
}
