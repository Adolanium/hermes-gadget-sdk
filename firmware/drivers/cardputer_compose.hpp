// Local text editor over the normal Gadget display. The portable App and
// protocol stay unchanged. Closing requests a full repaint of the core's UI.
#pragma once
#include <string>
#include "hg/canvas.hpp"
#include "hg/hal.hpp"

namespace hg {
class CardputerCompose final : public Display {
 public:
  explicit CardputerCompose(Display& panel) : panel_(panel) {}
  DisplayInfo info() const override { return panel_.info(); }
  uint16_t* framebuffer() override { return panel_.framebuffer(); }
  void set_backlight(uint8_t percent) override { panel_.set_backlight(percent); }
  /** Consume our restore request and the underlying panel's redraw request. */
  bool take_redraw_request() override {
    const bool dirty = repaint_;
    repaint_ = false;
    return panel_.take_redraw_request() || dirty;
  }
  /** Keep an active draft visible over core redraws of the specified rows. */
  void flush(uint16_t y0, uint16_t y1) override {
    if (active_) paint(y0, y1);
    panel_.flush(y0, y1);
  }
  bool active() const { return active_; }
  const std::string& draft() const { return draft_; }
  static constexpr size_t kLimit = 256;
  /** Open the editor, append up to kLimit ASCII bytes, and repaint it. */
  void append(char ch) {
    active_ = true;
    if (draft_.size() < kLimit) draft_ += ch;
    refresh();
  }
  /** Remove the last draft byte if present, then repaint the editor. */
  void backspace() {
    if (!draft_.empty()) draft_.pop_back();
    refresh();
  }
  /** Discard the draft and request a core repaint even if its model is unchanged. */
  void close() {
    active_ = false;
    draft_.clear();
    repaint_ = true;
  }
 private:
  void refresh() { paint(0, info().height); panel_.flush(0, info().height); }
  /** Paint clipped editor rows, keeping the end of a wrapped draft in view. */
  void paint(int y0, int y1) {
    const auto d = info();
    Canvas c(framebuffer(), d.width, d.height, d.swap_bytes);
    c.set_clip_rows(y0, y1);
    constexpr auto bg = rgb565(12, 15, 20), fg = rgb565(235, 237, 240), dim = rgb565(135, 150, 165);
    c.fill_rect(0, 0, d.width, d.height, bg);
    c.text(5, 4, "Message to Hermes", 1, fg);
    c.fill_rect(5, 16, d.width - 10, 1, dim);
    const int cols = (d.width - 10) / 6;
    auto lines = wrap_text(draft_ + "_", cols);
    const int rows = (d.height - 38) / 10;
    const size_t first = lines.size() > static_cast<size_t>(rows) ? lines.size() - rows : 0;
    for (size_t i = first; i < lines.size(); ++i)
      c.text(5, 22 + static_cast<int>(i - first) * 10, lines[i], 1, fg);
    c.text(5, d.height - 12, "Enter:send  Fn+Esc:discard", 1, dim);
  }
  Display& panel_;
  std::string draft_;
  bool active_ = false, repaint_ = false;
};
}  // namespace hg
