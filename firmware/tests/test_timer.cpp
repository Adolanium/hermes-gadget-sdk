// Timers: the device actions, the countdown, the alarm and its beeps, the screen
// it lights, and the pixels of the edge arc, the top-bar line and the alarm screen.
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/app.hpp"
#include "hg/canvas.hpp"
#include "hg/crypto.hpp"

using hg::json::Value;
using hg::Screen;

namespace {

// The colours the renderer uses for the timer (ui.cpp).
constexpr uint16_t kBg = hg::rgb565(10, 14, 20);
constexpr uint16_t kAccent = hg::rgb565(242, 179, 61);
constexpr uint16_t kAccentDim = hg::rgb565(110, 82, 30);
constexpr uint16_t kGreen = hg::rgb565(61, 214, 140);
constexpr uint16_t kAlarmInk = hg::rgb565(4, 40, 24);
constexpr uint16_t kDim = hg::rgb565(132, 146, 160);
constexpr uint16_t kText = hg::rgb565(232, 238, 242);

// A board's parts, with a clock the test moves. A send can move the clock too (`send_ms`).
struct TimerHal : hg::Display, hg::AudioIn, hg::AudioOut, hg::Transport, hg::Storage, hg::System {
  // System
  uint32_t clock = 1000;
  uint32_t send_ms = 0;  // each send takes this long, as on hardware (0: the clock stands still)
  uint32_t now_ms() override { return clock; }
  void random_bytes(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i);
  }
  void log(hg::LogLevel, std::string_view) override {}

  // Transport
  std::vector<Value> sent;
  bool send_text(std::string_view text) override {
    clock += send_ms;
    Value v;
    if (hg::json::parse(text, v)) sent.push_back(v);
    return true;
  }
  bool send_binary(const uint8_t*, size_t) override {
    clock += send_ms;
    return true;
  }
  void connect(const std::string&, const std::string&) override {}
  void close() override {}

  // Storage
  std::map<std::string, std::string> kv;
  std::optional<std::string> get(std::string_view key) override {
    auto it = kv.find(std::string(key));
    if (it == kv.end()) return std::nullopt;
    return it->second;
  }
  bool set(std::string_view key, std::string_view value) override {
    kv[std::string(key)] = std::string(value);
    return true;
  }
  void erase(std::string_view key) override { kv.erase(std::string(key)); }

  // Display
  int width = 466, height = 466;
  bool round = true;
  std::vector<uint16_t> fb = std::vector<uint16_t>(466 * 466, 0);
  int brightness = -1;  // the last backlight level set: 0 while the screen sleeps
  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = static_cast<uint16_t>(width);
    d.height = static_cast<uint16_t>(height);
    d.round = round;
    d.has_backlight = true;
    return d;
  }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t, uint16_t) override {}
  void set_backlight(uint8_t percent) override { brightness = percent; }
  void panel(int w, int h, bool round_panel) {
    width = w;
    height = h;
    round = round_panel;
    fb.assign(static_cast<size_t>(w * h), 0);
  }
  uint16_t at(int x, int y) const { return fb[static_cast<size_t>(y * width + x)]; }

  // Mic
  bool mic_on = false;
  bool start(uint32_t) override { return mic_on = true; }
  void stop() override { mic_on = false; }

  // Speaker: every beep is one write. Like a port, it stays busy after end() until the
  // stream's audio has played: its length at the rate begin() opened it with.
  bool spk_open = false;
  uint32_t spk_rate = 16000, stream_samples = 0, playing_until = 0;
  size_t writes = 0, samples = 0;
  bool begin(uint32_t rate) override {
    spk_rate = rate;
    stream_samples = 0;
    return spk_open = true;
  }
  void write(const int16_t*, size_t n) override {
    ++writes;
    samples += n;
    stream_samples += static_cast<uint32_t>(n);
  }
  void end() override {
    spk_open = false;
    playing_until = clock + stream_samples * 1000 / spk_rate;
  }
  void abort() override {
    spk_open = false;
    playing_until = clock;
  }
  bool busy() const override { return spk_open || static_cast<int32_t>(playing_until - clock) > 0; }

  hg::Hal hal() {
    hg::Hal h;
    h.system = this;
    h.transport = this;
    h.storage = this;
    h.display = this;
    h.mic = this;
    h.speaker = this;
    return h;
  }

  const Value* last(const std::string& type) const {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      if ((*it)["type"].as_string() == type) return &*it;
    return nullptr;
  }
};

// A battery at 80 percent, not charging, for the battery strip.
struct TimerPower : hg::Power {
  std::optional<hg::PowerStatus> read() override {
    hg::PowerStatus p;
    p.battery_present = true;
    p.battery_percent = 80;
    p.external_power = false;
    p.charging = false;
    return p;
  }
  bool power_off() override { return false; }
};

// The last timer.done event the device sent.
const Value* timer_done(const TimerHal& f) {
  const Value* found = nullptr;
  for (const Value& v : f.sent)
    if (v["type"].as_string() == "event" && v["name"].as_string() == "timer.done") found = &v;
  return found;
}

struct Rig {
  TimerHal fake;
  TimerPower power;
  hg::Hal hal = fake.hal();
  hg::App app;
  int ids = 0;

  // A touch board: the screen stands in for the buttons.
  explicit Rig(bool touch = true) : app(hal, profile(touch)) { hal.power = &power; }

  static hg::DeviceProfile profile(bool touch) {
    hg::DeviceProfile p;
    p.board = "test-board";
    p.firmware = "1.2.3";
    p.default_server_url = "ws://hermes.local:8765/gadget";
    p.touch_screen = touch;
    if (touch) p.cancel_label = "Swipe down";
    return p;
  }

  void advance(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      fake.clock += 10;
      app.tick();
    }
  }

  // Like advance, but answers a ping each second, so the session stays up.
  void idle(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 1000) {
      advance(std::min<uint32_t>(1000, ms - t));
      server(R"({"type":"ping"})");
    }
  }

  void server(const std::string& json) { app.on_transport_text(json); }

  // Boots, connects and is approved: the Ready screen.
  void online() {
    app.begin();
    app.on_network(true, "test-wifi");
    advance(1000);
    app.on_transport_open();
    server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
    server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true})");
  }

  // Calls a device action the way Hermes does; returns the action.result it sent.
  const Value* action(const std::string& name, const std::string& args = "{}") {
    server("{\"type\":\"action\",\"id\":\"t" + std::to_string(++ids) + "\",\"name\":\"" + name +
           "\",\"args\":" + args + "}");
    return fake.last("action.result");
  }

  // The `timer` object of status.
  Value timer_status() {
    Value s;
    hg::json::parse(app.status_json(), s);
    return Value(s["timer"]);
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Device actions

TEST("timer: the actions are listed on a board with a display or a speaker, not on one with neither") {
  Rig with;
  with.online();
  const Value* hello = with.fake.last("hello");
  CHECK(hello != nullptr);
  if (hello) {
    bool start = false, status = false, cancel = false;
    for (const Value& a : (*hello)["actions"].elements()) {
      start |= a["name"].as_string() == "timer.start";
      status |= a["name"].as_string() == "timer.status";
      cancel |= a["name"].as_string() == "timer.cancel";
    }
    CHECK(start && status && cancel);
  }

  Rig none;
  none.hal.display = nullptr;
  none.hal.speaker = nullptr;
  none.app.begin();
  none.app.on_network(true, "test-wifi");
  none.advance(1000);
  none.app.on_transport_open();
  const Value* bare = none.fake.last("hello");
  CHECK(bare != nullptr);
  if (bare) {
    for (const Value& a : (*bare)["actions"].elements()) CHECK(a["name"].as_string().rfind("timer.", 0) != 0);
  }
}

TEST("timer: start checks its seconds, and cuts a long label to 40 characters") {
  Rig r;
  r.online();
  const Value* res = r.action("timer.start", "{}");
  CHECK(res != nullptr && !(*res)["ok"].as_bool());
  if (res) CHECK_EQ((*res)["error"].as_string(), std::string("seconds is required"));
  res = r.action("timer.start", R"({"seconds":"ten"})");
  if (res) CHECK_EQ((*res)["error"].as_string(), std::string("seconds is required"));
  res = r.action("timer.start", R"({"seconds":0})");
  if (res) CHECK_EQ((*res)["error"].as_string(), std::string("seconds must be between 1 and 86400"));
  res = r.action("timer.start", R"({"seconds":86401})");
  if (res) CHECK_EQ((*res)["error"].as_string(), std::string("seconds must be between 1 and 86400"));

  // Fifty accented letters (two bytes each) come back as the first forty letters, whole.
  std::string label, forty;
  for (int i = 0; i < 50; ++i) label += "\xC3\xB1";  // ñ
  for (int i = 0; i < 40; ++i) forty += "\xC3\xB1";
  res = r.action("timer.start", R"({"seconds":60,"label":")" + label + R"("})");
  CHECK(res != nullptr && (*res)["ok"].as_bool());
  if (res) {
    CHECK_EQ((*res)["result"]["seconds"].as_int(), int64_t(60));
    CHECK_EQ((*res)["result"]["label"].as_string(), forty);
  }
  res = r.action("timer.start", R"({"seconds":86400})");
  CHECK(res != nullptr && (*res)["ok"].as_bool());
}

TEST("timer: the seconds left reach timer.status, status and the Ready caption, and the arc shrinks") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":5,"label":"Pasta"})");
  CHECK_EQ(r.app.model().detail, std::string("Timer 0:05"));
  CHECK_EQ(r.app.model().timer_permille, int16_t(1000));
  const Value* res = r.action("timer.status");
  if (res) {
    CHECK(!(*res)["result"]["ringing"].as_bool());
    CHECK_EQ((*res)["result"]["remaining_s"].as_int(), int64_t(5));
  }
  CHECK_EQ(r.timer_status()["remaining_s"].as_int(), int64_t(5));
  CHECK(r.timer_status()["running"].as_bool());
  CHECK_EQ(r.timer_status()["label"].as_string(), std::string("Pasta"));

  r.idle(2000);  // exactly two seconds on: 3 s left, and the arc is 60 percent of its length
  CHECK_EQ(r.timer_status()["remaining_s"].as_int(), int64_t(3));
  CHECK(r.app.model().timer_permille >= 600 && r.app.model().timer_permille <= 610);  // the last frame may lag
  CHECK(r.app.model().detail == "Timer 0:03" || r.app.model().detail == "Timer 0:04");
  res = r.action("timer.status");
  if (res) CHECK_EQ((*res)["result"]["remaining_s"].as_int(), int64_t(3));
}

TEST("timer: timer.cancel stops a timer; timer.start replaces one and dismisses a ringing alarm") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":10})");
  r.idle(3000);
  const Value* res = r.action("timer.cancel");
  CHECK(res != nullptr && (*res)["result"]["cancelled"].as_bool());
  res = r.action("timer.cancel");
  CHECK(res != nullptr && !(*res)["result"]["cancelled"].as_bool());
  const size_t before = r.fake.writes;
  r.idle(10000);
  CHECK(timer_done(r.fake) == nullptr);
  CHECK(r.app.screen() != Screen::Alarm);
  CHECK_EQ(r.fake.writes, before);

  // Cancelled while ringing: silence, and the alarm goes.
  r.action("timer.start", R"({"seconds":2})");
  r.idle(2100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(r.fake.writes > before);
  res = r.action("timer.cancel");
  CHECK(res != nullptr && (*res)["result"]["cancelled"].as_bool());
  CHECK(r.app.screen() != Screen::Alarm);
  const size_t silent = r.fake.writes;
  r.idle(5000);
  CHECK_EQ(r.fake.writes, silent);

  // A new timer replaces a running one.
  r.action("timer.start", R"({"seconds":10})");
  r.idle(5000);
  r.action("timer.start", R"({"seconds":20})");
  CHECK_EQ(r.timer_status()["remaining_s"].as_int(), int64_t(20));

  // ...and it dismisses an alarm on screen.
  r.action("timer.start", R"({"seconds":1})");
  r.idle(1100);
  CHECK(r.app.screen() == Screen::Alarm);
  r.action("timer.start", R"({"seconds":30})");
  CHECK(r.app.screen() != Screen::Alarm);
  CHECK(r.timer_status()["running"].as_bool());
}

// ---------------------------------------------------------------------------
// Expiry, the alarm and its beeps

TEST("timer: at zero it sends timer.done, shows TIMER DONE, beeps, and a press dismisses it") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":2,"label":"Pasta"})");
  r.fake.sent.clear();
  r.advance(2000);
  const Value* done = timer_done(r.fake);
  CHECK(done != nullptr);
  if (done) {
    CHECK_EQ((*done)["data"]["label"].as_string(), std::string("Pasta"));
    CHECK(!(*done)["notify"].as_bool(true));
  }
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK_EQ(r.app.model().headline, std::string("TIMER DONE"));
  CHECK_EQ(r.app.model().detail, std::string("Pasta"));
  CHECK_EQ(r.fake.writes, size_t(1));
  CHECK_EQ(r.fake.samples, size_t(5120));  // two 120 ms beeps and an 80 ms gap at 16 kHz
  CHECK(r.timer_status()["done"].as_bool());
  CHECK(r.timer_status()["ringing"].as_bool());

  r.advance(110);  // the first half of the cycle is green
  CHECK(r.app.model().flash);
  r.advance(700);  // then dark until the next double beep
  CHECK(!r.app.model().flash);
  r.advance(690);  // 1.5 s after the first beep
  CHECK_EQ(r.fake.writes, size_t(2));

  r.fake.sent.clear();
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() != Screen::Alarm);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.start") == nullptr);
  CHECK(!r.fake.mic_on);
  const size_t after = r.fake.writes;
  r.idle(3000);
  CHECK_EQ(r.fake.writes, after);
  CHECK(!r.timer_status()["done"].as_bool());
}

TEST("timer: the alarm beeps for a minute, then stays up quietly until dismissed") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":1})");
  r.idle(1000);
  r.idle(60000);
  CHECK_EQ(r.fake.writes, size_t(40));  // beeps at 0, 1.5 s, ... 58.5 s
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(!r.timer_status()["ringing"].as_bool());
  CHECK(r.timer_status()["done"].as_bool());
  r.idle(5000);
  CHECK_EQ(r.fake.writes, size_t(40));
  CHECK(r.app.screen() == Screen::Alarm);
}

TEST("timer: the alarm beeps when a send moves the clock past the moment it started") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":2})");
  r.fake.send_ms = 50;  // timer.done takes 50 ms to send, after the expiry was read
  r.advance(2000);
  CHECK(r.app.screen() == Screen::Alarm);
  r.advance(20);
  CHECK(r.fake.writes >= 1);
  CHECK(r.timer_status()["ringing"].as_bool());
}

TEST("timer: expiry while recording waits until the recording ends") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":2})");
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.mic_on);
  r.idle(2500);
  CHECK(r.app.screen() == Screen::Listening);
  CHECK_EQ(r.fake.writes, size_t(0));
  r.app.on_button(hg::Button::Talk, false);
  r.advance(100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(r.fake.writes >= 1);
}

TEST("timer: a question from Hermes delays the alarm until it is answered") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":2})");
  r.server(R"({"type":"prompt","id":"q1","title":"Go on?","text":"Keep the timer?"})");
  r.idle(3000);
  CHECK(r.app.screen() == Screen::Prompt);
  CHECK_EQ(r.fake.writes, size_t(0));
  r.server(R"({"type":"prompt.close","id":"q1"})");
  r.advance(100);
  CHECK(r.app.screen() == Screen::Alarm);
}

TEST("timer: a question that arrives while the alarm rings shows first, and the alarm comes back after it") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":1})");
  r.idle(1100);
  CHECK(r.app.screen() == Screen::Alarm);
  r.server(R"({"type":"prompt","id":"q1","title":"Go on?","text":"Keep going?","ttl_s":30})");
  CHECK(r.app.screen() == Screen::Prompt);
  CHECK(!r.timer_status()["ringing"].as_bool());
  CHECK(r.timer_status()["done"].as_bool());
  const size_t beeps = r.fake.writes;
  r.idle(3000);
  CHECK_EQ(r.fake.writes, beeps);  // no beeps over the question
  r.app.on_button(hg::Button::Talk, true);  // the press answers the question, not the alarm
  r.app.on_button(hg::Button::Talk, false);
  const Value* reply = r.fake.last("prompt.reply");
  CHECK(reply != nullptr);
  if (reply) CHECK_EQ((*reply)["answer"].as_string(), std::string("yes"));
  r.advance(100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(r.timer_status()["ringing"].as_bool());
  CHECK(r.fake.writes > beeps);
}

TEST("timer: settings do not open over the alarm") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":1})");
  r.idle(1100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(!r.app.open_settings());  // a hold that began before the alarm completes now
  CHECK(r.app.screen() == Screen::Alarm);
  const size_t beeps = r.fake.writes;
  r.advance(1600);
  CHECK(r.timer_status()["ringing"].as_bool());
  CHECK(r.fake.writes > beeps);
}

TEST("timer: an alarm shows over a dropped connection and rings") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":5})");
  r.app.on_transport_closed("bye");
  r.advance(5100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(r.fake.writes >= 1);
}

TEST("timer: Hermes speech stops the ringing, and the alarm stays on screen") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":1})");
  r.idle(1100);
  CHECK(r.app.screen() == Screen::Alarm);
  const size_t beeps = r.fake.writes;
  r.server(R"({"type":"audio.start","stream":5,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK(!r.timer_status()["ringing"].as_bool());
  r.idle(3000);
  CHECK_EQ(r.fake.writes, beeps);
  CHECK(r.app.screen() == Screen::Alarm);
}

TEST("timer: an alarm wakes an asleep screen, and a quiet alarm lets it sleep again") {
  Rig r;
  r.fake.kv["screen_timeout"] = "60";
  r.online();
  r.idle(61000);
  CHECK_EQ(r.fake.brightness, 0);  // asleep after the 60 s timeout
  r.action("timer.start", R"({"seconds":2})");
  r.idle(2100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK_EQ(r.fake.brightness, 100);  // woken by the alarm
  r.idle(58500);
  CHECK_EQ(r.fake.brightness, 100);  // still ringing: lit
  r.idle(1500);
  CHECK_EQ(r.fake.brightness, 100);  // quiet now, but the last beep was activity: lit
  r.idle(60000);
  CHECK_EQ(r.fake.brightness, 0);  // asleep one timeout after the last beep
  CHECK(r.app.screen() == Screen::Alarm);

  // The first press on the asleep screen only wakes it; the next press dismisses the alarm.
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == Screen::Alarm);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == Screen::Alarm);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() != Screen::Alarm);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("timer: a board without a speaker shows the countdown and the alarm, and writes no samples") {
  Rig r;
  r.hal.speaker = nullptr;
  r.online();
  r.action("timer.start", R"({"seconds":1,"label":"Tea"})");
  r.idle(1100);
  CHECK(r.app.screen() == Screen::Alarm);
  CHECK_EQ(r.fake.writes, size_t(0));
  CHECK_EQ(r.fake.samples, size_t(0));
  CHECK_EQ(r.app.model().detail, std::string("Tea"));
}

// ---------------------------------------------------------------------------
// Pixels

TEST("timer: round panel: the arc shows the time left clockwise from 12 o'clock, and nothing without a timer") {
  Rig r;
  r.fake.panel(466, 466, true);
  r.online();
  r.idle(200);
  // The band of sim-466x466-round: radii 219 (exclusive) to 227 (inclusive) from (233, 233).
  CHECK_EQ(r.fake.at(456, 233), kBg);  // the right side, no timer: the band is background
  CHECK_EQ(r.fake.at(10, 233), kBg);

  r.action("timer.start", R"({"seconds":60})");
  r.idle(200);
  CHECK_EQ(r.fake.at(456, 233), kAccent);  // 3 o'clock: the whole ring still to go
  CHECK_EQ(r.fake.at(10, 233), kAccent);

  r.idle(29800);  // half gone: the right half (12 to 6 o'clock) remains; the last frame may lag a little
  CHECK(r.app.model().timer_permille >= 495 && r.app.model().timer_permille <= 505);
  CHECK_EQ(r.fake.at(456, 233), kAccent);     // 3 o'clock: still to go
  CHECK_EQ(r.fake.at(10, 233), kAccentDim);   // 9 o'clock: gone
  CHECK_EQ(r.fake.at(233, 10), kAccent);      // 12 o'clock: where the arc starts
}

TEST("timer: round panel: the arc stays in the corner wedge where the UI square meets the glass") {
  Rig r;
  r.fake.panel(466, 466, true);
  r.online();
  r.action("timer.start", R"({"seconds":60})");
  r.idle(2100);  // the caption changes every second, so the bands redraw and flush their rows
  CHECK_EQ(r.app.model().detail, std::string("Timer 0:58"));
  CHECK_EQ(r.fake.at(391, 75), kAccent);   // 45 degrees, top right, in the top bar
  CHECK_EQ(r.fake.at(391, 391), kAccent);  // 135 degrees, bottom right, in the bottom bar
}

TEST("timer: round panel: the battery strip starts below the arc, and the top of the arc stays amber") {
  Rig r;
  r.fake.panel(466, 466, true);
  r.fake.kv["battery_display"] = "icon";
  r.online();
  r.action("timer.start", R"({"seconds":60})");
  r.idle(300);
  CHECK_EQ(r.fake.at(233, 10), kAccent);  // the band's top, above the strip
  CHECK_EQ(r.fake.at(233, 13), kAccent);
  int first = -1;
  for (int y = 0; y < 68 && first < 0; ++y)
    for (int x = 200; x < 266 && first < 0; ++x)
      if (r.fake.at(x, y) == kDim || r.fake.at(x, y) == kText) first = y;
  CHECK(first >= 14);  // the band's inner edge is row 14 at the centre
  CHECK(first >= 0);
}

TEST("timer: rectangular panel: the top-bar line's amber part shrinks with the time") {
  Rig r;
  r.fake.panel(320, 240, false);
  r.online();
  r.action("timer.start", R"({"seconds":60})");
  r.idle(100);
  // The line sits in the bar's bottom rows: 3 px tall at 320x240, rows 19 to 21.
  CHECK_EQ(r.fake.at(250, 20), kAccent);
  r.idle(29900);  // half gone; the last frame may lag a little
  CHECK(r.app.model().timer_permille >= 495 && r.app.model().timer_permille <= 505);
  CHECK_EQ(r.fake.at(100, 20), kAccent);     // the left half still to go
  CHECK_EQ(r.fake.at(250, 20), kAccentDim);  // the right half gone
}

TEST("timer: the alarm is green across the glass on a flash, and its Spanish text stays inside the panel") {
  Rig r;
  r.fake.panel(466, 466, true);
  r.online();
  r.action("timer.start", R"({"seconds":2,"label":"Pasta"})");
  r.idle(2000);
  r.advance(110);
  CHECK(r.app.model().flash);
  CHECK_EQ(r.fake.at(2, 2), kGreen);  // outside the circle: green too
  CHECK_EQ(r.fake.at(233, 2), kGreen);
  r.advance(700);
  CHECK(!r.app.model().flash);
  CHECK_EQ(r.fake.at(2, 2), kBg);
}

TEST("timer: Spanish alarm text fits the smallest round and rectangular panels") {
  // 240 round: every drawn ink pixel is inside the glass.
  {
    Rig r;
    r.fake.kv["language"] = "es";
    r.fake.panel(240, 240, true);
    r.online();
    r.action("timer.start", R"({"seconds":2,"label":"Pasta"})");
    r.idle(2000);
    r.advance(110);
    CHECK(r.app.model().flash);
    int ink = 0, outside = 0;
    for (int y = 0; y < 240; ++y)
      for (int x = 0; x < 240; ++x)
        if (r.fake.at(x, y) == kAlarmInk) {
          ++ink;
          const int dx = x - 120, dy = y - 120;
          if (dx * dx + dy * dy > 118 * 118) ++outside;
        }
    CHECK(ink > 0);
    CHECK_EQ(outside, 0);
    CHECK_EQ(r.app.model().headline, std::string("TIEMPO AGOTADO"));
  }
  // 128 x 128 rectangular: the text keeps off the edge columns.
  {
    Rig r(false);
    r.fake.kv["language"] = "es";
    r.fake.panel(128, 128, false);
    r.online();
    r.action("timer.start", R"({"seconds":2,"label":"Pasta"})");
    r.idle(2000);
    r.advance(110);
    CHECK(r.app.model().flash);
    int ink = 0, edge = 0;
    for (int y = 0; y < 128; ++y)
      for (int x = 0; x < 128; ++x)
        if (r.fake.at(x, y) == kAlarmInk) {
          ++ink;
          if (x < 2 || x > 125) ++edge;
        }
    CHECK(ink > 0);
    CHECK_EQ(edge, 0);
  }
}

TEST("timer: the alarm's label stays clear of the hint at the bottom") {
  for (int size : {240, 466}) {
    for (const char* lang : {"en", "es"}) {
      auto render = [&](const std::string& label) {
        Rig r;
        r.fake.kv["language"] = lang;
        r.fake.panel(size, size, true);
        r.online();
        r.action("timer.start", "{\"seconds\":2,\"label\":\"" + label + "\"}");
        r.idle(2000);
        r.advance(110);  // the green half of the first cycle
        CHECK(r.app.model().flash);
        return r.fake.fb;
      };
      const std::vector<uint16_t> plain = render(""), labelled = render("Pasta");
      auto inked = [&](const std::vector<uint16_t>& fb, int y) {
        for (int x = 0; x < size; ++x)
          if (fb[static_cast<size_t>(y * size + x)] == kAlarmInk) return true;
        return false;
      };
      // The hint: the lowest block of rows with ink when there is no label.
      int bottom = size - 1;
      while (bottom > 0 && !inked(plain, bottom)) --bottom;
      int top = bottom;
      while (top > 0 && inked(plain, top - 1)) --top;
      CHECK(top < bottom);
      bool same = true;
      for (int y = top; y <= bottom; ++y)
        for (int x = 0; x < size; ++x)
          if (plain[static_cast<size_t>(y * size + x)] != labelled[static_cast<size_t>(y * size + x)]) same = false;
      CHECK(same);  // the label drew nothing into the hint's rows
    }
  }
}

namespace {
// A face message (docs/protocol.md#faces) for the default profile, every bitmap byte solid ink.
std::string solid_face(const std::string& hash) {
  std::vector<uint8_t> data(3 * (512 + 1152 + 2592 + 4608), 0xff);
  for (int v : {607, 293, 262, 620, 801, 107, 396, 508, 586, 649}) {
    data.push_back(static_cast<uint8_t>(v & 0xff));
    data.push_back(static_cast<uint8_t>(v >> 8));
  }
  return R"({"type":"face","profile":"default","hash":")" + hash + R"(","bits":")" +
         hg::crypto::base64_encode(data.data(), data.size()) + R"("})";
}
size_t pixels_changed(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
  return n;
}
}  // namespace

TEST("timer: a face that arrives or goes while the alarm is up quietly redraws it") {
  Rig r;
  r.online();
  r.action("timer.start", R"({"seconds":1})");
  r.idle(1000);
  r.idle(61000);  // the beeping, and the flashing with it, is over
  CHECK(r.app.screen() == Screen::Alarm && !r.timer_status()["ringing"].as_bool());
  const std::vector<uint16_t> mascot = r.fake.fb;
  r.server(solid_face("a1"));
  r.idle(1000);
  CHECK(pixels_changed(mascot, r.fake.fb) > 1000);  // the face, a solid square, where the mascot was
  r.server(R"({"type":"face","profile":"default"})");
  r.idle(1000);
  CHECK_EQ(pixels_changed(mascot, r.fake.fb), size_t(0));  // the mascot again
}
