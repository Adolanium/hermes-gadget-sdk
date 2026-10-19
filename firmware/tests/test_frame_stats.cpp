// Frame timing: the rolling statistics, the renderer's timers on a fake clock,
// and the App's `perf` command and bench.
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/app.hpp"
#include "hg/frame_stats.hpp"
#include "hg/ui.hpp"

using hg::json::Value;

namespace {

// A display whose flush takes kFlushUs on the fake clock, and a clock that
// advances kReadUs every time it is read, so drawing takes time too.
struct FakeDevice : hg::Display, hg::System, hg::Transport {
  static constexpr uint64_t kFlushUs = 500;
  static constexpr uint64_t kReadUs = 7;

  uint32_t clock_ms = 1000;
  uint64_t clock_us = 1000000;
  int us_reads = 0;
  int flushes = 0;
  std::vector<std::string> logs;
  std::vector<uint16_t> fb = std::vector<uint16_t>(320 * 240, 0);

  uint32_t now_ms() override { return clock_ms; }
  uint64_t now_us() override {
    ++us_reads;
    clock_us += kReadUs;
    return clock_us;
  }
  void random_bytes(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i);
  }
  void log(hg::LogLevel, std::string_view message) override { logs.emplace_back(message); }

  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = 320;
    d.height = 240;
    return d;
  }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t, uint16_t) override {
    ++flushes;
    clock_us += kFlushUs;
  }

  void connect(const std::string&, const std::string&) override {}
  bool send_text(std::string_view) override { return true; }
  bool send_binary(const uint8_t*, size_t) override { return true; }
  void close() override {}

  hg::Hal hal() {
    hg::Hal h;
    h.system = this;
    h.transport = this;
    h.display = this;
    return h;
  }
  bool logged(const std::string& prefix) const {
    for (const auto& l : logs)
      if (l.rfind(prefix, 0) == 0) return true;
    return false;
  }
};

hg::UiModel text_model(const std::string& body) {
  hg::UiModel m;
  m.screen = hg::Screen::Card;
  m.title = "Bench";
  m.headline = "Card";
  m.body = body;
  m.hint = "hint";
  return m;
}

Value perf(hg::App& app) {
  std::string out = app.console("perf");
  CHECK(out.rfind("@perf ", 0) == 0);
  Value v;
  CHECK(hg::json::parse(out.substr(6), v));
  return v;
}

}  // namespace

TEST("frame stats: summaries give min, average, nearest-rank percentiles and max") {
  hg::Series s(1000);
  CHECK_EQ(s.summary().count, size_t{0});
  CHECK_EQ(s.summary().max, 0u);
  for (uint32_t v = 100; v >= 1; --v) s.add(v);  // order must not matter
  const auto sum = s.summary();
  CHECK_EQ(sum.count, size_t{100});
  CHECK_EQ(sum.min, 1u);
  CHECK_EQ(sum.max, 100u);
  CHECK_EQ(sum.avg, 50u);  // 50.5, rounded down
  CHECK_EQ(sum.p50, 50u);
  CHECK_EQ(sum.p95, 95u);

  hg::Series one(8);
  one.add(42);
  CHECK_EQ(one.summary().p50, 42u);
  CHECK_EQ(one.summary().p95, 42u);
}

TEST("frame stats: the window keeps only the newest values") {
  hg::Series s(4);
  for (uint32_t v = 1; v <= 6; ++v) s.add(v);
  auto sum = s.summary();
  CHECK_EQ(sum.count, size_t{4});
  CHECK_EQ(sum.min, 3u);
  CHECK_EQ(sum.max, 6u);
  s.clear();
  CHECK_EQ(s.summary().count, size_t{0});
}

TEST("frame stats: frames over the budget count as missed; empty renders as idle") {
  hg::FrameStats st(10000);
  st.add_frame(8000, 3000, 2);
  st.add_frame(10000, 4000, 1);  // exactly on budget is in time
  st.add_frame(12000, 9000, 4);
  st.add_frame(500, 0, 0);       // sent nothing
  CHECK_EQ(st.frames(), 3u);
  CHECK_EQ(st.missed(), 1u);
  CHECK_EQ(st.idle(), 1u);
  CHECK_EQ(st.render().max, 6000u);  // frame minus flush
  CHECK_EQ(st.render().min, 3000u);
  CHECK_EQ(st.flush().max, 9000u);
  CHECK_EQ(st.bands().max, 4u);
  CHECK_EQ(st.frame().min, 8000u);

  const Value j = st.to_json();
  CHECK_EQ(j["missed"].as_int(), 1);
  CHECK_EQ(j["frame_us"]["max"].as_int(), 12000);
  CHECK(st.log_line().find("frames 3 (missed 1 > 10.0 ms") == 0);

  st.reset();
  CHECK_EQ(st.frames(), 0u);
  CHECK_EQ(st.frame().count, size_t{0});
}

TEST("frame stats: a renderer that isn't measuring never reads the clock") {
  FakeDevice dev;
  hg::Ui ui(dev);
  ui.mark_input();
  ui.render(text_model("one"));
  ui.render(text_model("two"));
  CHECK(dev.flushes > 0);
  CHECK_EQ(dev.us_reads, 0);
}

TEST("frame stats: the renderer times drawing, flushing and the bands it sends") {
  FakeDevice dev;
  hg::Ui ui(dev);
  hg::FrameStats st;
  ui.measure(&st, &dev);
  ui.render(text_model("first"));  // everything is new: four bands
  CHECK_EQ(st.frames(), 1u);
  CHECK_EQ(st.bands().max, 4u);
  CHECK(st.flush().max >= 4 * FakeDevice::kFlushUs);
  CHECK(st.render().max > 0);  // the clock moved while drawing too
  CHECK_EQ(st.frame().max, st.flush().max + st.render().max);

  ui.render(text_model("first"));  // nothing changed
  CHECK_EQ(st.frames(), 1u);
  CHECK_EQ(st.idle(), 1u);

  ui.render(text_model("second"));  // the content band only
  CHECK_EQ(st.frames(), 2u);
  CHECK_EQ(st.bands().min, 1u);

  ui.measure(nullptr, &dev);
  const int reads = dev.us_reads;
  ui.render(text_model("third"));
  CHECK_EQ(dev.us_reads, reads);
  CHECK_EQ(st.frames(), 2u);
}

TEST("frame stats: input latency ends with the first frame that sends something") {
  FakeDevice dev;
  hg::Ui ui(dev);
  hg::FrameStats st;
  ui.measure(&st, &dev);
  ui.render(text_model("a"));
  ui.mark_input();
  dev.clock_us += 3000;  // the app works on the input
  ui.render(text_model("a"));  // nothing to send yet
  CHECK_EQ(st.input().count, size_t{0});
  ui.mark_input();  // a second input while the first waits doesn't restart the timer
  ui.render(text_model("b"));
  CHECK_EQ(st.input().count, size_t{1});
  CHECK(st.input().max >= 3000 + FakeDevice::kFlushUs);
  ui.render(text_model("c"));  // no input waiting
  CHECK_EQ(st.input().count, size_t{1});
}

TEST("frame stats: the app keeps frame timing off unless asked") {
  FakeDevice dev;
  hg::Hal hal = dev.hal();
  hg::App app(hal, hg::DeviceProfile{});
  app.begin();
  for (int i = 0; i < 30; ++i) {
    dev.clock_ms += 100;
    app.tick();
  }
  app.on_button(hg::Button::Talk, true);
  app.on_button(hg::Button::Talk, false);
  CHECK(dev.flushes > 0);
  CHECK_EQ(dev.us_reads, 0);
  const Value p = perf(app);
  CHECK(!p["enabled"].as_bool());
  CHECK(!p.has("frames"));
  CHECK(app.console("diag").find("\"frames\"") == std::string::npos);
}

TEST("frame stats: perf on measures the app's frames, inputs and logs a summary") {
  FakeDevice dev;
  hg::Hal hal = dev.hal();
  hg::DeviceProfile profile;
  profile.frame_log_ms = 5000;
  hg::App app(hal, profile);
  app.begin();
  CHECK_EQ(app.console("perf on"), std::string("@ok frame timing on"));
  for (int i = 0; i < 20; ++i) {  // past the boot screen, which animates
    dev.clock_ms += 100;
    app.tick();
  }
  app.on_button(hg::Button::Up, true);
  app.on_button(hg::Button::Up, false);
  for (int i = 0; i < 40; ++i) {
    dev.clock_ms += 100;
    app.tick();
  }
  const Value p = perf(app);
  CHECK(p["enabled"].as_bool());
  CHECK(p["frames"].as_int() > 0);
  CHECK_EQ(p["budget_us"].as_int(), 16667);
  CHECK(p["frame_us"]["p95"].as_int() >= p["frame_us"]["p50"].as_int());
  CHECK_EQ(p["input_us"]["count"].as_int(), 1);
  CHECK(dev.logged("perf: frames "));
  CHECK(app.console("diag").find("\"frames\"") != std::string::npos);

  CHECK_EQ(app.console("perf budget 1"), std::string("@ok frame budget 1 us"));
  CHECK_EQ(app.console("perf reset"), std::string("@ok frame timing reset"));
  CHECK_EQ(perf(app)["frames"].as_int(), 0);
  CHECK(app.console("perf budget nope").rfind("@error", 0) == 0);
  CHECK_EQ(app.console("perf off"), std::string("@ok frame timing off"));
  const int reads = dev.us_reads;
  for (int i = 0; i < 10; ++i) {
    dev.clock_ms += 100;
    app.tick();
  }
  CHECK_EQ(dev.us_reads, reads);
}

TEST("frame stats: profile.frame_stats turns timing on from boot") {
  FakeDevice dev;
  hg::Hal hal = dev.hal();
  hg::DeviceProfile profile;
  profile.frame_stats = true;
  profile.frame_budget_us = 33333;
  hg::App app(hal, profile);
  app.begin();
  const Value p = perf(app);
  CHECK(p["enabled"].as_bool());
  CHECK_EQ(p["budget_us"].as_int(), 33333);
  CHECK(p["frames"].as_int() >= 1);  // the boot screen
}

TEST("frame stats: the bench draws the same fixed workloads, then gives the screen back") {
  FakeDevice dev;
  hg::Hal hal = dev.hal();
  hg::App app(hal, hg::DeviceProfile{});
  app.begin();
  for (int i = 0; i < 20; ++i) {
    dev.clock_ms += 100;
    app.tick();
  }
  const hg::Screen before = app.screen();
  CHECK(app.console("perf bench all").rfind("@ok bench all: 360 frames", 0) == 0);
  CHECK(app.console("perf bench").rfind("@error a bench is running", 0) == 0);
  CHECK_EQ(perf(app)["bench_running"].as_string(), std::string("scroll"));
  for (int i = 0; i < 360; ++i) {
    dev.clock_ms += 10;
    app.tick();
  }
  const Value p = perf(app);
  CHECK(!p.has("bench_running"));
  CHECK(!p["enabled"].as_bool());  // the bench doesn't leave timing on
  for (const char* name : {"scroll", "mascot", "redraw"}) {
    const Value& r = p["bench"][name];
    CHECK_EQ(r["frames"].as_int(), 120);  // every bench frame changes the screen
    CHECK(r["flush_us"]["min"].as_int() >= static_cast<int>(FakeDevice::kFlushUs));
  }
  CHECK_EQ(p["bench"]["redraw"]["bands"]["min"].as_int(), 4);
  CHECK_EQ(p["bench"]["scroll"]["bands"]["max"].as_int(), 1);
  CHECK(dev.logged("perf bench redraw: frames 120"));
  CHECK(app.screen() == before);
  CHECK(app.console("perf bench sideways").rfind("@error usage", 0) == 0);

  // The same workload twice draws the same frames.
  app.console("perf bench scroll");
  const int flushes = dev.flushes;
  for (int i = 0; i < 120; ++i) app.tick();
  const int first = dev.flushes - flushes;
  app.console("perf bench scroll");
  for (int i = 0; i < 120; ++i) app.tick();
  CHECK_EQ(dev.flushes - flushes - first, first);
}
