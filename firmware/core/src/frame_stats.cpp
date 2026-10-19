#include "hg/frame_stats.hpp"

#include <algorithm>
#include <cstdio>

namespace hg {

void Series::add(uint32_t value) {
  if (capacity_ == 0) return;
  if (values_.size() < capacity_) {
    values_.push_back(value);
    return;
  }
  values_[next_] = value;
  next_ = (next_ + 1) % capacity_;
}

void Series::clear() {
  values_.clear();
  next_ = 0;
}

Series::Summary Series::summary() const {
  Summary s;
  s.count = values_.size();
  if (values_.empty()) return s;
  std::vector<uint32_t> sorted(values_);
  std::sort(sorted.begin(), sorted.end());
  uint64_t sum = 0;
  for (uint32_t v : sorted) sum += v;
  // Nearest rank: the smallest value with at least p% of the window at or below it.
  auto rank = [&](unsigned pct) { return sorted[(sorted.size() * pct + 99) / 100 - 1]; };
  s.min = sorted.front();
  s.max = sorted.back();
  s.avg = static_cast<uint32_t>(sum / sorted.size());
  s.p50 = rank(50);
  s.p95 = rank(95);
  return s;
}

void FrameStats::add_frame(uint32_t frame_us, uint32_t flush_us, uint32_t bands) {
  if (bands == 0) {
    ++idle_;
    return;
  }
  ++frames_;
  if (frame_us > budget_us_) ++missed_;
  flush_us = std::min(flush_us, frame_us);
  render_.add(frame_us - flush_us);
  flush_.add(flush_us);
  frame_.add(frame_us);
  bands_.add(bands);
}

void FrameStats::reset() {
  frames_ = missed_ = idle_ = 0;
  render_.clear();
  flush_.clear();
  frame_.clear();
  bands_.clear();
  input_.clear();
}

namespace {

json::Value summary_json(const Series::Summary& s) {
  json::Value o = json::Value::object();
  o.set("count", static_cast<unsigned long>(s.count))
      .set("min", s.min)
      .set("avg", s.avg)
      .set("p50", s.p50)
      .set("p95", s.p95)
      .set("max", s.max);
  return o;
}

// Microseconds as milliseconds with one decimal: "16.7".
std::string ms(uint32_t us) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%lu.%lu", static_cast<unsigned long>(us / 1000),
                static_cast<unsigned long>(us % 1000 / 100));
  return buf;
}

std::string phase(const char* name, const Series::Summary& s) {
  return std::string(name) + " " + ms(s.p50) + "/" + ms(s.p95) + "/" + ms(s.max);
}

}  // namespace

json::Value FrameStats::to_json() const {
  json::Value o = json::Value::object();
  o.set("budget_us", budget_us_)
      .set("frames", frames_)
      .set("missed", missed_)
      .set("idle", idle_)
      .set("render_us", summary_json(render()))
      .set("flush_us", summary_json(flush()))
      .set("frame_us", summary_json(frame()))
      .set("bands", summary_json(bands()))
      .set("input_us", summary_json(input()));
  return o;
}

std::string FrameStats::log_line() const {
  const Series::Summary b = bands(), in = input();
  std::string line = "frames " + std::to_string(frames_) + " (missed " + std::to_string(missed_) + " > " +
                     ms(budget_us_) + " ms, idle " + std::to_string(idle_) + "), ms p50/p95/max: " +
                     phase("render", render()) + ", " + phase("flush", flush()) + ", " + phase("frame", frame()) +
                     "; bands avg " + std::to_string(b.avg) + " max " + std::to_string(b.max);
  if (in.count) line += "; input " + ms(in.p50) + "/" + ms(in.p95) + "/" + ms(in.max) + " ms";
  return line;
}

}  // namespace hg
