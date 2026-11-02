// Touchscreen gestures mapped onto the gadget's buttons, for boards whose
// screen is the main input:
//
//   hold the title     local settings after one second
//   hold elsewhere     TALK, held for as long as the finger stays down (on the name too)
//   quick tap          a TALK press and release (answers "yes" to a question); on the
//                      name under the mascot it opens the profile grid instead; on the
//                      resting clock it does nothing (the touch-down brought Ready back)
//   swipe sideways     on the Ready screen, opens the profile grid; on the grid, turns its page
//   tap a grid cell    asks Hermes for that profile
//   in settings        a drag scrolls the list (or moves the selected row's slider,
//                      when it starts on it), a tap changes the row under it; with a
//                      hardware check running, a tap and a swipe down are TALK and CANCEL
//   swipe down         a CANCEL press and release (discard, close, stop, "no")
//
// With the touch_talk setting off, holds and taps press TALK only where it doesn't
// record (settings, a question, a ringing alarm); elsewhere they do nothing, and the
// name opens the profile grid in tap-to-talk mode too.
//
// Feed it raw touch samples; it calls App::on_button. Portable and clock-free,
// so the simulator and the tests drive it exactly like the firmware does.
#pragma once

#include <cstdint>

#include "hg/app.hpp"

namespace hg {

class TouchGestures {
 public:
  struct Config {
    uint32_t hold_ms = 120;  // a still finger becomes TALK after this long
    int slop_px = 18;        // movement allowed before a touch stops being a hold or tap
    int swipe_px = 60;       // travel that makes a swipe, down or sideways
    bool swipe_cancel = true;
  };

  explicit TouchGestures(App& app) : app_(app) {}
  TouchGestures(App& app, Config config) : app_(app), cfg_(config) {}

  void set_swipe_cancel(bool on) { cfg_.swipe_cancel = on; }

  // Report the touch state. Call for every controller sample and when the
  // finger lifts (`touching` false; x and y are then ignored).
  void update(bool touching, int x, int y, uint32_t now_ms);
  // Promotes a still finger to TALK even when the controller sends no new
  // samples. Call regularly (every app tick is fine).
  void tick(uint32_t now_ms);

 private:
  enum class State : uint8_t { Idle, Pending, Settings, Picker, PickerDrag, Side, List, ListDrag, Talk, Swipe, Ignored };
  void press(Button b);
  void release(Button b);

  App& app_;
  Config cfg_;
  State state_ = State::Idle;
  int x0_ = 0, y0_ = 0;
  bool on_name_ = false;     // a Pending touch that started on the profile name
  bool from_clock_ = false;  // the touch began on the clock face, which the touch-down ended
  bool in_picker_ = false;   // a Side touch began on the profile grid (turns its page) rather than Ready
  int dx_ = 0, dy_ = 0;      // a Side touch's travel so far
  uint32_t t0_ = 0;
};

}  // namespace hg
