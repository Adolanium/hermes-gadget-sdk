#include "hg/touch.hpp"

#include <cstdlib>

namespace hg {

void TouchGestures::press(Button b) { app_.on_button(b, true); }
void TouchGestures::release(Button b) { app_.on_button(b, false); }

void TouchGestures::tick(uint32_t now_ms) {
  if (state_ == State::Settings && now_ms - t0_ >= 1000) {
    state_ = State::Ignored;
    app_.open_settings();
  }
  if (state_ == State::Pending && static_cast<int32_t>(now_ms - t0_) >= static_cast<int32_t>(cfg_.hold_ms)) {
    state_ = State::Talk;
    press(Button::Talk);
  }
}

void TouchGestures::update(bool touching, int x, int y, uint32_t now_ms) {
  if (!touching) {
    switch (state_) {
      case State::Pending:  // a quick tap
        if (from_clock_) break;  // Ready came back at touch-down; a tap does nothing else
        if (on_name_) {  // a quick tap on the name opens the list, or does nothing while it can't
          app_.open_profile_picker();
          break;
        }
        press(Button::Talk);
        release(Button::Talk);
        break;
      case State::Picker: app_.profile_tap(x0_, y0_); break;
      case State::Talk: release(Button::Talk); break;
      case State::Swipe: release(Button::Cancel); break;
      default: break;
    }
    state_ = State::Idle;
    return;
  }

  if (state_ == State::Idle) {
    // Before the wake: the touch that ends the clock face is only a wake, so its
    // tap lands on nothing and its hold talks to the Ready screen it brought back.
    from_clock_ = app_.clock_showing();
    if (app_.wake_display()) { state_ = State::Ignored; return; }
    state_ = app_.profile_picker_open() ? State::Picker
              : !from_clock_ && app_.settings_title_hit(x, y) ? State::Settings
              : State::Pending;
    on_name_ = !from_clock_ && state_ == State::Pending && app_.profile_name_hit(x, y);
    x0_ = x;
    y0_ = y;
    t0_ = now_ms;
    return;
  }

  const int dx = x - x0_, dy = y - y0_;
  const bool swiped_down = (cfg_.swipe_cancel || app_.settings_open() || app_.wifi_setup_open() ||
                           app_.phone_pairing_open() || app_.profile_picker_open()) &&
                          dy >= cfg_.swipe_px && std::abs(dx) < dy;
  switch (state_) {
    case State::Picker:  // a tap picks a row and a swipe down closes the list; a hold or a drag picks nothing
    case State::PickerDrag:
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        state_ = dy < 0 || std::abs(dx) >= std::abs(dy) ? State::Ignored : State::PickerDrag;
      }
      break;
    case State::Settings:
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        state_ = State::Ignored;
      } else {
        tick(now_ms);
      }
      break;
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
        release(Button::Talk);
      }
      break;
    default: break;
  }
}

}  // namespace hg
