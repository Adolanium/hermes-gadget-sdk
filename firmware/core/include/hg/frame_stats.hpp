// Frame timing: how long each Ui::render takes to draw and to flush, how many
// bands it sends, and how long an input waits for the frame that answers it.
//
// Off by default. Ui measures a frame only while it has a FrameStats, and reads
// the clock (System::now_us) only then, so the off path costs one null check per
// render. docs/development.md#measure-frame-timing explains how to collect a
// measurement.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hg/json.hpp"

namespace hg {

// The last `capacity` values of one measurement, and their summary.
class Series {
 public:
  struct Summary {
    size_t count = 0;  // values in the window
    uint32_t min = 0, avg = 0, p50 = 0, p95 = 0, max = 0;
  };

  explicit Series(size_t capacity) : capacity_(capacity) {}
  void add(uint32_t value);
  void clear();
  // Nearest-rank percentiles over the window; all zero when it is empty.
  Summary summary() const;

 private:
  size_t capacity_;
  std::vector<uint32_t> values_;  // a ring once full
  size_t next_ = 0;
};

class FrameStats {
 public:
  // 60 Hz: the frame budget the renderer is working towards.
  static constexpr uint32_t kDefaultBudgetUs = 16667;
  static constexpr size_t kWindow = 256;   // frames the summaries cover
  static constexpr size_t kInputWindow = 64;

  explicit FrameStats(uint32_t budget_us = kDefaultBudgetUs) : budget_us_(budget_us) {}

  // One Ui::render that sent `bands` regions to the panel. `frame_us` covers the
  // whole call; `flush_us` the part spent in Display::flush. A render that sends
  // nothing is counted as idle and leaves the timings alone.
  void add_frame(uint32_t frame_us, uint32_t flush_us, uint32_t bands);
  // From an input event (button, or a touch gesture's button) to the end of the
  // first frame after it that sent anything to the panel.
  void add_input(uint32_t latency_us) { input_.add(latency_us); }
  void reset();

  uint32_t budget_us() const { return budget_us_; }
  void set_budget_us(uint32_t us) { budget_us_ = us; }
  uint32_t frames() const { return frames_; }  // since the last reset
  uint32_t missed() const { return missed_; }  // frames longer than the budget
  uint32_t idle() const { return idle_; }      // renders with nothing to send

  Series::Summary render() const { return render_.summary(); }
  Series::Summary flush() const { return flush_.summary(); }
  Series::Summary frame() const { return frame_.summary(); }
  Series::Summary bands() const { return bands_.summary(); }
  Series::Summary input() const { return input_.summary(); }

  // Everything above, for the console's `perf` command and `diag`.
  json::Value to_json() const;
  // One line for the serial log: "frames 120 (missed 3 > 16.7 ms) render p50 ..."
  std::string log_line() const;

 private:
  uint32_t budget_us_;
  uint32_t frames_ = 0, missed_ = 0, idle_ = 0;
  Series render_{kWindow}, flush_{kWindow}, frame_{kWindow}, bands_{kWindow}, input_{kInputWindow};
};

}  // namespace hg
