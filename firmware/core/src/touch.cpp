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
        if (app_.show_reply_text()) break;  // the caption under the talking face gives way to the text
        if (on_name_) {  // a quick tap on the name opens the grid, or does nothing while it can't
          app_.open_profile_picker();
          break;
        }
        press(Button::Talk);
        release(Button::Talk);
        break;
      case State::Picker: app_.profile_tap(x0_, y0_); break;
      case State::Side:  // a sideways swipe: travel enough, and more sideways than down or up
        if (std::abs(dx_) >= cfg_.swipe_px && std::abs(dx_) > 2 * std::abs(dy_)) {
          if (in_picker_) app_.profile_page_step(dx_ < 0 ? 1 : -1);  // the next page comes in from the right
          else if (dx_ < 0) app_.open_profile_picker();  // from right to left, as the grid's next page
        }
        break;
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
    if (app_.touch_ignored()) { state_ = State::Ignored; return; }  // touch_wake off: a key wakes it
    if (app_.wake_display()) { state_ = State::Ignored; return; }
    app_.close_notices_for_input();  // a low battery alert goes, and the touch acts on what is under it
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
    case State::Picker:  // a tap picks a cell, a swipe sideways turns the page and a swipe down closes the grid
    case State::PickerDrag:
      if (swiped_down) {
        state_ = State::Swipe;
        press(Button::Cancel);
      } else if (state_ == State::Picker && std::abs(dx) > cfg_.slop_px && std::abs(dx) >= std::abs(dy)) {
        state_ = State::Side;
        in_picker_ = true;
        dx_ = dx, dy_ = dy;
      } else if (std::abs(dx) > cfg_.slop_px || std::abs(dy) > cfg_.slop_px) {
        state_ = dy < 0 || std::abs(dx) >= std::abs(dy) ? State::Ignored : State::PickerDrag;
      }
      break;
    case State::Side:
      dx_ = dx, dy_ = dy;
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
        // Neither a hold nor a downward swipe yet: one from right to left may open the profile
        // grid; wait for a clear swipe, else ignore.
        if (dx < 0 && std::abs(dx) >= std::abs(dy) && !from_clock_ && app_.profile_swipe_opens()) {
          state_ = State::Side;
          in_picker_ = false;
          dx_ = dx, dy_ = dy;
        } else if (dy < 0 || std::abs(dx) >= std::abs(dy)) {
          state_ = State::Ignored;
        }
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
