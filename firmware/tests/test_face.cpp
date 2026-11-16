// The face's expression: the face.set action and the console command, how long an
// expression lasts, and what the renderer draws and flushes for each one.
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/app.hpp"
#include "hg/canvas.hpp"
#include "hg/mascot.hpp"
#include "hg/ui.hpp"

using hg::Screen;
using hg::json::Value;
using hg::mascot::Expression;

namespace {

// A panel, 320x240 unless said otherwise, that counts the rows flushed to it.
struct Panel : hg::Display {
  static constexpr int kW = 320, kH = 240;
  int w, h;
  bool round;
  std::vector<uint16_t> fb;
  int flushes = 0, rows = 0;
  explicit Panel(int width = kW, int height = kH, bool round_panel = false)
      : w(width), h(height), round(round_panel), fb(static_cast<size_t>(width * height), 0) {}
  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = static_cast<uint16_t>(w);
    d.height = static_cast<uint16_t>(h);
    d.round = round;
    return d;
  }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t y0, uint16_t y1) override {
    ++flushes;
    rows += y1 - y0;
  }
  void reset() { flushes = rows = 0; }
};

// The idle screen's hero model at a frame where the eyes are open and nothing else moves.
hg::UiModel hero(Screen screen, Expression e, uint32_t frame = 10) {
  hg::UiModel m;
  m.screen = screen;
  m.hero = true;
  m.link = hg::Link::Online;
  m.title = "Desk";
  m.headline = "Hi, I'm Hermes";
  m.detail = "Ask me anything";
  m.hint = "Hold TALK";
  m.frame = frame;
  m.expression = e;
  return m;
}

// The pixels a fresh Ui draws for `m`: what a whole repaint shows.
std::vector<uint16_t> fresh(const hg::UiModel& m, int w = Panel::kW, int h = Panel::kH, bool round = false) {
  Panel p(w, h, round);
  hg::Ui ui(p);
  ui.render(m);
  return p.fb;
}

// The rows where two pictures differ, as [lo, hi); lo == hi when they are the same.
void differing_rows(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, int& lo, int& hi) {
  lo = Panel::kH;
  hi = 0;
  for (int y = 0; y < Panel::kH; ++y) {
    if (std::equal(a.begin() + y * Panel::kW, a.begin() + (y + 1) * Panel::kW, b.begin() + y * Panel::kW)) continue;
    lo = std::min(lo, y);
    hi = y + 1;
  }
  if (hi == 0) lo = 0;
}

struct FaceHal : Panel, hg::AudioIn, hg::Transport, hg::Storage, hg::System {
  bool start(uint32_t) override { return true; }
  void stop() override {}

  uint32_t clock = 1000;
  uint32_t now_ms() override { return clock; }
  void random_bytes(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i);
  }
  void log(hg::LogLevel, std::string_view) override {}

  std::vector<Value> sent;
  bool send_text(std::string_view text) override {
    Value v;
    if (hg::json::parse(text, v)) sent.push_back(v);
    return true;
  }
  bool send_binary(const uint8_t*, size_t) override { return true; }
  void connect(const std::string&, const std::string&) override {}
  void close() override {}

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

  const Value* last(const std::string& type) const {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      if ((*it)["type"].as_string() == type) return &*it;
    return nullptr;
  }
};

struct Rig {
  FaceHal fake;
  hg::Hal hal;
  hg::App app;
  int ids = 0;

  explicit Rig(bool display = true) : hal(make(fake, display)), app(hal, profile()) {}

  static hg::Hal make(FaceHal& f, bool display) {
    hg::Hal h;
    h.system = &f;
    h.transport = &f;
    h.storage = &f;
    h.mic = &f;
    if (display) h.display = &f;
    return h;
  }
  static hg::DeviceProfile profile() {
    hg::DeviceProfile p;
    p.board = "test-board";
    p.firmware = "1.2.3";
    p.default_server_url = "ws://hermes.local:8765/gadget";
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
  void connect() {
    app.begin();
    app.on_network(true, "test-wifi");
    advance(1000);
    app.on_transport_open();
  }
  void online() {
    connect();
    server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
    server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true})");
  }
  const Value* action(const std::string& args) {
    server("{\"type\":\"action\",\"id\":\"f" + std::to_string(++ids) + "\",\"name\":\"face.set\",\"args\":" + args + "}");
    return fake.last("action.result");
  }
  bool lists_face_set() const {
    const Value* hello = fake.last("hello");
    if (!hello) return false;
    for (const Value& a : (*hello)["actions"].elements())
      if (a["name"].as_string() == "face.set") return true;
    return false;
  }
  std::string status_expression() {
    Value s;
    hg::json::parse(app.status_json(), s);
    return std::string(s["expression"].str_or(""));
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// The action

TEST("face: face.set is listed on a board with a display, and not on one without") {
  Rig with;
  with.online();
  CHECK(with.lists_face_set());

  Rig none(false);
  none.connect();
  CHECK(none.fake.last("hello") != nullptr);
  CHECK(!none.lists_face_set());
}

TEST("face: face.set checks its expression and its seconds") {
  Rig r;
  r.online();
  const Value* res = r.action("{}");
  CHECK(res != nullptr && !(*res)["ok"].as_bool());
  if (res) CHECK_EQ((*res)["error"].as_string(), std::string("expression must be one of: neutral, happy, confused, alert, sleepy"));
  res = r.action(R"({"expression":"angry"})");
  CHECK(res != nullptr && !(*res)["ok"].as_bool());
  for (const char* bad : {R"({"expression":"happy","seconds":0})", R"({"expression":"happy","seconds":3601})",
                          R"({"expression":"happy","seconds":"ten"})", R"({"expression":"happy","seconds":2.9})",
                          R"({"expression":"happy","seconds":true})", R"({"expression":"happy","seconds":1e300})",
                          R"({"expression":"happy","seconds":-1e300})"}) {
    res = r.action(bad);
    CHECK(res != nullptr && !(*res)["ok"].as_bool());
    if (res) CHECK_EQ((*res)["error"].as_string(), std::string("seconds must be between 1 and 3600"));
  }
  CHECK(r.app.model().expression == Expression::Neutral);  // a refused call changes nothing

  res = r.action(R"({"expression":"happy","seconds":3600})");
  CHECK(res != nullptr && (*res)["ok"].as_bool());
  if (res) {
    CHECK_EQ((*res)["result"]["expression"].as_string(), std::string("happy"));
    CHECK_EQ((*res)["result"]["seconds"].as_int(), int64_t(3600));
  }
}

TEST("face: an expression reaches the screen and status, and neutral clears it") {
  Rig r;
  r.online();
  CHECK_EQ(r.status_expression(), std::string());
  for (Expression e : {Expression::Happy, Expression::Confused, Expression::Alert, Expression::Sleepy}) {
    const std::string name = hg::mascot::expression_name(e);
    const Value* res = r.action("{\"expression\":\"" + name + "\"}");
    CHECK(res != nullptr && (*res)["ok"].as_bool());
    CHECK(r.app.screen() == Screen::Ready);
    CHECK(r.app.model().expression == e);
    CHECK_EQ(r.status_expression(), name);
  }
  r.idle(30000);  // with no seconds it stays
  CHECK(r.app.model().expression == Expression::Sleepy);
  const Value* res = r.action(R"({"expression":"neutral"})");
  CHECK(res != nullptr && (*res)["ok"].as_bool());
  CHECK(r.app.model().expression == Expression::Neutral);
  CHECK_EQ(r.status_expression(), std::string());
}

TEST("face: an expression with seconds goes back to neutral when they are over") {
  Rig r;
  r.online();
  r.action(R"({"expression":"alert","seconds":3})");
  r.idle(2000);
  CHECK(r.app.model().expression == Expression::Alert);
  r.idle(1500);
  CHECK(r.app.model().expression == Expression::Neutral);
  CHECK_EQ(r.status_expression(), std::string());
}

TEST("face: an expression set during a turn outlasts its reply, and the user's next turn ends it") {
  Rig r;
  r.online();
  CHECK(r.app.console("say hello") == "@ok say");
  CHECK(r.app.screen() == Screen::Thinking);
  r.action(R"({"expression":"happy"})");
  CHECK(r.app.model().expression == Expression::Happy);
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.idle(1000);
  CHECK(r.app.screen() == Screen::Ready);
  CHECK(r.app.model().expression == Expression::Happy);

  r.app.console("say and another thing");  // a typed message: the next turn
  CHECK(r.app.screen() == Screen::Thinking);
  CHECK(r.app.model().expression == Expression::Neutral);
}

TEST("face: a recording ends an expression, in any mode") {
  Rig r;
  r.online();
  r.action(R"({"expression":"happy"})");
  r.app.console("talk");  // from idle
  CHECK(r.app.screen() == Screen::Listening);
  CHECK(r.app.model().expression == Expression::Neutral);
  r.app.console("cancel");
  r.app.console("release");
  r.idle(1000);

  // Talking over a reply: the expression set in that turn goes with the new recording.
  r.app.console("say hello");
  r.action(R"({"expression":"confused"})");
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.server(R"({"type":"reply","turn":"t1","text":"Here is the answer.","final":true})");
  CHECK(r.app.screen() == Screen::Responding);
  CHECK(r.app.model().expression == Expression::Confused);
  r.app.console("talk");
  CHECK(r.app.screen() == Screen::Listening);
  CHECK(r.app.model().expression == Expression::Neutral);
}

TEST("face: a message typed while a reply shows ends an expression, and so does a new session") {
  Rig r;
  r.online();
  r.app.console("say hello");
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.server(R"({"type":"reply","turn":"t1","text":"Here is the answer.","final":true})");
  r.action(R"({"expression":"happy"})");
  CHECK(r.app.screen() == Screen::Responding);
  r.app.console("say and another thing");  // Responding to Thinking: not from idle
  CHECK(r.app.model().expression == Expression::Neutral);
  r.server(R"({"type":"turn.end","turn":"t1","outcome":"success"})");
  r.idle(1000);

  r.action(R"({"expression":"alert"})");
  CHECK(r.app.model().expression == Expression::Alert);
  r.app.console("new-session");
  CHECK(r.app.model().expression == Expression::Neutral);
}

TEST("face: a turn Hermes starts by itself leaves the expression") {
  Rig r;
  r.online();
  r.action(R"({"expression":"sleepy"})");
  r.server(R"({"type":"turn.start","turn":"e1"})");  // as for a device event that notifies Hermes
  CHECK(r.app.screen() == Screen::Thinking);
  CHECK(r.app.model().expression == Expression::Sleepy);
  r.server(R"({"type":"turn.end","turn":"e1","outcome":"success"})");
  r.idle(1000);
  CHECK(r.app.screen() == Screen::Ready);
  CHECK(r.app.model().expression == Expression::Sleepy);
}

TEST("face: a typed message that cannot be sent leaves the expression") {
  Rig r;
  r.online();
  r.action(R"({"expression":"happy"})");
  r.app.on_transport_closed("lost");
  r.app.console("say hello");
  CHECK_EQ(r.app.console("face"), std::string(R"(@face {"expression":"happy"})"));
}

TEST("face: a new face.set replaces the time of the one before") {
  Rig r;
  r.online();
  r.action(R"({"expression":"happy","seconds":2})");
  r.idle(1000);
  r.action(R"({"expression":"alert"})");  // no seconds: the first one's two must not end it
  r.idle(3000);
  CHECK(r.app.model().expression == Expression::Alert);

  r.action(R"({"expression":"happy","seconds":2})");
  r.idle(1000);
  r.action(R"({"expression":"confused","seconds":10})");
  r.idle(3000);
  CHECK(r.app.model().expression == Expression::Confused);
  r.idle(7500);
  CHECK(r.app.model().expression == Expression::Neutral);
}

TEST("face: an expression's seconds run across the clock's wrap") {
  Rig r;
  r.fake.clock = 0xFFFFFFFFu - 20000;  // 20 s before the 32-bit millisecond clock wraps
  r.online();
  r.idle(15000);
  r.action(R"({"expression":"alert","seconds":10})");  // ends after the wrap
  r.idle(2000);
  CHECK(r.app.model().expression == Expression::Alert);
  r.idle(6000);  // the clock has wrapped
  CHECK(r.fake.clock < 0x80000000u);
  CHECK(r.app.model().expression == Expression::Alert);
  r.idle(2500);
  CHECK(r.app.model().expression == Expression::Neutral);
}

TEST("face: the mark moves for a minute, then stands still") {
  Rig r;
  r.online();
  r.action(R"({"expression":"happy"})");
  CHECK(!r.app.model().expression_still);
  r.idle(59000);
  CHECK(!r.app.model().expression_still);
  r.idle(1500);
  CHECK(r.app.model().expression_still);
  CHECK(r.app.model().expression == Expression::Happy);
  r.action(R"({"expression":"happy"})");  // set again: it moves again
  CHECK(!r.app.model().expression_still);
}

TEST("face: the mascot bench draws the same frames whatever expression is set") {
  auto bench = [](const char* expression) {
    Rig r;
    r.online();
    if (expression) r.app.console(std::string("face ") + expression);
    r.app.console("perf bench mascot");
    r.advance(300);  // a few of its frames
    return r.fake.fb;
  };
  const std::vector<uint16_t> plain = bench(nullptr);
  CHECK(bench("alert") == plain);
  CHECK(bench("sleepy") == plain);
}

TEST("face: the console sets, reports and refuses expressions") {
  Rig r;
  r.online();
  CHECK_EQ(r.app.console("face"), std::string(R"(@face {"expression":"neutral"})"));
  CHECK_EQ(r.app.console("face confused 2"), std::string("@ok face confused"));
  CHECK_EQ(r.app.console("face"), std::string(R"(@face {"expression":"confused"})"));
  CHECK(r.app.model().expression == Expression::Confused);
  r.idle(2500);
  CHECK(r.app.model().expression == Expression::Neutral);
  CHECK_EQ(r.app.console("face grumpy"), std::string("@error expression must be one of: neutral, happy, confused, alert, sleepy"));
  CHECK_EQ(r.app.console("face happy soon"), std::string("@error usage: face [<expression> [seconds]]"));
  CHECK_EQ(r.app.console("face happy 0"), std::string("@error seconds must be between 1 and 3600"));
  CHECK(r.app.console("help").find("face [<expression> [seconds]]") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The renderer

TEST("face: every expression draws something of its own on the idle screen") {
  const std::vector<uint16_t> neutral = fresh(hero(Screen::Ready, Expression::Neutral));
  std::vector<std::vector<uint16_t>> seen = {neutral};
  for (Expression e : {Expression::Happy, Expression::Confused, Expression::Alert, Expression::Sleepy}) {
    const std::vector<uint16_t> fb = fresh(hero(Screen::Ready, e));
    for (const auto& other : seen) CHECK(fb != other);
    seen.push_back(fb);
  }
}

TEST("face: sleepy shuts the eyes and alert never blinks") {
  // Frame 10 has open eyes, frame 0 is a blink. Past the mark's rows, sleepy at 10 is a blink, and
  // alert during the blink differs from neutral there (open eyes, and its open mouth).
  const std::vector<uint16_t> open = fresh(hero(Screen::Ready, Expression::Neutral, 10));
  const std::vector<uint16_t> shut = fresh(hero(Screen::Ready, Expression::Neutral, 0));
  int eyes_lo, eyes_hi;
  differing_rows(open, shut, eyes_lo, eyes_hi);
  CHECK(eyes_hi > eyes_lo);
  const std::vector<uint16_t> sleepy = fresh(hero(Screen::Ready, Expression::Sleepy, 10));
  const std::vector<uint16_t> alert = fresh(hero(Screen::Ready, Expression::Alert, 0));
  auto rows = [&](const std::vector<uint16_t>& fb) {
    return std::vector<uint16_t>(fb.begin() + eyes_lo * Panel::kW, fb.begin() + eyes_hi * Panel::kW);
  };
  CHECK(rows(sleepy) == rows(shut));
  CHECK(rows(alert) == rows(open));
}

TEST("face: sleepy shuts the eyes while thinking and under a reply too, and alert keeps them open") {
  // Frames 0 (a blink) and 8 (open eyes) show the same thinking dots.
  for (Screen s : {Screen::Thinking, Screen::Responding}) {
    const std::vector<uint16_t> open = fresh(hero(s, Expression::Neutral, 8));
    const std::vector<uint16_t> shut = fresh(hero(s, Expression::Neutral, 0));
    int lo, hi;
    differing_rows(open, shut, lo, hi);
    CHECK(hi > lo);
    auto rows = [&](const std::vector<uint16_t>& fb) {
      return std::vector<uint16_t>(fb.begin() + lo * Panel::kW, fb.begin() + hi * Panel::kW);
    };
    CHECK(rows(fresh(hero(s, Expression::Sleepy, 8))) == rows(shut));
    CHECK(rows(fresh(hero(s, Expression::Alert, 0))) == rows(open));
  }
}

TEST("face: a change of expression redraws only its rows, and leaves what a whole repaint would") {
  Panel p;
  hg::Ui ui(p);
  ui.render(hero(Screen::Ready, Expression::Neutral));
  const Expression steps[] = {Expression::Happy, Expression::Alert, Expression::Sleepy, Expression::Confused,
                              Expression::Neutral, Expression::Alert, Expression::Neutral};
  for (Expression e : steps) {
    const hg::UiModel m = hero(Screen::Ready, e);
    p.reset();
    ui.render(m);
    CHECK(p.flushes > 0);
    CHECK(p.rows < Panel::kH / 2);  // the mark, the eyes and the mouth: never the whole hero band
    CHECK(p.fb == fresh(m));        // nothing of the expression before is left behind
  }
}

TEST("face: a mark's step flushes only the mark's rows, and stands still with animation off") {
  Panel p;
  hg::Ui ui(p);
  hg::UiModel m = hero(Screen::Ready, Expression::Happy, 10);
  ui.render(m);
  p.reset();
  m.frame = 14;  // the same step
  ui.render(m);
  CHECK_EQ(p.flushes, 0);
  m.frame = 15;  // the next step
  ui.render(m);
  CHECK(p.flushes > 0);
  CHECK(p.rows < Panel::kH / 2);
  CHECK(p.fb == fresh(m));

  m.animate = false;
  ui.render(m);
  p.reset();
  m.frame = 20;
  ui.render(m);
  CHECK_EQ(p.flushes, 0);

  // Moving again, then stopped by the app after its minute: no more steps.
  m.animate = true;
  ui.render(m);
  m.expression_still = true;
  ui.render(m);
  CHECK(p.fb == fresh(m));
  p.reset();
  for (uint32_t f = 21; f < 44; ++f) {  // up to the next blink
    m.frame = f;
    ui.render(m);
  }
  CHECK_EQ(p.flushes, 0);
}

TEST("face: a step of the mark flushes the mark's rows alone, for alert as for the others") {
  // The mark's box is 8 rows per text scale, 4 at this face's 144 px, and a flush adds 3 rows of margin.
  // The eyes and alert's open mouth lie below it and have not changed.
  const int mark_rows = 8 * 4 + 3;
  for (Expression e : {Expression::Happy, Expression::Confused, Expression::Alert, Expression::Sleepy}) {
    Panel p;
    hg::Ui ui(p);
    hg::UiModel m = hero(Screen::Ready, e, 10);
    ui.render(m);
    for (uint32_t f : {15u, 20u, 25u}) {
      p.reset();
      m.frame = f;
      ui.render(m);
      CHECK_EQ(p.flushes, 1);
      CHECK(p.rows <= mark_rows);
      CHECK(p.fb == fresh(m));
    }
  }
}

TEST("face: a blink under an expression still repaints the eyes") {
  Panel p;
  hg::Ui ui(p);
  hg::UiModel m = hero(Screen::Ready, Expression::Happy, 44);
  ui.render(m);
  for (uint32_t f : {45u, 46u, 47u, 50u}) {  // the eyes shut for two frames, open, then the mark steps
    m.frame = f;
    ui.render(m);
    CHECK(p.fb == fresh(m));
  }
}

TEST("face: the mark stays whole on small, narrow and round panels, wherever the face puts its anchor") {
  // A still colour face whose thinking-dots anchor is in a corner: the mark must come back onto
  // the panel and under the top bar, with every pixel a mark in the middle of the face has.
  const int px = 96;
  std::vector<uint16_t> picture(static_cast<size_t>(px * px), hg::rgb565(60, 60, 200));
  // The pixels the widest mark, the sparkles, changes: the same number wherever it is drawn whole.
  auto changed = [&](int w, int h, bool round, int16_t ax, int16_t ay) {
    hg::mascot::Face face;
    face.hash = "corner";
    face.rgb565 = picture.data();
    face.px = px;
    face.anchors = hg::mascot::anchors();
    face.anchors.head_top_right = {ax, ay};
    hg::UiModel m = hero(Screen::Ready, Expression::Happy);
    m.face = &face;
    const std::vector<uint16_t> fb = fresh(m, w, h, round);
    m.expression = Expression::Neutral;
    const std::vector<uint16_t> plain = fresh(m, w, h, round);
    size_t n = 0;
    for (size_t i = 0; i < fb.size(); ++i) n += fb[i] != plain[i];
    return n;
  };
  struct Size {
    int w, h;
    bool round;
  };
  for (const Size& s : {Size{320, 240, false}, Size{240, 135, false}, Size{240, 320, false}, Size{128, 128, false},
                        Size{240, 240, true}, Size{466, 466, true}}) {
    const auto whole = changed(s.w, s.h, s.round, 500, 500);
    CHECK(whole > 0);
    CHECK_EQ(changed(s.w, s.h, s.round, 1000, 0), whole);     // top right corner
    CHECK_EQ(changed(s.w, s.h, s.round, 1000, 1000), whole);  // bottom right
    CHECK_EQ(changed(s.w, s.h, s.round, 0, 0), whole);        // top left
  }
}

TEST("face: while thinking the mark takes the dots' place") {
  // The dots step every other frame; with an expression nothing moves between those frames.
  Panel p;
  hg::Ui ui(p);
  hg::UiModel m = hero(Screen::Thinking, Expression::Confused, 10);
  ui.render(m);
  p.reset();
  m.frame = 12;
  ui.render(m);
  CHECK_EQ(p.flushes, 0);
  CHECK(fresh(hero(Screen::Thinking, Expression::Confused)) != fresh(hero(Screen::Thinking, Expression::Neutral)));

  // Back to neutral, the dots return in the same partial redraw.
  m.expression = Expression::Neutral;
  ui.render(m);
  CHECK(p.rows < Panel::kH / 2);
  CHECK(p.fb == fresh(m));
}

TEST("face: screens other than idle, thinking and speaking ignore the expression") {
  for (Screen s : {Screen::Listening, Screen::Connecting, Screen::Pairing, Screen::Card, Screen::Prompt})
    CHECK(fresh(hero(s, Expression::Alert)) == fresh(hero(s, Expression::Neutral)));
  // Speaking: the mouth keeps moving under a sleepy mark.
  hg::UiModel a = hero(Screen::Responding, Expression::Sleepy, 10), b = hero(Screen::Responding, Expression::Sleepy, 12);
  a.speaking = b.speaking = true;
  CHECK(fresh(a) != fresh(b));
  CHECK(fresh(a) != fresh(hero(Screen::Responding, Expression::Neutral, 10)));
}

TEST("face: a still colour face shows the mark and keeps its picture") {
  // A colour face with no blink or talk rows: the expression's frames have nothing to change.
  const int px = 96;
  std::vector<uint16_t> picture(static_cast<size_t>(px * px), hg::rgb565(200, 60, 60));
  hg::mascot::Face face;
  face.hash = "colour";
  face.rgb565 = picture.data();
  face.px = px;
  face.anchors = hg::mascot::anchors();
  auto model = [&](Expression e) {
    hg::UiModel m = hero(Screen::Ready, e);
    m.face = &face;
    return m;
  };
  const std::vector<uint16_t> neutral = fresh(model(Expression::Neutral));
  for (Expression e : {Expression::Sleepy, Expression::Alert}) {
    const std::vector<uint16_t> fb = fresh(model(e));
    int lo, hi;
    differing_rows(neutral, fb, lo, hi);
    CHECK(hi > lo);                 // the mark is there
    CHECK(hi - lo <= 8 * 5 + 1);    // and only the mark: its box is 8 rows per text scale, 5 at most
  }
}
