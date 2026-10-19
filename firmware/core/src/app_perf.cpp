// Frame timing: the console's `perf` command, the periodic log line, and the
// bench, a fixed workload drawn the same way on every board and in the simulator
// so a renderer change can be measured before and after.
#include <algorithm>
#include <cstdlib>

#include "hg/app.hpp"

namespace hg {
namespace {

constexpr int kBenchFrames = 120;  // per workload

const char* bench_name(int b) {
  switch (b) {
    case 1: return "scroll";
    case 2: return "mascot";
    case 3: return "redraw";
    default: return "none";
  }
}

// A long reply: the same text on every run, so every run draws the same glyphs.
std::string bench_transcript() {
  static const char* const kWords[] = {"the", "gadget", "renders", "each", "band", "of", "text", "from", "a",
                                       "model", "and", "flushes", "only", "what", "changed", "since", "last",
                                       "frame", "while", "Hermes", "keeps", "talking"};
  constexpr size_t kCount = sizeof(kWords) / sizeof(kWords[0]);
  std::string out;
  for (size_t p = 0; p < 60; ++p) {
    if (p) out += '\n';
    out += std::to_string(p + 1) + ".";
    for (size_t w = 0; w < 16; ++w) out += std::string(" ") + kWords[(p * 7 + w * 3) % kCount];
  }
  return out;
}

}  // namespace

void App::render(const UiModel& m) {
  if (ui_ && bench_ == Bench::None) ui_->render(m);
}

void App::set_frame_stats(bool on) {
  if (on && !frame_stats_) {
    frame_stats_ = std::make_unique<FrameStats>(profile_.frame_budget_us);
    frame_log_at_ = now();
    frame_log_frames_ = 0;
  } else if (!on) {
    frame_stats_.reset();
  }
  if (ui_ && bench_ == Bench::None) ui_->measure(frame_stats_.get(), hal_.system);
}

std::string App::perf_command(std::string_view args) {
  if (args.empty()) {
    json::Value r = frame_stats_ ? frame_stats_->to_json() : json::Value::object();
    r.set("enabled", frame_stats_ != nullptr);
    if (bench_ != Bench::None) r.set("bench_running", bench_name(static_cast<int>(bench_)));
    r.set("bench", bench_results_);
    return "@perf " + r.dump();
  }
  if (args == "on") {
    set_frame_stats(true);
    return "@ok frame timing on";
  }
  if (args == "off") {
    set_frame_stats(false);
    return "@ok frame timing off";
  }
  if (args == "reset") {
    if (frame_stats_) frame_stats_->reset();
    frame_log_frames_ = 0;
    return "@ok frame timing reset";
  }
  if (args.substr(0, 7) == "budget ") {
    const std::string n(args.substr(7));
    char* end = nullptr;
    const unsigned long us = std::strtoul(n.c_str(), &end, 10);
    if (n.empty() || *end || us == 0 || us > 10000000) return "@error usage: perf budget <microseconds, 1..10000000>";
    profile_.frame_budget_us = static_cast<uint32_t>(us);
    bench_stats_.set_budget_us(profile_.frame_budget_us);
    if (frame_stats_) frame_stats_->set_budget_us(profile_.frame_budget_us);
    return "@ok frame budget " + n + " us";
  }
  if (args == "bench" || args.substr(0, 6) == "bench ") {
    const std::string_view which = args.size() > 6 ? args.substr(6) : std::string_view("all");
    Bench first = Bench::None, last = Bench::None;
    if (which == "scroll") first = last = Bench::Scroll;
    else if (which == "mascot") first = last = Bench::Mascot;
    else if (which == "redraw") first = last = Bench::Redraw;
    else if (which == "all") first = Bench::Scroll, last = Bench::Redraw;
    else return "@error usage: perf bench [scroll | mascot | redraw | all]";
    if (!ui_) return "@error this device has no display";
    if (bench_ != Bench::None) return "@error a bench is running";
    if (display_sleeping_) return "@error the screen is off";
    bench_ = first;
    bench_last_ = last;
    bench_frame_ = 0;
    bench_results_ = json::Value::object();
    return std::string("@ok bench ") + std::string(which) + ": " +
           std::to_string(kBenchFrames * (static_cast<int>(last) - static_cast<int>(first) + 1)) +
           " frames, one per tick; read the result with perf";
  }
  return "@error usage: perf [on | off | reset | budget <us> | bench [scroll | mascot | redraw | all]]";
}

void App::perf_tick() {
  if (bench_ != Bench::None) bench_step();
  if (!frame_stats_ || !profile_.frame_log_ms || now() - frame_log_at_ < profile_.frame_log_ms) return;
  frame_log_at_ = now();
  // Quiet while nothing is drawn (a dark screen, an idle text screen).
  if (frame_stats_->frames() == frame_log_frames_) return;
  frame_log_frames_ = frame_stats_->frames();
  log(LogLevel::Info, "perf: " + frame_stats_->log_line());
}

void App::bench_step() {
  UiModel& m = bench_model_;
  if (bench_frame_ == 0) {
    // Start a workload: the app's bars, a fixed body, fresh statistics.
    m = model_;
    m.qr = nullptr;
    m.color_test = false;
    m.yes.clear();
    m.no.clear();
    m.code.clear();
    m.level = 0;
    m.frame = 0;
    m.hint = "perf bench";
    m.hero = bench_ == Bench::Mascot;
    m.speaking = bench_ == Bench::Mascot;
    m.screen = bench_ == Bench::Mascot ? Screen::Responding : Screen::Card;
    m.headline = std::string("Bench: ") + bench_name(static_cast<int>(bench_));
    m.detail = m.hero ? "Talking for the frame timer" : "";
    m.body = m.hero ? "" : bench_transcript();
    m.scroll = 0;
    // The first, full draw sets the scene and isn't counted.
    ui_->invalidate();
    ui_->render(m);
    bench_stats_ = FrameStats(profile_.frame_budget_us);
    ui_->measure(&bench_stats_, hal_.system);
  }
  const int i = bench_frame_ + 1;  // frame 0 was the scene
  switch (bench_) {
    case Bench::Scroll: {
      // One line per frame, down to the end and back.
      const int lines = static_cast<int>(wrap_text(m.body, ui_->layout().body_cols).size());
      const int span = std::max(1, lines - ui_->body_rows(m));
      const int step = i % (2 * span);
      m.scroll = step <= span ? step : 2 * span - step;
      break;
    }
    case Bench::Mascot: m.frame = static_cast<uint32_t>(2 * i); break;  // the mouth moves every 2 frames
    case Bench::Redraw: ui_->invalidate(); break;
    case Bench::None: return;
  }
  ui_->render(m);
  if (++bench_frame_ < kBenchFrames) return;

  const char* name = bench_name(static_cast<int>(bench_));
  bench_results_.set(name, bench_stats_.to_json());
  log(LogLevel::Info, std::string("perf bench ") + name + ": " + bench_stats_.log_line());
  bench_frame_ = 0;
  if (bench_ != bench_last_) {
    bench_ = static_cast<Bench>(static_cast<int>(bench_) + 1);
    return;
  }
  bench_ = Bench::None;
  ui_->measure(frame_stats_.get(), hal_.system);
  ui_->invalidate();
  update_model();
}

}  // namespace hg
