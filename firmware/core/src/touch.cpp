#include "hg/touch.hpp"

#include <cstdlib>

namespace hg {

void TouchGestures::press(Button b) { app_.on_button(b, true); }
void TouchGestures::release(Button b) { app_.on_button(b, false); }

void TouchGestures::tick(uint32_t now_ms) {
  if (state_ == State::Pending && static_cast<int32_t>(now_ms - t0_) >= static_cast<int32_t>(cfg_.hold_ms)) {
    state_ = State::Talk;
    press(Button::Talk);
  }
}

void TouchGestures::update(bool touching, int x, int y, uint32_t now_ms) {
  if (!touching) {
    switch (state_) {
      case State::Pending:  // a quick tap
        press(Button::Talk);
        release(Button::Talk);
        break;
      case State::Talk: release(Button::Talk); break;
      case State::Swipe: release(Button::Cancel); break;
      default: break;
    }
    state_ = State::Idle;
    return;
  }

  if (state_ == State::Idle) {
    state_ = State::Pending;
    x0_ = x;
    y0_ = y;
    t0_ = now_ms;
    return;
  }

  const int dx = x - x0_, dy = y - y0_;
  const bool swiped_down = cfg_.swipe_cancel && dy >= cfg_.swipe_px && std::abs(dx) < dy;
  switch (state_) {
    case State::Pending:
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        // Neither a hold nor a downward swipe yet: wait for a clear swipe, else ignore.
        if (dy < 0 || std::abs(dx) >= std::abs(dy)) state_ = State::Ignored;
      } else {
        tick(now_ms);
      }
      break;
    case State::Talk:
      if (swiped_down) {
        // Swiping down while holding throws the recording away. A whole CANCEL
        // tap does that; letting go of TALK would send it instead.
        state_ = State::Ignored;
        press(Button::Cancel);
        release(Button::Cancel);
      }
      break;
    default: break;
  }
}

}  // namespace hg
