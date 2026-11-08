// Drives hg::App through a fake HAL the way a Hermes gateway would.
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/app.hpp"
#include "hg/crypto.hpp"
#include "hg/protocol.hpp"
#include "hg/qr.hpp"
#include "hg/setup.hpp"
#include "hg/touch.hpp"

using hg::json::Value;

namespace {

struct FakeHal : hg::Display, hg::AudioIn, hg::AudioOut, hg::Transport, hg::Storage, hg::System {
  // System
  uint32_t clock = 1000;
  uint32_t drift = 0;  // ms the clock moves on at every read, as it does on a device mid-tick
  uint32_t now_ms() override {
    const uint32_t c = clock;
    clock += drift;
    return c;
  }
  hg::WallTime wall;  // what wall_time() reports once wall_ok holds
  bool wall_ok = false;
  bool wall_time(hg::WallTime& out) override {
    if (!wall_ok) return false;
    out = wall;
    return true;
  }
  bool dozing = false;
  int doze_changes = 0;
  void set_dozing(bool d) override {
    dozing = d;
    ++doze_changes;
  }
  void random_bytes(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i);
  }
  std::vector<std::string> logs;
  void log(hg::LogLevel, std::string_view message) override { logs.emplace_back(message); }

  // Transport
  std::string url;
  int connects = 0, closes = 0;
  std::vector<Value> sent;
  std::vector<std::vector<uint8_t>> sent_binary;
  void connect(const std::string& u, const std::string&) override {
    url = u;
    ++connects;
  }
  bool refuse_text = false, refuse_binary = false;  // the send queue is full
  bool send_text(std::string_view text) override {
    if (refuse_text) return false;
    Value v;
    if (hg::json::parse(text, v)) sent.push_back(v);
    return true;
  }
  bool send_binary(const uint8_t* data, size_t len) override {
    if (refuse_binary) return false;
    sent_binary.emplace_back(data, data + len);
    return true;
  }
  void close() override { ++closes; }

  // Storage
  std::map<std::string, std::string> kv;
  bool storage_full = false;  // every set() fails, as a worn or misconfigured flash would
  std::optional<std::string> get(std::string_view key) override {
    auto it = kv.find(std::string(key));
    if (it == kv.end()) return std::nullopt;
    return it->second;
  }
  bool set(std::string_view key, std::string_view value) override {
    if (storage_full) return false;
    if (key.empty() || key.size() > 15) return false;  // NVS refuses longer keys, as the ESP32 port does
    kv[std::string(key)] = std::string(value);
    return true;
  }
  void erase(std::string_view key) override { kv.erase(std::string(key)); }

  // Display
  int width = 320, height = 240;
  bool round = false;
  uint8_t font_scale = 1;
  bool emissive = false;
  bool backlight = false;
  int brightness = 0, volume = 0;
  std::vector<uint16_t> fb = std::vector<uint16_t>(320 * 240, 0);
  int flushes = 0;
  int flushed_rows = 0;
  hg::DisplayInfo info() const override {
    hg::DisplayInfo d;
    d.width = static_cast<uint16_t>(width);
    d.height = static_cast<uint16_t>(height);
    d.round = round;
    d.font_scale = font_scale;
    d.emissive = emissive;
    d.has_backlight = backlight;
    return d;
  }
  void make_round(int diameter) { make_panel(diameter, diameter, true); }
  void make_panel(int w, int h, bool round_panel = false) {
    width = w;
    height = h;
    round = round_panel;
    fb.assign(static_cast<size_t>(w * h), 0);
  }
  uint16_t* framebuffer() override { return fb.data(); }
  // A port with no PSRAM: fb holds `band_rows` panel rows starting at `origin`
  // (0 rows: the whole panel). make_banded comes after make_panel or make_round.
  int band_rows = 0, origin = 0;
  void make_banded(int rows) {
    band_rows = rows;
    fb.assign(static_cast<size_t>(width * rows), 0);
    keep_panel = true;
  }
  int framebuffer_rows() const override { return band_rows; }
  void set_frame_origin(int y) override { origin = y; }
  // With keep_panel, every flush copies its rows into `panel`: what the screen shows.
  bool keep_panel = false;
  std::vector<uint16_t> panel;
  int stray_flushes = 0;  // flushes of rows the framebuffer window does not hold
  bool lit_while_dozing = false;  // the screen was turned on before the port woke
  void set_backlight(uint8_t percent) override {
    brightness = percent;
    if (percent && dozing) lit_while_dozing = true;
  }
  void flush(uint16_t y0, uint16_t y1) override {
    ++flushes;
    flushed_rows += y1 - y0;
    if (!keep_panel) return;
    panel.resize(static_cast<size_t>(width * height), 0);
    const int top = band_rows ? origin : 0, rows = band_rows ? band_rows : height;
    if (y0 < top || y1 > top + rows || y1 > height) {
      ++stray_flushes;
      return;
    }
    std::copy(fb.begin() + static_cast<long>((y0 - top) * width), fb.begin() + static_cast<long>((y1 - top) * width),
              panel.begin() + static_cast<long>(y0 * width));
  }

  // Mic
  bool mic_on = false;
  bool start(uint32_t) override { return mic_on = true; }
  void stop() override { mic_on = false; }

  // Speaker
  bool spk_open = false;
  bool spk_draining = false;  // ended, but the last audio is still playing
  size_t spk_samples = 0;
  bool begin(uint32_t) override { return spk_open = true; }
  bool spoke_while_dozing = false;  // the speaker was written before the port woke
  void write(const int16_t*, size_t n) override {
    spk_samples += n;
    if (dozing) spoke_while_dozing = true;
  }
  void end() override { spk_open = false; }
  void abort() override { spk_open = false; }
  bool busy() const override { return spk_open || spk_draining; }
  void set_volume(uint8_t percent) override { volume = percent; }
  size_t speaker_buffer = 0;  // what buffer_bytes() reports; 0: unknown, as a port without one says
  size_t buffer_bytes() const override { return speaker_buffer; }

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

struct FakeUpdater : hg::Updater {
  size_t slot = 64 * 1024;
  std::vector<uint8_t> image;
  bool open = false, installed = false, restarted = false, pending = false, confirmed = false;
  bool reject_image = false;
  size_t capacity() const override { return slot; }
  bool begin(size_t, std::string&) override {
    image.clear();
    return open = true;
  }
  bool write(const uint8_t* data, size_t len, std::string&) override {
    image.insert(image.end(), data, data + len);
    return open;
  }
  bool finish(std::string& error) override {
    open = false;
    if (reject_image) error = "not an app image";
    return installed = !reject_image;
  }
  void abort() override { open = false; }
  void restart() override { restarted = true; }
  bool pending_verify() const override { return pending; }
  void confirm() override {
    pending = false;
    confirmed = true;
  }
};

struct Rig {
  FakeHal fake;
  hg::Hal hal = fake.hal();
  hg::App app;

  explicit Rig(const std::string& server = "ws://hermes.local:8765/gadget") : app(hal, profile(server)) {}
  explicit Rig(hg::DeviceProfile p) : app(hal, std::move(p)) {}

  // A board whose port feeds App::on_motion: it can sleep face down.
  static hg::DeviceProfile motion_profile() {
    hg::DeviceProfile p = profile("ws://hermes.local:8765/gadget");
    p.motion_sensor = true;
    return p;
  }

  static hg::DeviceProfile touch_profile() {
    hg::DeviceProfile p = profile("ws://hermes.local:8765/gadget");
    p.touch_screen = true;
    p.talk_key = false;  // a touchscreen only: the boards whose screen is their only way to talk
    p.cancel_label = "Swipe down";
    p.extra_settings = {"touch_cancel"};
    return p;
  }

  static hg::DeviceProfile profile(const std::string& server) {
    hg::DeviceProfile p;
    p.board = "test-board";
    p.firmware = "1.2.3";
    p.default_server_url = server;
    return p;
  }

  void advance(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      fake.clock += 10;
      app.tick();
    }
  }

  // Feeds on_motion at 10 Hz while the clock runs, like the port's motion task.
  void hold_still(int16_t x, int16_t y, int16_t z, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) {
      app.on_motion(x, y, z);
      advance(100);
    }
  }

  void server(const std::string& json) { app.on_transport_text(json); }

  // Boot, connect, authenticate and (optionally) get approved.
  void bring_online(bool paired) {
    app.begin();
    app.on_network(true, "test-wifi");
    advance(1000);
    app.on_transport_open();
    server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
    server(std::string(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":)") +
           (paired ? "true" : "false") + "}");
  }

  // Like bring_online, against a server that offers two Hermes profiles.
  void bring_online_with_profiles(bool paired = true, const std::string& current = "default") {
    app.begin();
    app.on_network(true, "test-wifi");
    advance(1000);
    app.on_transport_open();
    server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
    server(std::string(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":)") +
           (paired ? "true" : "false") + R"(,"profile":")" + current +
           R"(","profiles":[{"id":"default","name":"Hermes"},{"id":"ops","name":"Ops"}]})");
  }
};

}  // namespace

TEST("app: connects after boot and sends a hello describing the device") {
  Rig r;
  r.app.begin();
  CHECK(r.app.screen() == hg::Screen::Boot);
  r.app.on_network(true, "wifi");
  r.advance(1000);
  CHECK_EQ(r.fake.connects, 1);
  CHECK_EQ(r.fake.url, std::string("ws://hermes.local:8765/gadget"));
  CHECK(r.app.screen() == hg::Screen::Connecting);
  r.app.on_transport_open();
  const Value* hello = r.fake.last("hello");
  CHECK(hello != nullptr);
  if (!hello) return;
  CHECK_EQ((*hello)["proto"].as_int(), int64_t(1));
  CHECK_EQ((*hello)["device_id"].as_string(), r.app.device_id());
  CHECK_EQ((*hello)["board"].as_string(), std::string("test-board"));
  CHECK_EQ((*hello)["caps"]["display"]["width"].as_int(), int64_t(320));
  CHECK_EQ((*hello)["caps"]["mic"]["rate"].as_int(), int64_t(16000));
  CHECK((*hello)["caps"]["speaker"].is_object());
  bool has_volume = false;
  for (const auto& a : (*hello)["actions"].elements()) has_volume |= a["name"].as_string() == "speaker.volume";
  CHECK(has_volume);
}

TEST("app: hello announces the speaker's playback buffer when the port knows it") {
  hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");
  p.speaker_rate = 24000;
  Rig r(p);
  r.fake.speaker_buffer = 192 * 1024;  // the AMOLED-1.75C's buffer: 4 s at 24 kHz
  r.app.begin();
  r.app.on_network(true);
  r.advance(1000);
  r.app.on_transport_open();
  const Value* hello = r.fake.last("hello");
  CHECK(hello != nullptr);
  if (!hello) return;
  CHECK_EQ((*hello)["caps"]["speaker"]["buffer_ms"].as_int(), int64_t(4096));

  Rig unknown;  // a port that doesn't report its buffer announces none
  unknown.app.begin();
  unknown.app.on_network(true);
  unknown.advance(1000);
  unknown.app.on_transport_open();
  const Value* plain = unknown.fake.last("hello");
  CHECK(plain && (*plain)["caps"]["speaker"].is_object() && (*plain)["caps"]["speaker"]["buffer_ms"].is_null());
}

TEST("app: first contact enrolls the key, later contact proves it with an HMAC") {
  Rig r;
  r.app.begin();
  r.app.on_network(true);
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"abc","enrolled":false})");
  const Value* auth = r.fake.last("auth");
  CHECK(auth && (*auth)["key"].is_string() && !(*auth)["mac"].is_string());
  std::vector<uint8_t> key;
  CHECK(hg::crypto::base64_decode((*auth)["key"].as_string(), key));
  CHECK_EQ(hg::proto::device_id_for_key(key.data(), key.size()), r.app.device_id());

  r.fake.sent.clear();
  r.app.on_transport_closed("bye");
  r.advance(3000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"n2","enrolled":true})");
  auth = r.fake.last("auth");
  CHECK(auth && !(*auth)["key"].is_string());
  CHECK_EQ((*auth)["mac"].as_string(), hg::proto::auth_mac(key.data(), key.size(), r.app.device_id(), "n2"));
}

TEST("app: the device key persists across restarts") {
  Rig r;
  r.app.begin();
  std::string id = r.app.device_id();
  hg::Hal hal = r.fake.hal();
  hg::App again(hal, Rig::profile("ws://x"));
  again.begin();
  CHECK_EQ(again.device_id(), id);
}

TEST("app: unpaired devices show the pairing code until approved") {
  Rig r;
  r.bring_online(false);
  CHECK(r.app.screen() == hg::Screen::Pairing);
  r.server(R"({"type":"pairing","code":"ABCD2345","command":"hermes pairing approve gadget ABCD2345"})");
  CHECK_EQ(r.app.model().code, std::string("ABCD2345"));
  // Talking is refused while unpaired.
  r.app.on_button(hg::Button::Talk, true);
  CHECK(!r.fake.mic_on);
  r.server(R"({"type":"paired"})");
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("app: a device a route refuses says so until a new connection or approval") {
  const std::string asking = "Asking Hermes for a pairing code...";
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"unpaired","reason":"routed"})");
  CHECK(r.app.screen() == hg::Screen::Pairing);
  CHECK_EQ(r.app.model().headline, std::string("Refused by a route"));
  CHECK_EQ(r.app.model().detail, std::string("Remove its route on the Hermes host."));
  CHECK_EQ(r.app.model().hint, r.app.device_id());
  CHECK(r.app.console("status").find(R"("unpaired_reason":"routed")") != std::string::npos);
  r.server(R"({"type":"paired"})");
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(r.app.console("status").find("unpaired_reason") == std::string::npos);
  r.server(R"({"type":"unpaired"})");  // revoked: a code can fix that
  CHECK_EQ(r.app.model().detail, asking);
  CHECK(r.app.model().hint != r.app.device_id());
  r.server(R"({"type":"unpaired","reason":"a-later-reason"})");  // one this firmware doesn't know
  CHECK_EQ(r.app.model().detail, asking);

  r.server(R"({"type":"unpaired","reason":"routed"})");
  r.app.on_transport_closed("server restarted");
  r.advance(1500);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":false})");
  CHECK_EQ(r.app.model().detail, asking);  // the new connection says again, or not
}

TEST("app: a refusal for a route drops a pairing code shown before") {
  Rig r;
  r.bring_online(false);
  r.server(R"({"type":"pairing","code":"ABCD2345","command":"hermes pairing approve gadget ABCD2345"})");
  r.app.on_transport_closed("route configured");
  r.advance(3000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":false})");
  CHECK_EQ(r.app.model().code, std::string("ABCD2345"));  // revoked, not refused: the code still works
  r.server(R"({"type":"unpaired","reason":"routed"})");
  CHECK_EQ(r.app.model().headline, std::string("Refused by a route"));
  CHECK(r.app.model().code.empty());
  CHECK(r.app.console("status").find("pairing_code") == std::string::npos);
}

TEST("app: push-to-talk streams audio and shows the reply") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Listening);
  const Value* start = r.fake.last("audio.start");
  CHECK(start != nullptr);
  int stream = start ? static_cast<int>((*start)["stream"].as_int()) : -1;

  std::vector<int16_t> pcm(1600, 1000);  // 100 ms
  for (int i = 0; i < 6; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(100);
  }
  CHECK(!r.fake.sent_binary.empty());
  hg::proto::BinaryFrame f;
  CHECK(hg::proto::parse_binary(r.fake.sent_binary[0].data(), r.fake.sent_binary[0].size(), f));
  CHECK(f.channel == hg::proto::Channel::Audio);
  CHECK_EQ(int(f.stream), stream);
  size_t samples = 0;
  for (auto& b : r.fake.sent_binary) samples += (b.size() - hg::proto::kBinaryHeader) / 2;
  CHECK_EQ(samples, size_t(9600));

  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.end") != nullptr);
  CHECK(r.app.screen() == hg::Screen::Thinking);

  r.server(R"({"type":"turn.start","turn":"a1"})");
  r.server(R"({"type":"status","text":"Searching the web"})");
  CHECK_EQ(r.app.model().detail, std::string("Searching the web"));
  r.server(R"({"type":"reply.delta","turn":"a1","text":"It is"})");
  CHECK(r.app.screen() == hg::Screen::Responding);
  r.server(R"({"type":"reply","turn":"a1","text":"It is sunny."})");
  CHECK_EQ(r.app.model().body, std::string("It is sunny."));
  r.server(R"({"type":"turn.end","turn":"a1","outcome":"success"})");
  r.advance(200);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.app.model().body, std::string("It is sunny."));
}

TEST("ring: the listening arc grows with the seconds and turns amber in the last 5 s") {
  constexpr uint16_t kGreen = hg::rgb565(61, 214, 140);
  constexpr uint16_t kGreenDim = hg::rgb565(28, 92, 62);
  constexpr uint16_t kAccent = hg::rgb565(242, 179, 61);
  Rig r;
  r.fake.make_round(466);
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  const auto at = [&](int x, int y) { return r.fake.fb[static_cast<size_t>(y * 466 + x)]; };

  r.advance(15000);  // half of the 30 s recording limit (the last frame may lag a little)
  const int16_t half = r.app.model().progress_permille;
  CHECK(half >= 490 && half <= 500);
  CHECK_EQ(at(456, 233), kGreen);    // 3 o'clock, on the arc
  CHECK_EQ(at(10, 233), kGreenDim);  // 9 o'clock, past it

  r.advance(11000);  // 26 s in: the last 5 s of the limit
  CHECK(r.app.model().progress_permille >= 834);
  CHECK_EQ(at(456, 233), kAccent);  // amber now
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("app: a turn lasts from TALK until the reply has finished playing") {
  Rig r;
  r.bring_online(true);
  CHECK(!r.app.in_turn());
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.in_turn());
  std::vector<int16_t> pcm(1600, 1000);
  for (int i = 0; i < 6; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(100);
  }
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.in_turn());  // waiting for the reply
  r.server(R"({"type":"turn.start","turn":"a1"})");
  r.server(R"({"type":"audio.start","stream":5,"rate":16000,"format":"pcm16"})");
  r.server(R"({"type":"reply","turn":"a1","text":"It is sunny."})");
  r.server(R"({"type":"turn.end","turn":"a1","outcome":"success"})");
  r.server(R"({"type":"audio.end","stream":5})");
  r.fake.spk_draining = true;
  r.advance(200);
  CHECK(r.app.in_turn());  // the text is done, the last audio is still playing
  r.fake.spk_draining = false;
  r.advance(200);
  CHECK(!r.app.in_turn());
}

TEST("app: a reply that arrives on its own is a turn while it plays, and a lost connection ends one") {
  Rig r;
  r.bring_online(true);
  // A question from the agent: its text, then its spoken version.
  r.server(R"({"type":"reply","text":"Red or blue?"})");
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.in_turn());
  r.server(R"({"type":"audio.end","stream":3})");
  r.advance(7000);  // past the 6 s the app waits for more of a reply without turn.end
  CHECK(!r.app.in_turn());

  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(1600, 1000);
  for (int i = 0; i < 6; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(100);
  }
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.in_turn());
  r.app.on_transport_closed("gone");
  r.advance(200);
  CHECK(!r.app.in_turn());
}

TEST("app: microphone frames the link refused are counted when the recording ends") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(640, 1000);  // one 40 ms frame
  for (int i = 0; i < 10; ++i) {
    r.fake.refuse_binary = i >= 4 && i < 7;
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(40);
  }
  r.fake.logs.clear();
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") != nullptr);
  bool counted = false;
  for (const auto& l : r.fake.logs) counted |= l.find("recording: 3 of 10 audio frames not sent") != std::string::npos;
  CHECK(counted);

  // The next recording starts from zero and, with every frame sent, logs nothing.
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(7000);
  r.app.on_button(hg::Button::Talk, true);
  r.fake.refuse_binary = false;
  for (int i = 0; i < 10; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(40);
  }
  r.fake.logs.clear();
  r.app.on_button(hg::Button::Talk, false);
  for (const auto& l : r.fake.logs) CHECK(l.find("recording:") == std::string::npos);
}

TEST("app: microphone audio goes in 40 ms frames whatever size the port delivers") {
  // The codec boards record at 24 kHz and deliver 320 samples (13 ms) at a time: before, each
  // went as its own message, 75 a second, and the send queue held under a second of audio.
  hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");
  p.mic_rate = 24000;
  Rig r(p);
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> all;
  int16_t next = 0;
  for (int i = 0; i < 31; ++i) {  // 9,920 samples: ten 960-sample frames and 320 over
    std::vector<int16_t> pcm(320);
    for (auto& v : pcm) v = next++;
    all.insert(all.end(), pcm.begin(), pcm.end());
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(10);
  }
  CHECK_EQ(r.fake.sent_binary.size(), size_t(10));
  for (const auto& f : r.fake.sent_binary) CHECK_EQ(f.size(), hg::proto::kBinaryHeader + 960 * 2);
  r.advance(400);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.sent_binary.size(), size_t(11));  // the part frame goes before audio.end
  CHECK_EQ(r.fake.sent_binary.back().size(), hg::proto::kBinaryHeader + 320 * 2);
  std::vector<int16_t> got;
  uint16_t seq = 0;
  for (const auto& f : r.fake.sent_binary) {
    hg::proto::BinaryFrame b;
    CHECK(hg::proto::parse_binary(f.data(), f.size(), b));
    CHECK_EQ(b.seq, seq++);
    const auto* sm = reinterpret_cast<const int16_t*>(f.data() + hg::proto::kBinaryHeader);
    got.insert(got.end(), sm, sm + (f.size() - hg::proto::kBinaryHeader) / 2);
  }
  CHECK(got == all);
  CHECK(r.fake.last("audio.end") != nullptr);

  // At 16 kHz, 640 samples, as before.
  Rig sixteen;
  sixteen.bring_online(true);
  sixteen.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(1600, 1);
  sixteen.app.on_mic_samples(pcm.data(), pcm.size());
  CHECK_EQ(sixteen.fake.sent_binary.size(), size_t(2));
  CHECK_EQ(sixteen.fake.sent_binary[0].size(), hg::proto::kBinaryHeader + 640 * 2);
}

TEST("app: a recording whose end can't be sent says so instead of waiting for a reply") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(1600, 1000);
  for (int i = 0; i < 6; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(100);
  }
  r.fake.refuse_text = true;
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() != hg::Screen::Thinking);
  CHECK(!r.app.in_turn());  // the radio may sleep again; nothing is coming
  CHECK_EQ(r.app.model().detail, std::string("Couldn't send the recording"));
  // The link recovers: the next recording works as usual.
  r.fake.refuse_text = false;
  r.advance(9000);
  r.app.on_button(hg::Button::Talk, true);
  for (int i = 0; i < 6; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(100);
  }
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
}

TEST("app: tap mode hears speech at the board's microphone level") {
  // A quiet microphone (the AMOLED-2.06's codec) gives speech around 300 RMS: under the default
  // floor that is no speech at all, so the board sets a lower one.
  auto talk_quietly = [](Rig& r) {
    r.fake.kv["talk_mode"] = "tap";
    r.bring_online(true);
    r.app.on_button(hg::Button::Talk, true);
    r.app.on_button(hg::Button::Talk, false);
    std::vector<int16_t> speech(1600), silence(1600, 20);  // 100 ms blocks
    for (size_t i = 0; i < speech.size(); ++i) speech[i] = (i / 8) % 2 ? 300 : -300;
    for (int i = 0; i < 10; ++i) {
      r.app.on_mic_samples(speech.data(), speech.size());
      r.advance(100);
    }
    for (int i = 0; i < 60 && r.fake.mic_on; ++i) {
      r.app.on_mic_samples(silence.data(), silence.size());
      r.advance(100);
    }
  };
  hg::DeviceProfile quiet = Rig::profile("ws://hermes.local:8765/gadget");
  quiet.vad_min_rms = 200;
  Rig r(quiet);
  talk_quietly(r);
  CHECK(r.fake.last("audio.end") != nullptr);
  CHECK(r.app.screen() == hg::Screen::Thinking);

  Rig d;  // the default floor
  talk_quietly(d);
  CHECK(d.fake.last("audio.end") == nullptr);
  CHECK(d.fake.last("audio.cancel") != nullptr);
}

TEST("app: a short tap is discarded instead of sent") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  r.advance(100);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.cancel") != nullptr);
  CHECK(r.fake.last("audio.end") == nullptr);
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("app: reply audio plays through the speaker and barge-in stops it") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  CHECK(r.fake.spk_open);
  std::vector<uint8_t> frame(hg::proto::kBinaryHeader + 640, 0);
  hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Audio, 3, 0);
  r.app.on_transport_binary(frame.data(), frame.size());
  CHECK_EQ(r.fake.spk_samples, size_t(320));
  // Frames for another stream are ignored.
  hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Audio, 4, 1);
  r.app.on_transport_binary(frame.data(), frame.size());
  CHECK_EQ(r.fake.spk_samples, size_t(320));
  r.advance(200);
  CHECK(r.app.model().speaking);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(!r.fake.spk_open);
  CHECK(r.app.screen() == hg::Screen::Listening);
}

// Stands in for an Opus decoder: each packet byte becomes four samples, and a packet starting
// 0xFF is one it cannot decode.
struct FakeDecoder : hg::AudioDecoder {
  uint32_t rate = 0;
  int packets = 0;
  const char* format() const override { return "opus"; }
  bool begin(uint32_t r) override { rate = r; return r == 16000; }
  int decode(const uint8_t* packet, size_t len, int16_t* pcm, size_t max_samples) override {
    ++packets;
    if (len == 0 || packet[0] == 0xFF) return -1;
    const size_t n = std::min(len * 4, max_samples);
    std::fill(pcm, pcm + n, int16_t(7));
    return static_cast<int>(n);
  }
};

TEST("app: a speaker decoder adds its format to hello and decodes compressed reply audio") {
  Rig plain;
  plain.bring_online(true);
  const auto* hello = plain.fake.last("hello");
  CHECK(hello && !(*hello)["caps"]["speaker"]["formats"].is_array());  // PCM-only devices stay as they were
  // An Opus stream to a device without a decoder is refused.
  plain.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"opus"})");
  CHECK(!plain.fake.spk_open);

  Rig r;
  FakeDecoder decoder;
  r.hal.speaker_decoder = &decoder;
  r.bring_online(true);
  hello = r.fake.last("hello");
  CHECK(hello != nullptr);
  if (!hello) return;
  CHECK_EQ((*hello)["caps"]["speaker"]["format"].as_string(), std::string("pcm16"));  // for older servers
  const auto& formats = (*hello)["caps"]["speaker"]["formats"];
  CHECK(formats.is_array() && formats.size() == 2);
  CHECK_EQ(formats[1].as_string(), std::string("opus"));

  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"opus"})");
  CHECK(r.fake.spk_open);
  CHECK_EQ(decoder.rate, uint32_t(16000));
  std::vector<uint8_t> frame(hg::proto::kBinaryHeader + 30, 1);
  hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Audio, 3, 0);
  r.app.on_transport_binary(frame.data(), frame.size());
  CHECK_EQ(r.fake.spk_samples, size_t(120));  // the decoder's output, not the packet bytes
  frame[hg::proto::kBinaryHeader] = 0xFF;  // a packet the decoder rejects plays nothing
  r.app.on_transport_binary(frame.data(), frame.size());
  CHECK_EQ(r.fake.spk_samples, size_t(120));
  CHECK_EQ(decoder.packets, 2);
  r.server(R"({"type":"audio.end","stream":3})");

  // The next PCM stream bypasses the decoder.
  r.server(R"({"type":"audio.start","stream":4,"rate":16000,"format":"pcm16"})");
  std::vector<uint8_t> pcm(hg::proto::kBinaryHeader + 640, 0);
  hg::proto::write_binary_header(pcm.data(), hg::proto::Channel::Audio, 4, 0);
  r.app.on_transport_binary(pcm.data(), pcm.size());
  CHECK_EQ(r.fake.spk_samples, size_t(440));
  CHECK_EQ(decoder.packets, 2);
  // A rate the decoder refuses leaves the speaker closed.
  r.server(R"({"type":"audio.end","stream":4})");
  r.server(R"({"type":"audio.start","stream":5,"rate":24000,"format":"opus"})");
  CHECK(!r.fake.spk_open);
}

TEST("app: agent actions run on the device and report results") {
  Rig r;
  int calls = 0;
  hg::Action led;
  led.name = "led.set";
  led.description = "Set the LED colour";
  led.handler = [&](const Value& args, Value& result, std::string& error) {
    ++calls;
    if (args["color"].as_string().empty()) {
      error = "color required";
      return false;
    }
    result.set("color", args["color"]);
    return true;
  };
  r.app.add_action(led);
  r.bring_online(true);
  r.server(R"({"type":"action","id":"x1","name":"led.set","args":{"color":"red"}})");
  const Value* res = r.fake.last("action.result");
  CHECK(res && (*res)["ok"].as_bool() && (*res)["result"]["color"].as_string() == "red");
  r.server(R"({"type":"action","id":"x2","name":"led.set","args":{}})");
  res = r.fake.last("action.result");
  CHECK(res && !(*res)["ok"].as_bool() && (*res)["error"].as_string() == "color required");
  r.server(R"({"type":"action","id":"x3","name":"nope","args":{}})");
  res = r.fake.last("action.result");
  CHECK(res && (*res)["id"].as_string() == "x3" && !(*res)["ok"].as_bool());
  CHECK_EQ(calls, 2);
}

TEST("app: lost connections retry with backoff") {
  Rig r;
  r.bring_online(true);
  r.app.on_transport_closed("server restarted");
  CHECK(r.app.screen() == hg::Screen::Connecting);
  int before = r.fake.connects;
  r.advance(500);
  CHECK_EQ(r.fake.connects, before);
  r.advance(1000);
  CHECK_EQ(r.fake.connects, before + 1);
}

TEST("app: a silent server is detected by the heartbeat timeout") {
  Rig r;
  r.bring_online(true);
  r.advance(61000);
  CHECK(r.fake.closes >= 1);
  CHECK(r.app.screen() == hg::Screen::Connecting);
}

TEST("app: traffic_at counts every frame from Hermes but its heartbeat ping") {
  Rig r;
  r.bring_online(true);
  r.advance(1000);
  const uint32_t before = r.app.traffic_at();
  CHECK(before != 0);  // the welcome
  r.server(R"({"type":"ping","ts":1})");
  CHECK_EQ(r.app.traffic_at(), before);
  r.advance(1000);
  r.server(R"({"type":"display","title":"Hi","body":"there","ttl_s":5})");
  CHECK_EQ(r.app.traffic_at(), r.fake.clock);
  r.advance(1000);
  const uint8_t frame[] = {0, 0, 0, 0};  // too short to parse: still traffic on the radio
  r.app.on_transport_binary(frame, sizeof(frame));
  CHECK_EQ(r.app.traffic_at(), r.fake.clock);
  r.advance(1000);
  r.app.on_transport_text("{not json");  // dropped, but not a ping either
  CHECK_EQ(r.app.traffic_at(), r.fake.clock);
}

TEST("app: console reconfigures the server and reconnects") {
  Rig r("");
  r.app.begin();
  r.app.on_network(true);
  r.advance(1000);
  CHECK(r.app.screen() == hg::Screen::Error);
  CHECK_EQ(r.app.console("set server ws://10.0.0.5:8765/gadget"), std::string("@ok server"));
  r.advance(100);
  CHECK_EQ(r.fake.url, std::string("ws://10.0.0.5:8765/gadget"));
  CHECK(r.app.console("get token").find("\"value\":\"\"") != std::string::npos);
  r.app.console("set token secret");
  CHECK(r.app.console("get token").find("<set>") != std::string::npos);
  CHECK(r.app.console("status").rfind("@status {", 0) == 0);
}

TEST("app: diag reports the core's state plus what the port adds") {
  Rig r;
  r.bring_online(true);
  CHECK_EQ(r.app.console("diag log"), std::string("@error this device keeps no log"));
  CHECK(r.app.console("help").find("diag") != std::string::npos);

  r.app.on_diag = [](Value& report) { report.set("reset_reason", "brownout"); };
  r.app.recent_log = [] { return std::string("I (10) hg: booted"); };
  std::string out = r.app.console("diag");
  CHECK(out.rfind("@diag {", 0) == 0);
  Value v;
  CHECK(hg::json::parse(out.substr(6), v));
  CHECK_EQ(v["board"].as_string(), std::string("test-board"));
  CHECK_EQ(v["firmware"].as_string(), std::string("1.2.3"));
  CHECK_EQ(v["app"]["phase"].as_string(), std::string("online"));
  CHECK_EQ(v["connection"]["network"].as_string(), std::string("test-wifi"));
  CHECK_EQ(v["reset_reason"].as_string(), std::string("brownout"));
  CHECK_EQ(r.app.console("diag log"), std::string("I (10) hg: booted\n@log end"));

  r.app.on_transport_closed("server went away");
  CHECK(hg::json::parse(r.app.console("diag").substr(6), v));
  CHECK_EQ(v["connection"]["last_close"].as_string(), std::string("server went away"));
}

namespace {

// The server's side of a firmware update, as the hub runs it.
struct OtaServer {
  Rig& r;
  std::vector<uint8_t> image;
  std::string sha;
  uint8_t stream = 9;
  uint16_t seq = 0;

  OtaServer(Rig& rig, size_t size) : r(rig), image(size) {
    for (size_t i = 0; i < size; ++i) image[i] = static_cast<uint8_t>(i * 7);
    hg::crypto::Digest d = hg::crypto::sha256(image.data(), image.size());
    sha = hg::crypto::hex(d.data(), d.size());
  }

  std::string key_b64() const { return r.fake.kv.at("device_key"); }

  std::string offer(size_t size = 0) {
    r.fake.sent.clear();
    r.server(R"({"type":"ota.offer","stream":9,"version":"9.9.9","size":)" +
             std::to_string(size ? size : image.size()) + R"(,"sha256":")" + sha + "\"}");
    const Value* ready = r.fake.last("ota.ready");
    return ready ? (*ready)["nonce"].as_string() : std::string();
  }

  void begin(const std::string& nonce, bool wrong_key = false) {
    std::vector<uint8_t> key;
    hg::crypto::base64_decode(key_b64(), key);
    if (wrong_key) key[0] ^= 1;
    std::string mac = hg::proto::ota_mac(key.data(), key.size(), r.app.device_id(), nonce, sha, image.size());
    r.server(R"({"type":"ota.begin","mac":")" + mac + "\"}");
  }

  void chunk(size_t off, size_t len) {
    std::vector<uint8_t> frame(hg::proto::kBinaryHeader + len);
    hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Firmware, stream, seq++);
    std::copy(image.begin() + static_cast<long>(off), image.begin() + static_cast<long>(off + len),
              frame.begin() + hg::proto::kBinaryHeader);
    r.app.on_transport_binary(frame.data(), frame.size());
  }

  void send_all() {
    for (size_t off = 0; off < image.size(); off += 4096) chunk(off, std::min<size_t>(4096, image.size() - off));
  }

  std::string error_code() const {
    const Value* e = r.fake.last("ota.error");
    return e ? (*e)["code"].as_string() : std::string();
  }
};

}  // namespace

TEST("ota: an authorized image streams in, is checked, and the device restarts into it") {
  FakeUpdater upd;
  Rig r;
  r.hal.updater = &upd;
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["ota"]["max_size"].as_int() == 64 * 1024);

  OtaServer s(r, 40000);
  std::string nonce = s.offer();
  CHECK(!nonce.empty());
  s.begin(nonce);
  const Value* ack = r.fake.last("ota.ack");
  CHECK(ack && (*ack)["offset"].as_int() == 0);
  CHECK(upd.open);
  CHECK_EQ(r.app.screen(), hg::Screen::Updating);
  CHECK_EQ(r.app.model().progress_permille, int16_t(0));  // nothing received yet
  CHECK(r.app.model().detail.find("9.9.9: 0% of 40 KB") != std::string::npos);

  s.chunk(0, 4096);   // 8 of 40 KB: the edge ring follows the bytes received
  s.chunk(4096, 4096);
  r.advance(150);  // the model refreshes on the next frame
  CHECK_EQ(r.app.model().progress_permille, int16_t(204));
  for (size_t off = 8192; off < s.image.size(); off += 4096)
    s.chunk(off, std::min<size_t>(4096, s.image.size() - off));
  ack = r.fake.last("ota.ack");
  CHECK(ack && (*ack)["offset"].as_int() == 40000);  // 16 KB steps, then the end
  CHECK_EQ(r.app.model().progress_permille, int16_t(1000));
  CHECK_EQ(r.app.model().detail, std::string("firmware 9.9.9: 100% of 40 KB"));
  r.server(R"({"type":"ota.end"})");
  const Value* done = r.fake.last("ota.done");
  CHECK(done && (*done)["version"].as_string() == "9.9.9");
  CHECK(upd.installed && upd.image == s.image);
  CHECK(!upd.restarted);  // ota.done leaves first
  r.advance(150);  // restarting: the ring stays full while it waits to restart
  CHECK_EQ(r.app.screen(), hg::Screen::Updating);
  CHECK_EQ(r.app.model().progress_permille, int16_t(1000));
  r.advance(1100);
  CHECK(upd.restarted);
}

TEST("ota: the device refuses images it can't trust or hold") {
  FakeUpdater upd;
  Rig r;
  r.hal.updater = &upd;
  r.bring_online(true);
  OtaServer s(r, 20000);

  s.begin(s.offer(), /*wrong_key=*/true);
  CHECK_EQ(s.error_code(), std::string("unauthorized"));
  CHECK(!upd.open);

  s.offer(100 * 1024);
  CHECK_EQ(s.error_code(), std::string("too_large"));

  r.server(R"({"type":"ota.begin","mac":"x"})");
  CHECK_EQ(s.error_code(), std::string("no_offer"));

  // A wrong SHA-256: the bytes arrive, but don't match what was offered.
  s.begin(s.offer());
  s.image[5] ^= 0xFF;
  s.send_all();
  r.server(R"({"type":"ota.end"})");
  CHECK_EQ(s.error_code(), std::string("checksum"));
  CHECK(!upd.installed);

  // The port rejects the image itself.
  s.image[5] ^= 0xFF;
  s.seq = 0;
  upd.reject_image = true;
  s.begin(s.offer());
  s.send_all();
  r.server(R"({"type":"ota.end"})");
  CHECK_EQ(s.error_code(), std::string("invalid"));
  CHECK(r.fake.last("ota.done") == nullptr);
  CHECK(r.app.model().detail.find("Update failed: not an app image") != std::string::npos);
}

TEST("ota: a missing chunk, a stall or a dropped connection abandons the update") {
  FakeUpdater upd;
  Rig r;
  r.hal.updater = &upd;
  r.bring_online(true);
  OtaServer s(r, 20000);

  s.begin(s.offer());
  s.chunk(0, 4096);
  ++s.seq;  // skip one
  s.chunk(8192, 4096);
  CHECK_EQ(s.error_code(), std::string("sequence"));
  CHECK(!upd.open);
  CHECK(r.app.screen() != hg::Screen::Updating);

  s.seq = 0;
  s.begin(s.offer());
  s.chunk(0, 4096);
  r.advance(31000);
  CHECK_EQ(s.error_code(), std::string("timeout"));
  CHECK(!upd.open);

  s.seq = 0;
  s.begin(s.offer());
  s.chunk(0, 4096);
  r.app.on_transport_closed("gone");
  CHECK(!upd.open);
  CHECK(r.app.status_json().find("\"update\"") == std::string::npos);

  // Devices without an update slot don't advertise one, and say so if offered an image.
  Rig plain;
  plain.bring_online(true);
  const Value* hello = plain.fake.last("hello");
  CHECK(hello && !(*hello)["caps"]["ota"].is_object());
  OtaServer p(plain, 100);
  p.offer();
  CHECK_EQ(p.error_code(), std::string("unsupported"));
}

TEST("ota: a newly installed firmware is kept once it reaches Hermes") {
  FakeUpdater upd;
  upd.pending = true;
  Rig r;
  r.hal.updater = &upd;
  r.app.begin();
  r.app.on_network(true, "test-wifi");
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  CHECK(!upd.confirmed);  // authenticating isn't enough
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":false})");
  CHECK(upd.confirmed);
}

TEST("app: sensor readings are reported, rate limited") {
  Rig r;
  r.bring_online(true);
  r.fake.sent.clear();
  r.app.set_sensor("battery_pct", 80);
  r.advance(100);
  const Value* st = r.fake.last("state");
  CHECK(st && (*st)["sensors"]["battery_pct"].as_number() == 80);
  r.fake.sent.clear();
  r.app.set_sensor("battery_pct", 79);
  r.advance(100);
  CHECK(r.fake.last("state") == nullptr);
  r.advance(2100);
  CHECK(r.fake.last("state") != nullptr);
}

TEST("app: the mascot fills idle, listening and thinking screens") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.model().hero);  // idle
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening && r.app.model().hero);
  r.advance(500);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking && r.app.model().hero);
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.screen() == hg::Screen::Responding && r.app.model().hero);
}

namespace {

constexpr const char* kLongReply =
    "The forecast for today is sunny with a light breeze, and tomorrow brings rain in the afternoon.";

// The caption under the talking face: two lines, the reply's latest, in order.
void check_latest_lines(const hg::UiModel& m, const std::string& reply) {
  CHECK(m.hero);
  CHECK_EQ(int(m.caption_lines), 2);
  const size_t nl = m.detail.find('\n');
  CHECK(nl != std::string::npos);
  if (nl == std::string::npos) return;
  const std::string first = m.detail.substr(0, nl), second = m.detail.substr(nl + 1);
  CHECK(!first.empty() && !second.empty());
  const std::string tail = first + " " + second;
  CHECK(reply.size() > tail.size());  // not the whole reply
  if (reply.size() > tail.size()) CHECK_EQ(reply.substr(reply.size() - tail.size()), tail);
}

std::string reply_msg(const char* type, const char* turn, const char* text) {
  return std::string(R"({"type":")") + type + R"(","turn":")" + turn + R"(","text":")" + text + "\"}";
}

}  // namespace

TEST("app: text, then speech: the face talks over the reply's latest lines, and the text shows when it ends") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(reply_msg("reply.delta", "t", "It is"));
  CHECK(r.app.screen() == hg::Screen::Responding && !r.app.model().hero);  // no speech yet: the text
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.screen() == hg::Screen::Responding && r.app.model().hero && r.app.model().speaking);
  CHECK_EQ(r.app.model().detail, std::string("It is"));
  CHECK_EQ(int(r.app.model().caption_lines), 2);
  r.server(reply_msg("reply", "t", kLongReply));
  check_latest_lines(r.app.model(), kLongReply);
  r.server(R"({"type":"audio.end","stream":2})");
  r.advance(100);
  CHECK(r.app.screen() == hg::Screen::Responding && !r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string(kLongReply));
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(200);
  CHECK(r.app.screen() == hg::Screen::Ready && !r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string(kLongReply));
}

TEST("app: speech, then text: the face talks from the start, and the text shows when the speech ends") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.screen() == hg::Screen::Responding && r.app.model().hero);
  r.server(reply_msg("reply.delta", "t", kLongReply));
  check_latest_lines(r.app.model(), kLongReply);
  r.server(reply_msg("reply", "t", kLongReply));
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(200);
  check_latest_lines(r.app.model(), kLongReply);  // the turn is over, the speech is not
  r.server(R"({"type":"audio.end","stream":2})");
  r.advance(200);
  CHECK(r.app.screen() == hg::Screen::Ready && !r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string(kLongReply));
}

TEST("app: a reply with no speech is text from its first words") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(reply_msg("reply.delta", "t", "It is"));
  CHECK(r.app.screen() == hg::Screen::Responding && !r.app.model().hero);
  r.server(reply_msg("reply", "t", kLongReply));
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string(kLongReply));
}

TEST("app: a tap or a scroll during speech shows the whole text, and the speech goes on") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  hg::TouchGestures touch(r.app);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(reply_msg("reply", "t", kLongReply));
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  check_latest_lines(r.app.model(), kLongReply);
  touch.update(true, 160, 120, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock + 30);
  CHECK(r.app.screen() == hg::Screen::Responding && !r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string(kLongReply));
  CHECK(r.fake.spk_open && !r.fake.mic_on);
  CHECK(r.fake.last("audio.start") == nullptr);  // the tap recorded nothing
  // The next spoken reply has its caption again.
  r.server(R"({"type":"audio.end","stream":2})");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(200);
  r.server(R"({"type":"turn.start","turn":"t2"})");
  r.server(reply_msg("reply", "t2", kLongReply));
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  check_latest_lines(r.app.model(), kLongReply);

  // On a board with buttons, UP or DOWN shows the text from where it is, without scrolling it yet.
  Rig b;
  b.bring_online(true);
  b.server(reply_msg("reply", "", kLongReply));
  b.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  check_latest_lines(b.app.model(), kLongReply);
  const int scroll = b.app.model().scroll;
  b.app.on_button(hg::Button::Down, true);
  b.app.on_button(hg::Button::Down, false);
  CHECK(!b.app.model().hero);
  CHECK_EQ(b.app.model().scroll, scroll);
  CHECK(b.fake.spk_open);
}

TEST("app: a question or a short card over a reply keeps its tap") {
  auto tap = [](Rig& r, hg::TouchGestures& touch) {
    touch.update(true, 160, 120, r.fake.clock);
    r.advance(30);
    touch.update(false, 0, 0, r.fake.clock);
  };
  // A question that arrives during the speech: the tap answers yes.
  {
    Rig r(Rig::touch_profile());
    r.bring_online(true);
    hg::TouchGestures touch(r.app);
    r.server(R"({"type":"turn.start","turn":"t"})");
    r.server(reply_msg("reply", "t", kLongReply));
    r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
    r.server(R"({"type":"prompt","id":"q","title":"Allow?","text":"Continue?"})");
    r.advance(700);
    CHECK(r.app.screen() == hg::Screen::Prompt);
    tap(r, touch);
    CHECK(r.fake.last("prompt.reply") != nullptr);
  }
  // A short card during the speech: the tap closes it and stops the speech, as before.
  {
    Rig r(Rig::touch_profile());
    r.bring_online(true);
    hg::TouchGestures touch(r.app);
    r.server(R"({"type":"turn.start","turn":"t"})");
    r.server(reply_msg("reply", "t", kLongReply));
    r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
    r.server(R"({"type":"display","title":"Card","body":"Remember milk.","ttl_s":0})");
    CHECK(r.app.screen() == hg::Screen::Card && r.app.model().hero);
    tap(r, touch);
    CHECK(r.app.screen() != hg::Screen::Card);
    CHECK(!r.fake.spk_open);
  }
  // A question over a silent reply on a panel with no caption: the tap answers it.
  {
    Rig r(Rig::touch_profile());
    r.fake.make_panel(240, 135);
    r.bring_online(true);
    hg::TouchGestures touch(r.app);
    r.server(reply_msg("reply.delta", "t", "Silent text."));
    r.server(R"({"type":"prompt","id":"q","title":"Allow?","text":"Continue?"})");
    r.advance(700);
    tap(r, touch);
    CHECK(r.fake.last("prompt.reply") != nullptr);
  }
}

TEST("app: a long reply spoken under the caption gives its first page the whole reading time after the speech") {
  Rig r;
  r.bring_online(true);
  std::string text;
  for (int i = 0; i < 40; ++i) text += "sentence " + std::to_string(i) + " of a long answer. ";
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  r.server(reply_msg("reply", "t", text.c_str()));
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(7000);  // past the first page's reading time, partway into a second one
  CHECK(r.app.model().hero);
  r.server(R"({"type":"audio.end","stream":2})");
  r.advance(100);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().scroll, 0);
  r.advance(7000);
  CHECK_EQ(r.app.model().scroll, 0);  // the first page gets its reading time
  r.advance(1500);
  CHECK(r.app.model().scroll > 0);
}

TEST("app: where the face and two caption lines don't fit, a spoken reply shows its text as before") {
  Rig r;
  r.fake.make_panel(240, 135);
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.screen() == hg::Screen::Responding && r.app.model().hero);  // no text yet: the face
  r.server(reply_msg("reply", "t", kLongReply));
  CHECK(r.app.model().speaking && !r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string(kLongReply));
}

TEST("app: a finished reply stays readable, then the mascot returns; UP brings it back") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"reply","turn":"t","text":"Here is the answer."})");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(200);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("Here is the answer."));
  r.advance(21000);
  CHECK(r.app.model().hero);
  r.app.on_button(hg::Button::Up, true);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("Here is the answer."));
}

namespace {
// A held TALK with speech in it, sent: the device waits for Hermes (Thinking).
void ask_by_voice(Rig& r) {
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(1600, 1000);
  for (int i = 0; i < 6; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(100);
  }
  r.app.on_button(hg::Button::Talk, false);
}

// Time passes with the connection alive (pings) and no word about the turn.
void wait_quietly(Rig& r, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10000) {
    r.server(R"({"type":"ping"})");
    r.advance(10000);
  }
}
}  // namespace

TEST("app: once Hermes starts the turn, Thinking waits with the screen lit through a long task") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  ask_by_voice(r);
  r.server(R"({"type":"turn.start","turn":"a1"})");
  wait_quietly(r, 10 * 60000);  // ten minutes without a status phrase
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK_EQ(r.fake.brightness, 100);
  r.server(R"({"type":"reply","turn":"a1","text":"Done at last."})");
  r.server(R"({"type":"turn.end","turn":"a1","outcome":"success"})");
  r.advance(200);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.app.model().body, std::string("Done at last."));

  // A request Hermes never took up still gives up after three minutes.
  Rig lost(Rig::touch_profile());
  lost.bring_online(true);
  ask_by_voice(lost);
  wait_quietly(lost, 170000);
  CHECK(lost.app.screen() == hg::Screen::Thinking);
  lost.server(R"({"type":"ping"})");
  lost.advance(11000);
  CHECK(lost.app.screen() == hg::Screen::Ready);
  CHECK_EQ(lost.app.model().detail, std::string("No reply from Hermes"));
}

TEST("app: a reply the screen went dark on comes back with the wake until an input meets it") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  ask_by_voice(r);
  r.server(R"({"type":"turn.start","turn":"a1"})");
  wait_quietly(r, 240000);
  r.server(R"({"type":"reply","turn":"a1","text":"It is sunny."})");
  r.server(R"({"type":"turn.end","turn":"a1","outcome":"success"})");
  wait_quietly(r, 60000);  // nobody looked: the text gave way to the mascot, then the screen went dark
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(r.app.model().hero);

  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 200, r.fake.clock);  // the touch that lights the screen is only a wake
  touch.update(false, 0, 0, r.fake.clock);
  r.advance(200);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(!r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("It is sunny."));
  CHECK_EQ(r.app.model().scroll, 0);

  // Still unread when the screen goes dark again: the next wake, by key, shows it again.
  wait_quietly(r, 40000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  r.advance(200);
  CHECK(!r.fake.mic_on);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("It is sunny."));

  // An input over the text reads it: the text leaves on its timer, and the next wake shows the mascot.
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  wait_quietly(r, 40000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  r.advance(200);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.model().hero);
}

TEST("app: an input over the mascot leaves the reply unread") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"reply","turn":"t","text":"Here is the answer."})");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  wait_quietly(r, 22000);  // the text has gone; the screen is dimmed, not dark
  CHECK(r.app.model().hero);
  CHECK(r.fake.brightness > 0);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  wait_quietly(r, 40000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Cancel, true);  // a wake
  r.app.on_button(hg::Button::Cancel, false);
  r.advance(200);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("Here is the answer."));
}

TEST("app: a press over the speaking mascot leaves the reply unread") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"audio.start","stream":2,"rate":16000,"format":"pcm16"})");
  r.server(R"({"type":"reply","turn":"t","text":"Here is the answer."})");
  CHECK(r.app.screen() == hg::Screen::Responding && r.app.model().hero);
  r.app.on_button(hg::Button::Down, true);  // over the talking face, not the text
  r.app.on_button(hg::Button::Down, false);
  r.server(R"({"type":"audio.end","stream":2})");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  wait_quietly(r, 80000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Cancel, true);  // a wake
  r.app.on_button(hg::Button::Cancel, false);
  r.advance(200);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("Here is the answer."));
}

TEST("app: PWR over a card leaves the reply under it unread") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"reply","turn":"t","text":"Here is the answer."})");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  wait_quietly(r, 10000);  // the turn settles; the reply's text still shows under what comes next
  r.server(R"({"type":"display","title":"Note","body":"Stays until closed","ttl_s":0})");
  CHECK(r.app.screen() == hg::Screen::Card);
  r.app.on_power_key();  // closes the card and turns the screen off
  CHECK_EQ(r.fake.brightness, 0);
  wait_quietly(r, 30000);  // dark past the reply's 20 s
  r.app.on_power_key();  // the wake shows the reply the card covered
  r.advance(200);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("Here is the answer."));
}

TEST("app: hero animation redraws only a slice of the screen") {
  Rig r;
  r.bring_online(true);
  r.advance(200);
  int flushes = r.fake.flushes, rows = r.fake.flushed_rows;
  r.advance(6000);  // at least one blink cycle, nothing else changing
  flushes = r.fake.flushes - flushes;
  rows = r.fake.flushed_rows - rows;
  CHECK(flushes > 0);
  // Each blink flushes only the eye rows (~25 of the 196-row mascot area).
  CHECK(rows / flushes < 60);
}

TEST("app: holding CANCEL starts a new session; a short press still cancels") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t1"})");
  CHECK(r.app.screen() == hg::Screen::Thinking);
  r.app.on_button(hg::Button::Cancel, true);
  r.advance(200);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(r.fake.last("cancel") != nullptr);
  CHECK(r.fake.last("session.new") == nullptr);

  r.fake.sent.clear();
  r.app.on_button(hg::Button::Cancel, true);
  r.advance(1000);
  CHECK(r.app.model().hint.find("New session in") == 0);  // countdown while holding
  CHECK(r.fake.last("session.new") == nullptr);
  r.advance(1200);
  CHECK(r.fake.last("session.new") != nullptr);  // fires without waiting for release
  CHECK(r.app.screen() == hg::Screen::Thinking);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(r.fake.last("cancel") == nullptr);  // the release does not also cancel
}

TEST("app: a new session discards a recording in progress") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.mic_on);
  CHECK(r.app.console("new-session") == "@ok new-session");
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.cancel") != nullptr);
  CHECK(r.fake.last("session.new") != nullptr);
}

TEST("app: rendering is deterministic for the same model") {
  Rig a, b;
  a.bring_online(true);
  b.bring_online(true);
  a.server(R"({"type":"reply","text":"Hello from Hermes"})");
  b.server(R"({"type":"reply","text":"Hello from Hermes"})");
  a.advance(300);
  b.advance(300);
  CHECK(a.fake.fb == b.fake.fb);
  CHECK(a.fake.flushes > 0);
}

TEST("app: a question from Hermes shows the mascot with TALK/CANCEL answers") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"prompt","id":"q1","title":"Confirm /new","text":"This starts a fresh session."})");
  CHECK(r.app.screen() == hg::Screen::Prompt);
  CHECK(r.app.model().hero);
  CHECK_EQ(r.app.model().headline, std::string("Confirm /new"));
  CHECK_EQ(r.app.model().yes, std::string("TALK: Yes"));
  CHECK_EQ(r.app.model().no, std::string("CANCEL: No"));
  // A press that lands as the question appears was meant for something else.
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("prompt.reply") == nullptr);
  CHECK(r.app.screen() == hg::Screen::Prompt);
  r.advance(700);
  r.app.on_button(hg::Button::Talk, true);
  const Value* reply = r.fake.last("prompt.reply");
  CHECK(reply != nullptr);
  if (!reply) return;
  CHECK_EQ((*reply)["id"].as_string(), std::string("q1"));
  CHECK_EQ((*reply)["answer"].as_string(), std::string("yes"));
  CHECK(!r.fake.mic_on);  // answering does not start a recording
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
}

TEST("app: CANCEL answers no and holding it does not start a new session") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"prompt","id":"q2","title":"Allow command?","text":"rm -rf build"})");
  r.advance(700);
  r.app.on_button(hg::Button::Cancel, true);
  r.advance(2500);
  CHECK(r.fake.last("session.new") == nullptr);
  r.app.on_button(hg::Button::Cancel, false);
  const Value* reply = r.fake.last("prompt.reply");
  CHECK(reply && (*reply)["answer"].as_string() == "no");
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("app: a question waits for a recording, and can be withdrawn or expire") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  r.server(R"({"type":"prompt","id":"q3","text":"Continue?"})");
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.advance(500);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") != nullptr);
  CHECK(r.app.screen() == hg::Screen::Prompt);
  CHECK(r.app.status_json().find("\"prompt\":\"q3\"") != std::string::npos);

  r.server(R"({"type":"prompt.close","id":"other"})");
  CHECK(r.app.screen() == hg::Screen::Prompt);
  r.server(R"({"type":"prompt.close","id":"q3"})");
  CHECK(r.app.screen() != hg::Screen::Prompt);

  r.server(R"({"type":"prompt","id":"q4","text":"Quick?","ttl_s":5})");
  CHECK(r.app.screen() == hg::Screen::Prompt);
  r.advance(5100);
  CHECK(r.app.screen() != hg::Screen::Prompt);
  CHECK(r.fake.last("prompt.reply") == nullptr);  // expiry is silent; Hermes times out on its own

  r.server(R"({"type":"prompt","id":"q5","text":"Via console?"})");
  CHECK(r.app.console("yes") == "@ok yes");
  CHECK(r.fake.last("prompt.reply") && (*r.fake.last("prompt.reply"))["id"].as_string() == "q5");
  CHECK(r.app.console("no") == "@error no question to answer");
}

TEST("app: connection, pairing and setup screens show the mascot") {
  Rig r;
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.advance(1000);
  CHECK(r.app.screen() == hg::Screen::Connecting && r.app.model().hero);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":false})");
  CHECK(r.app.screen() == hg::Screen::Pairing && r.app.model().hero);
  r.server(R"({"type":"pairing","code":"ABCD2345","command":"hermes pairing approve gadget ABCD2345"})");
  CHECK(r.app.model().headline.find("ABCD2345") != std::string::npos);
  CHECK(r.app.model().detail.find("hermes pairing approve gadget ABCD2345") != std::string::npos);
  CHECK(r.app.model().caption_lines >= 3);

  Rig unset("");
  unset.app.begin();
  unset.app.on_network(true);
  unset.advance(1000);
  CHECK(unset.app.screen() == hg::Screen::Error && unset.app.model().hero);
}

TEST("app: short cards sit under the mascot, long ones use the text layout") {
  Rig r;
  r.bring_online(true);
  r.server(R"({"type":"display","title":"Timer","body":"Pasta: 9 min"})");
  CHECK(r.app.screen() == hg::Screen::Card && r.app.model().hero);
  CHECK_EQ(r.app.model().detail, std::string("Pasta: 9 min"));
  std::string body;
  for (int i = 0; i < 30; ++i) body += "line " + std::to_string(i) + " of a long card. ";
  r.server(R"({"type":"display","title":"Notes","ttl_s":0,"body":")" + body + "\"}");
  CHECK(r.app.screen() == hg::Screen::Card && !r.app.model().hero);
  CHECK_EQ(r.app.model().scroll, 0);
  r.advance(8100);
  CHECK(r.app.model().scroll > 0);  // pages without scroll buttons
}

TEST("app: long replies page themselves; UP/DOWN pauses the paging") {
  Rig r;
  r.bring_online(true);
  std::string text;
  for (int i = 0; i < 40; ++i) text += "sentence " + std::to_string(i) + " of a long answer. ";
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"reply","turn":"t","text":")" + text + "\"}");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  CHECK_EQ(r.app.model().scroll, 0);
  r.advance(4000);
  CHECK_EQ(r.app.model().scroll, 0);  // the first page gets its reading time
  r.advance(4100);
  int page2 = r.app.model().scroll;
  CHECK(page2 > 0);
  r.advance(8100);
  CHECK(r.app.model().scroll > page2);

  r.server(R"({"type":"ping","ts":1})");  // keep the heartbeat alive through the long waits
  int before = r.app.model().scroll;
  r.app.on_button(hg::Button::Up, true);
  int manual = r.app.model().scroll;
  CHECK(manual < before);
  r.advance(12000);
  CHECK_EQ(r.app.model().scroll, manual);  // paused after a manual scroll
  CHECK(!r.app.model().hero);              // and the reply is still on screen

  // Paging stops at the last page, which stays readable before the mascot returns.
  int last = -1;
  for (int i = 0; i < 40 && r.app.model().scroll != last; ++i) {
    last = r.app.model().scroll;
    r.server(R"({"type":"ping","ts":1})");
    r.advance(9000);
  }
  CHECK(!r.app.model().hero);
  r.advance(25000);
  CHECK(r.app.model().hero);
}

TEST("app: on a round panel everything stays inside the circle") {
  Rig r;
  r.fake.make_round(466);
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["display"]["shape"].as_string() == "round");
  r.server(R"({"type":"reply","text":"A long enough answer to fill several lines of the round screen with text."})");
  r.advance(300);
  CHECK(r.app.model().body.size() > 0);
  // Every pixel outside the circle is still the background colour.
  const uint16_t bg = r.fake.fb[static_cast<size_t>(233 * 466)];  // left edge of the middle row
  int stray = 0, drawn = 0;
  for (int y = 0; y < 466; ++y) {
    for (int x = 0; x < 466; ++x) {
      int dx = 2 * x + 1 - 466, dy = 2 * y + 1 - 466;
      bool lit = r.fake.fb[static_cast<size_t>(y * 466 + x)] != bg;
      if (dx * dx + dy * dy > 466 * 466) stray += lit;
      else drawn += lit;
    }
  }
  CHECK_EQ(stray, 0);
  CHECK(drawn > 1000);  // and the reply really was drawn inside
}

TEST("touch: hold to talk, lift to send") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  CHECK_EQ(r.app.model().hint, std::string("Hold the screen to talk"));
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 200, r.fake.clock);
  CHECK(!r.fake.mic_on);  // not yet: it could still become a swipe
  r.advance(150);
  touch.tick(r.fake.clock);
  CHECK(r.fake.mic_on);
  CHECK_EQ(r.app.model().detail, std::string("Lift your finger to send"));
  r.advance(600);
  touch.update(true, 204, 203, r.fake.clock);  // a wobbly finger is still a hold
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.end") != nullptr);
}

TEST("touch: with touch_wake off, a dark screen ignores the touchscreen until a key lights it") {
  hg::DeviceProfile p = Rig::touch_profile();
  p.wake_key = true;
  Rig r(p);
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set touch_wake off"), std::string("@ok touch_wake"));
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.advance(31000);
  CHECK_EQ(r.fake.brightness, 0);
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 200, r.fake.clock);  // a hold on the dark screen: ignored, no wake, no talk
  r.advance(500);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, true);  // the key lights it, and its press is only a wake
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(!r.fake.mic_on);
  touch.update(true, 200, 200, r.fake.clock);  // lit, the touchscreen works as before
  r.advance(150);
  touch.tick(r.fake.clock);
  CHECK(r.fake.mic_on);
  touch.update(false, 0, 0, r.fake.clock);
}

TEST("touch: touch_wake stays on without a key to wake the screen, and takes only on or off") {
  Rig r(Rig::touch_profile());  // no wake_key: the touchscreen is the only way back
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set touch_wake maybe"), std::string("@error touch_wake must be on or off"));
  CHECK_EQ(r.app.console("set touch_wake off"), std::string("@ok touch_wake"));
  CHECK(r.app.touch_wake());
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.advance(31000);
  CHECK_EQ(r.fake.brightness, 0);
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 200, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("touch: the Touch wakes screen item follows Screen timeout on a touchscreen with a wake key, and toggles") {
  hg::DeviceProfile p = Rig::touch_profile();
  p.wake_key = true;
  Rig r(p);
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Screen timeout"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Screen timeout"));
  r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Touch wakes screen"));
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  CHECK(!r.app.touch_wake());
  CHECK_EQ(r.fake.kv["touch_wake"], std::string("off"));
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  CHECK(r.app.touch_wake());
  r.app.close_settings();
}

TEST("touch: the Touch wakes screen item is never offered without a wake key or a touchscreen") {
  const auto never_offered = [](Rig& r) {
    CHECK(r.app.open_settings());
    for (int i = 0; i < 20; ++i) {
      r.app.on_button(hg::Button::Down, true);
      CHECK(r.app.model().detail != std::string("Touch wakes screen"));
    }
    r.app.close_settings();
  };
  {
    Rig r(Rig::touch_profile());  // a touchscreen, but no key to wake it
    r.fake.backlight = true;
    r.bring_online(true);
    never_offered(r);
  }
  {
    hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");  // a key, but no touchscreen
    p.wake_key = true;
    Rig r(p);
    r.fake.backlight = true;
    r.bring_online(true);
    never_offered(r);
  }
}

namespace {
// A touchscreen with a TALK key (BOOT), like the AMOLED watches.
hg::DeviceProfile touch_talk_key_profile() {
  hg::DeviceProfile p = Rig::touch_profile();
  p.talk_key = true;
  p.talk_label = "BOOT";
  return p;
}
}  // namespace

TEST("touch: with touch_talk off, holds and taps on the screen never record in hold mode; BOOT does") {
  Rig r(touch_talk_key_profile());
  r.bring_online(true);
  CHECK_EQ(r.app.model().hint, std::string("Hold the screen to talk"));
  CHECK_EQ(r.app.console("set touch_talk off"), std::string("@ok touch_talk"));
  CHECK(!r.app.touch_talk());
  CHECK_EQ(r.app.model().hint, std::string("hold BOOT to talk"));
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 150, r.fake.clock);  // a hold
  r.advance(800);
  touch.tick(r.fake.clock);
  CHECK(!r.fake.mic_on);
  touch.update(false, 0, 0, r.fake.clock);
  touch.update(true, 200, 150, r.fake.clock);  // a quick tap
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.start") == nullptr);
  CHECK_EQ(r.app.model().hint, std::string("hold BOOT to talk"));  // no "Not connected" or other flash

  r.app.on_button(hg::Button::Talk, true);  // BOOT records as before
  CHECK(r.fake.mic_on);
  CHECK_EQ(r.app.model().detail, std::string("Release BOOT to send"));
  touch.update(true, 200, 120, r.fake.clock);  // and a swipe down still discards it
  r.advance(30);
  touch.update(true, 202, 220, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.cancel") != nullptr);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") == nullptr);

  r.advance(500);  // past the double-press window: this press talks again
  r.app.on_button(hg::Button::Talk, true);
  r.advance(600);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") != nullptr);
  r.server(R"({"type":"turn.start","turn":"a1"})");
  r.server(R"({"type":"reply.delta","turn":"a1","text":"It is"})");
  CHECK(r.app.screen() == hg::Screen::Responding);
  CHECK_EQ(r.app.model().hint, std::string("BOOT to reply"));
  touch.update(true, 200, 150, r.fake.clock);  // a hold on the reply doesn't talk over it
  r.advance(500);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Responding);
}

TEST("touch: with touch_talk off in tap mode, a screen tap neither starts nor sends a recording") {
  Rig r(touch_talk_key_profile());
  r.bring_online(true);
  CHECK_EQ(r.app.console("set talk_mode tap"), std::string("@ok talk_mode"));
  CHECK_EQ(r.app.console("set touch_talk off"), std::string("@ok touch_talk"));
  CHECK_EQ(r.app.model().hint, std::string("tap BOOT to talk"));
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 150, r.fake.clock);
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, true);  // BOOT starts it
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.mic_on);
  r.advance(600);
  touch.update(true, 200, 150, r.fake.clock);  // a tap on the screen doesn't send it
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.fake.mic_on);
  CHECK(r.fake.last("audio.end") == nullptr);
  r.app.on_button(hg::Button::Talk, true);  // BOOT does
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.end") != nullptr);
}

TEST("touch: with touch_talk off, the screen still answers questions, works settings and stops an alarm") {
  Rig r(touch_talk_key_profile());
  r.bring_online(true);
  CHECK_EQ(r.app.console("set touch_talk off"), std::string("@ok touch_talk"));
  hg::TouchGestures touch(r.app);
  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"rm -rf build"})");
  CHECK_EQ(r.app.model().yes, std::string("Tap: Yes"));
  r.advance(700);
  touch.update(true, 200, 200, r.fake.clock);  // a tap answers yes and records nothing
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  const Value* yes = r.fake.last("prompt.reply");
  CHECK(yes && (*yes)["answer"].as_string() == "yes");
  CHECK(!r.fake.mic_on);

  CHECK(r.app.open_settings());  // a tap changes the item: Volume goes up 10
  const std::string before = r.app.model().body;
  touch.update(true, 200, 200, r.fake.clock);
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.app.settings_open());
  CHECK(r.app.model().body != before);
  CHECK(!r.fake.mic_on);
  r.app.close_settings();

  r.server(R"({"type":"action","id":"t1","name":"timer.start","args":{"seconds":1}})");
  r.advance(1500);
  CHECK(r.app.screen() == hg::Screen::Alarm);
  CHECK_EQ(r.app.model().hint, std::string("Tap the screen to stop"));
  touch.update(true, 200, 200, r.fake.clock);
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.app.screen() != hg::Screen::Alarm);
  CHECK(!r.fake.mic_on);
}

TEST("touch: touch_talk takes only on or off, survives a restart, and stays on without a TALK key") {
  {
    Rig r(touch_talk_key_profile());
    r.bring_online(true);
    CHECK_EQ(r.app.console("set touch_talk maybe"), std::string("@error touch_talk must be on or off"));
    CHECK(r.app.touch_talk());
    CHECK_EQ(r.app.console("set touch_talk off"), std::string("@ok touch_talk"));
    CHECK_EQ(r.fake.kv["touch_talk"], std::string("off"));
    CHECK(r.app.console("get touch_talk").find("\"value\":\"off\"") != std::string::npos);
    Rig again(touch_talk_key_profile());
    again.fake.kv = r.fake.kv;
    again.bring_online(true);
    CHECK(!again.app.touch_talk());
    CHECK_EQ(r.app.console("set touch_talk"), std::string("@ok touch_talk"));  // clearing it is on
    CHECK(r.app.touch_talk());
  }
  {
    Rig r(Rig::touch_profile());  // no TALK key: the touchscreen is the only way to talk
    r.bring_online(true);
    CHECK_EQ(r.app.console("set touch_talk off"), std::string("@ok touch_talk"));
    CHECK(r.app.touch_talk());
    CHECK_EQ(r.app.model().hint, std::string("Hold the screen to talk"));
    hg::TouchGestures touch(r.app);
    touch.update(true, 200, 150, r.fake.clock);
    r.advance(500);
    touch.tick(r.fake.clock);
    CHECK(r.fake.mic_on);
    touch.update(false, 0, 0, r.fake.clock);
  }
}

TEST("touch: the Touch to talk item follows Talk mode on a touchscreen with a TALK key, and toggles") {
  Rig r(touch_talk_key_profile());
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Talk mode"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Talk mode"));
  r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Touch to talk"));
  CHECK_EQ(r.app.model().body, std::string("On\nTouch the screen to talk."));
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.app.model().body, std::string("Off\nTalk with BOOT; touch only navigates."));
  CHECK(!r.app.touch_talk());
  CHECK_EQ(r.fake.kv["touch_talk"], std::string("off"));
  r.app.close_settings();  // a tap on its row: "settings list: Touch to talk is a switch..."
}

TEST("touch: the Touch to talk item is never offered without a TALK key or a touchscreen") {
  const auto never_offered = [](Rig& r) {
    CHECK(r.app.open_settings());
    for (int i = 0; i < 20; ++i) {
      r.app.on_button(hg::Button::Down, true);
      CHECK(r.app.model().detail != std::string("Touch to talk"));
    }
    r.app.close_settings();
  };
  {
    Rig r(Rig::touch_profile());  // a touchscreen, but no TALK key
    r.bring_online(true);
    never_offered(r);
  }
  {
    hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");  // a TALK key, but no touchscreen
    p.talk_key = true;
    Rig r(p);
    r.bring_online(true);
    never_offered(r);
  }
}

TEST("touch: swipe down while holding discards the recording") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 150, r.fake.clock);
  r.advance(500);
  touch.tick(r.fake.clock);
  CHECK(r.fake.mic_on);
  touch.update(true, 205, 260, r.fake.clock);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.cancel") != nullptr);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.fake.last("audio.end") == nullptr);
}

TEST("touch: tap answers yes, swipe answers no, sideways drags do nothing") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  hg::TouchGestures touch(r.app);
  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"rm -rf build"})");
  CHECK_EQ(r.app.model().yes, std::string("Tap: Yes"));
  CHECK_EQ(r.app.model().no, std::string("Swipe: No"));
  r.advance(700);
  touch.update(true, 100, 100, r.fake.clock);  // sideways: ignored
  touch.update(true, 200, 104, r.fake.clock);
  r.advance(300);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.fake.last("prompt.reply") == nullptr);
  CHECK(!r.fake.mic_on);

  touch.update(true, 200, 200, r.fake.clock);  // quick tap
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  const Value* yes = r.fake.last("prompt.reply");
  CHECK(yes && (*yes)["answer"].as_string() == "yes");

  r.server(R"({"type":"prompt","id":"q2","title":"Allow?","text":"git push --force"})");
  r.advance(700);
  touch.update(true, 200, 120, r.fake.clock);
  r.advance(30);
  touch.update(true, 202, 220, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  const Value* no = r.fake.last("prompt.reply");
  CHECK(no && (*no)["id"].as_string() == "q2" && (*no)["answer"].as_string() == "no");
}

TEST("touch: swipes can be turned off; board settings go through the console") {
  Rig r(Rig::touch_profile());
  std::string changed;
  r.app.on_setting_changed = [&](std::string_view key) { changed = std::string(key); };
  r.bring_online(true);
  CHECK_EQ(r.app.console("set touch_cancel pwr"), std::string("@ok touch_cancel"));
  CHECK_EQ(changed, std::string("touch_cancel"));
  CHECK(r.app.console("get touch_cancel").find("\"pwr\"") != std::string::npos);
  CHECK(r.app.console("help").find("touch_cancel") != std::string::npos);
  CHECK_EQ(r.app.console("set nonsense 1"), std::string("@error unknown key"));

  hg::TouchGestures touch(r.app);
  touch.set_swipe_cancel(false);
  r.server(R"({"type":"turn.start","turn":"t"})");
  touch.update(true, 200, 100, r.fake.clock);
  touch.update(true, 200, 260, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.fake.last("cancel") == nullptr);
}

TEST("settings: physical chord opens locally and saves volume, brightness and talk mode") {
  Rig r;
  r.fake.backlight = true;
  r.app.begin();
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Talk, true);
  r.advance(1000);
  CHECK(r.app.screen() == hg::Screen::Settings);
  r.app.on_button(hg::Button::Talk, false);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.volume, 80);
  CHECK_EQ(r.fake.kv["volume"], std::string("80"));
  r.app.console("cancel");
  r.app.console("talk");
  r.app.console("release");
  CHECK_EQ(r.fake.brightness, 10);
  CHECK_EQ(r.fake.kv["brightness"], std::string("10"));
  r.app.console("cancel");
  r.app.console("talk");
  r.app.console("release");
  CHECK_EQ(r.fake.kv["talk_mode"], std::string("tap"));
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Cancel, true);
  r.advance(1000);
  r.app.on_button(hg::Button::Cancel, false);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.app.settings_open());
  CHECK_EQ(r.fake.kv["talk_mode"], std::string("tap"));
  Rig reboot;
  reboot.fake.backlight = true;
  reboot.fake.kv = r.fake.kv;
  reboot.app.begin();
  CHECK_EQ(reboot.fake.volume, 80);
  CHECK_EQ(reboot.fake.brightness, 10);
  CHECK_EQ(reboot.app.device_id(), r.app.device_id());
}

TEST("settings: hardware checks are local and prompts release the microphone") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 3; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Microphone check"));
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.fake.mic_on);
  const int16_t samples[] = {32767, -32767, 32767, -32767};
  r.app.on_mic_samples(samples, 4);
  CHECK_EQ(r.app.model().level, uint8_t(99));
  CHECK(r.app.model().body.find("Local only") != std::string::npos);
  CHECK(r.fake.sent_binary.empty());
  CHECK(r.fake.last("audio.start") == nullptr);
  r.app.console("cancel");
  CHECK(!r.fake.mic_on);
  CHECK_EQ(r.app.model().detail, std::string("Speaker check"));
  r.app.console("talk");
  r.app.console("release");
  r.advance(20);
  CHECK_EQ(r.fake.spk_samples, size_t(4000));
  CHECK(r.app.model().body.find("Tone finished") != std::string::npos);
  r.app.console("cancel");
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.app.model().color_test);
  CHECK_EQ(r.fake.fb[100 * 320 + 10], uint16_t(0xf800));
  CHECK_EQ(r.fake.fb[100 * 320 + 150], uint16_t(0x001f));
  r.app.close_settings();
  CHECK(r.app.open_settings());
  for (int i = 0; i < 3; ++i) r.app.console("cancel");
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.fake.mic_on);
  r.server(R"({"type":"prompt","id":"local-check","text":"Continue?"})");
  CHECK(!r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Prompt);
  CHECK(!r.app.open_settings());
}

TEST("settings: title hold and a drag on the list work without starting a recording") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  hg::TouchGestures touch(r.app);
  touch.set_swipe_cancel(false);
  touch.update(true, 100, 4, r.fake.clock);
  r.advance(1100);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.app.screen() == hg::Screen::Settings);
  CHECK(!r.fake.mic_on);
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  touch.update(true, 100, 200, r.fake.clock);  // a drag up scrolls; the selection stays
  touch.update(true, 100, 120, r.fake.clock + 30);
  touch.update(false, 0, 0, r.fake.clock + 40);
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  CHECK(r.app.model().settings_scroll > 0);
  CHECK(!r.fake.mic_on);
  touch.update(true, 100, 4, r.fake.clock);
  r.advance(1100);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.app.settings_open());
  CHECK(r.app.screen() == hg::Screen::Ready);
}

// The settings target of a round panel: where the UI square sits, and the hold target inside it.
struct RoundTarget {
  int diameter, left, top, width, height;
};
constexpr RoundTarget kRound360{360, 120, 53, 120, 36};
constexpr RoundTarget kRound466{466, 112, 68, 240, 36};

static void check_round_target_gestures(const RoundTarget& t) {
  Rig r(Rig::touch_profile());
  r.fake.make_round(t.diameter);
  r.bring_online(true);
  const int right = t.left + t.width - 1, bottom = t.top + t.height - 1, cx = t.left + t.width / 2;
  CHECK(r.app.settings_title_hit(t.left, t.top));
  CHECK(r.app.settings_title_hit(right, bottom));
  CHECK(!r.app.settings_title_hit(t.left - 1, t.top + 1));
  CHECK(!r.app.settings_title_hit(right + 1, t.top + 1));
  CHECK(!r.app.settings_title_hit(cx, t.top - 1));
  CHECK(!r.app.settings_title_hit(cx, bottom + 1));
  hg::TouchGestures touch(r.app);
  // Holding the target opens settings without ever starting the microphone.
  touch.update(true, cx, t.top + 10, r.fake.clock);
  r.advance(200);
  touch.tick(r.fake.clock);
  CHECK(!r.fake.mic_on);
  r.advance(900);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.app.settings_open());
  CHECK(!r.fake.mic_on);
  // And closes it again.
  touch.update(true, cx, t.top + 10, r.fake.clock);
  r.advance(1100);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.app.settings_open());
  // Just outside the target is still hold-to-talk.
  touch.update(true, right + 1, t.top + 10, r.fake.clock);
  r.advance(200);
  touch.tick(r.fake.clock);
  CHECK(r.fake.mic_on);
  r.app.console("cancel");
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.app.settings_open());
}

static void check_round_header_label(const RoundTarget& t) {
  FakeHal display;
  display.make_round(t.diameter);
  hg::Ui ui(display);
  hg::UiModel m;
  m.screen = hg::Screen::Ready;
  m.link = hg::Link::Online;
  ui.render(m);
  const auto top = [&]() {
    return std::vector<uint16_t>(display.fb.begin() + t.top * t.diameter,
                                 display.fb.begin() + (t.top + t.height) * t.diameter);
  };
  const auto ready = top();
  m.screen = hg::Screen::Settings;
  ui.render(m);
  CHECK(top() != ready);
  m.screen = hg::Screen::Ready;
  ui.render(m);
  CHECK(top() == ready);
}

TEST("settings: round 360 target holds without recording and leaves TALK outside it") {
  check_round_target_gestures(kRound360);
}

TEST("settings: round 466 target holds without recording and leaves TALK outside it") {
  check_round_target_gestures(kRound466);
}

TEST("settings: round header redraws its return label and restores settings label") {
  check_round_header_label(kRound360);
  check_round_header_label(kRound466);
}

TEST("settings: round target is drawn only while the hold works") {
  Rig r(Rig::touch_profile());
  r.fake.make_round(466);
  r.bring_online(true);
  const auto band = [&] {
    return std::vector<uint16_t>(r.fake.fb.begin() + kRound466.top * 466,
                                 r.fake.fb.begin() + (kRound466.top + kRound466.height) * 466);
  };
  const auto shown = band();
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"prompt","id":"q1","text":"Continue?"})");
  CHECK(r.app.screen() == hg::Screen::Prompt);
  CHECK(band() != shown);  // the label is hidden while a hold would do nothing
  r.advance(700);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK(band() == shown);
}

TEST("settings: rectangular boards keep the full-width title strip and their header") {
  Rig r(Rig::touch_profile());  // 320x240: scale 2, so the strip is 22 px tall
  r.bring_online(true);
  CHECK(r.app.settings_title_hit(0, 0));
  CHECK(r.app.settings_title_hit(319, 21));
  CHECK(!r.app.settings_title_hit(319, 22));
  CHECK(!r.app.settings_title_hit(160, 22));
  FakeHal display;
  hg::Ui ui(display);
  hg::UiModel m;
  m.screen = hg::Screen::Ready;
  m.link = hg::Link::Online;
  ui.render(m);
  const auto top = [&]() {
    return std::vector<uint16_t>(display.fb.begin(), display.fb.begin() + 22 * 320);
  };
  const auto ready = top();
  m.screen = hg::Screen::Settings;
  ui.render(m);
  CHECK(top() == ready);  // no BACK TO HERMES label on a rectangular header
}

TEST("power: idle screen dims, sleeps and consumes the wake input without recording") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  CHECK_EQ(r.app.console("set screen_timeout -1"), std::string("@error screen_timeout must be 0..3600 seconds"));
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 10);
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.start") == nullptr);
  r.app.console("talk");
  CHECK(r.fake.mic_on);
  r.advance(1000);
  r.app.console("cancel");
  r.app.console("release");
  r.server(R"({"type":"ping"})");
  r.advance(30000);
  CHECK_EQ(r.fake.brightness, 0);
  r.server(R"({"type":"prompt","id":"wake","text":"Continue?"})");
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Prompt);
  r.advance(30000);
  CHECK_EQ(r.fake.brightness, 100);
}

namespace {
// A PMIC whose power key the board can only see as short presses.
struct KeyPower : hg::Power {
  int presses = 0;
  std::optional<bool> usb = false;  // on battery unless a test plugs it in
  std::optional<bool> external_power() override { return usb; }
  std::optional<hg::PowerStatus> read() override { return hg::PowerStatus{true, 3900, 70, false, false}; }
  bool power_off() override { return true; }
  bool take_key_press() override {
    if (!presses) return false;
    --presses;
    return true;
  }
};
}  // namespace

TEST("power: the power key turns the screen off and back on, without a screen timeout") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  CHECK_EQ(r.fake.brightness, 100);
  key.presses = 1;
  r.advance(100);
  CHECK_EQ(r.fake.brightness, 0);
  for (int i = 0; i < 4; ++i) {  // a minute with the server alive
    r.server(R"({"type":"ping"})");
    r.advance(15000);
  }
  CHECK_EQ(r.fake.brightness, 0);  // stays off: no timeout brings it back
  key.presses = 1;
  r.advance(100);
  CHECK_EQ(r.fake.brightness, 100);
  for (int i = 0; i < 4; ++i) {
    r.server(R"({"type":"ping"})");
    r.advance(15000);
  }
  CHECK_EQ(r.fake.brightness, 100);  // and stays on
}

TEST("power: other input wakes a screen the power key turned off, and only wakes it") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(100);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.start") == nullptr);
}

TEST("power: a prompt turns a screen the power key turned off back on") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(100);
  CHECK_EQ(r.fake.brightness, 0);
  r.server(R"({"type":"prompt","id":"wake","text":"Continue?"})");
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Prompt);
}

TEST("power: a dark screen stays dark while the connection drops and comes back") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(100);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_transport_closed("gone");
  r.advance(5000);
  CHECK_EQ(r.fake.brightness, 0);  // reconnecting in the dark (a commute must not light it)
  r.app.on_network(false, "Wi-Fi lost");
  r.app.on_network(true, "192.168.1.146");
  r.advance(5000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);  // the user still wakes it
  CHECK_EQ(r.fake.brightness, 100);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("power: a lit screen stays lit while reconnecting, as before") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.app.on_transport_closed("gone");
  r.advance(60000);  // reconnect attempts fail: still not settled
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("power: the power key brings a dimmed screen back to full brightness") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 10);
  key.presses = 1;
  r.advance(100);
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("doze: a dark, idle screen on battery dozes; any input ends it before drawing") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  r.advance(1000);
  CHECK(!r.fake.dozing);  // the screen is on
  key.presses = 1;
  r.advance(200);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(r.fake.dozing);
  CHECK(r.app.dozing());
  r.app.on_button(hg::Button::Talk, true);
  CHECK(!r.fake.dozing);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(!r.fake.lit_while_dozing);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("doze: external power never dozes, and plugging in ends a doze within 100 ms") {
  KeyPower key;
  key.usb = true;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  // The charging notice would light the screen on plugging in; this is about the doze alone.
  CHECK_EQ(r.app.console("set charging_notice off"), std::string("@ok charging_notice"));
  key.presses = 1;
  r.advance(1000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.fake.dozing);  // dark but plugged in: USB debugging keeps working
  key.usb = false;
  r.advance(200);
  CHECK(r.fake.dozing);
  key.usb = true;
  r.advance(100);
  CHECK(!r.fake.dozing);
  CHECK_EQ(r.fake.brightness, 0);  // the screen stays off; only the doze ends
}

TEST("doze: an unknown power source counts as plugged in") {
  KeyPower key;
  key.usb = std::nullopt;  // the PMIC didn't answer
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(1000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.fake.dozing);
  CHECK_EQ(r.fake.doze_changes, 0);
}

TEST("doze: a prompt or a lost connection ends it; Screen timeout dozes too") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(200);
  CHECK(r.fake.dozing);
  r.server(R"({"type":"prompt","id":"wake","text":"Continue?"})");
  r.advance(10);
  CHECK(!r.fake.dozing);
  r.app.console("no");
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.server(R"({"type":"ping"})");
  r.advance(25000);
  r.server(R"({"type":"ping"})");
  r.advance(10000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(r.fake.dozing);
  r.app.on_transport_closed("gone");
  r.advance(10);
  CHECK(!r.fake.dozing);
}

TEST("doze: without Wi-Fi it keeps dozing between join attempts; reconnecting to Hermes is awake") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(200);
  CHECK(r.fake.dozing);
  r.app.on_network(false, "Wi-Fi lost (reason 200)");
  r.advance(60000);
  CHECK(r.fake.dozing);  // the port wakes the chip for each attempt
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_network(true, "192.168.1.146");
  r.advance(1500);  // Wi-Fi is back: reconnecting to Hermes
  CHECK(!r.fake.dozing);
}

TEST("doze: boards without a power reading never doze") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.server(R"({"type":"ping"})");
  r.advance(25000);
  r.server(R"({"type":"ping"})");
  r.advance(10000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.fake.dozing);
}

namespace {
// The last state message about the screen (sensor readings also travel as state).
const Value* last_display_state(const FakeHal& fake) {
  for (auto it = fake.sent.rbegin(); it != fake.sent.rend(); ++it)
    if ((*it)["type"].as_string() == "state" && (*it)["display_sleeping"].is_bool()) return &*it;
  return nullptr;
}
}  // namespace

TEST("face down: off by default; a motion board lying face down stays lit") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  r.hold_still(30, -20, -1000, 2000);
  CHECK_EQ(r.fake.brightness, 100);
  Value status;
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK(status["face_down"].is_bool() && !status["face_down"].as_bool());
}

TEST("face down: set face_down_sleep takes on or off only, and the choice survives a restart") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep maybe"), std::string("@error face_down_sleep must be on or off"));
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  Rig again(Rig::motion_profile());
  again.fake.backlight = true;
  again.fake.kv = r.fake.kv;
  again.bring_online(true);
  again.hold_still(0, 0, -1000, 1000);
  CHECK_EQ(again.fake.brightness, 0);  // on out of the box, no command needed
}

TEST("face down: the screen goes dark, listening is cancelled and Hermes is told; turned over it lights") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.mic_on);
  r.hold_still(30, -20, -1000, 1000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.cancel") != nullptr);
  Value status;
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK(status["face_down"].as_bool());
  const Value* dark = last_display_state(r.fake);
  CHECK(dark && (*dark)["display_sleeping"].as_bool());
  r.app.on_button(hg::Button::Talk, false);  // the release is swallowed, as on a timed-out screen
  CHECK_EQ(r.fake.brightness, 0);
  r.hold_still(0, 0, 1000, 500);
  CHECK_EQ(r.fake.brightness, 100);
  const Value* lit = last_display_state(r.fake);
  CHECK(lit && (*lit)["display_sleeping"].is_bool() && !(*lit)["display_sleeping"].as_bool());
}

TEST("face down: a reply being spoken goes quiet, the turn goes on, and its text waits for the turn-over") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  r.server(R"({"type":"reply","text":"Your package arrived."})");
  CHECK(r.fake.spk_open);
  r.hold_still(30, -20, -1000, 1000);
  CHECK(!r.fake.spk_open);
  CHECK(r.fake.last("cancel") == nullptr);  // Hermes keeps working
  CHECK_EQ(r.fake.brightness, 0);
  r.server(R"({"type":"audio.end","stream":3})");
  r.server(R"({"type":"turn.end","turn":"t1","outcome":"success"})");
  // Longer than a reply normally stays up: the text still waits for the turn-over.
  r.hold_still(0, 0, -1000, 30000);
  r.hold_still(0, 0, 1000, 500);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.app.model().hero);  // the reply's text, not the mascot
  CHECK_EQ(r.app.model().body, std::string("Your package arrived."));
}

TEST("face down: speech that arrives face down plays nothing, and its text waits for the turn-over") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  // A new turn starts while the last one's speech still plays: the mode is Thinking.
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  r.server(R"({"type":"turn.start","turn":"t2"})");
  CHECK(r.fake.spk_open);
  r.hold_still(30, -20, -1000, 1000);
  CHECK(!r.fake.spk_open);  // the left-over speech stops too
  CHECK_EQ(r.fake.brightness, 0);
  // The thinking turn answers while the gadget lies there: silent, and the text waits.
  r.server(R"({"type":"audio.start","stream":4,"rate":16000,"format":"pcm16"})");
  CHECK(!r.fake.spk_open);  // the stream is never begun
  r.server(R"({"type":"reply","text":"Your package arrived."})");
  r.server(R"({"type":"turn.end","turn":"t2","outcome":"success"})");
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.fake.mic_on);
  CHECK_EQ(r.fake.brightness, 0);
  // Longer than a reply normally stays up: the text still waits for the turn-over.
  r.hold_still(0, 0, -1000, 30000);
  r.hold_still(0, 0, 1000, 500);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.app.model().hero);  // the reply's text, not the mascot
  CHECK_EQ(r.app.model().body, std::string("Your package arrived."));
  CHECK_EQ(r.app.model().scroll, 0);  // from the top
}

TEST("face down: nothing lights the screen - a press, a question, brightness and volume changes") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  r.hold_still(0, 0, -1000, 1000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.fake.mic_on);  // the press is consumed, no recording starts
  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"Run the command"})");
  CHECK_EQ(r.fake.brightness, 0);
  r.server(R"({"type":"action","id":"b1","name":"screen.brightness","args":{"percent":60}})");
  CHECK_EQ(r.fake.brightness, 0);  // the level is stored, not applied
  CHECK_EQ(r.app.console("set volume 40"), std::string("@ok volume"));
  CHECK_EQ(r.fake.brightness, 0);
  r.hold_still(0, 0, -1000, 200);
  const Value* dark = last_display_state(r.fake);
  CHECK(dark && (*dark)["display_sleeping"].as_bool());  // Hermes still holds cards for it
  r.hold_still(0, 0, 1000, 500);
  CHECK_EQ(r.fake.brightness, 60);  // the turn-over applies the stored level
}

TEST("face down: a ringing alarm goes quiet; a timer that ends face down rings with a dark screen") {
  {
    KeyPower key;
    Rig r(Rig::motion_profile());
    r.fake.backlight = true;
    r.hal.power = &key;
    r.bring_online(true);
    CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
    r.server(R"({"type":"action","id":"x1","name":"timer.start","args":{"seconds":1}})");
    r.hold_still(0, 0, 1000, 1500);  // face up while it starts to ring
    CHECK(r.app.screen() == hg::Screen::Alarm);
    const size_t beeps = r.fake.spk_samples;
    CHECK(beeps > 0);
    r.hold_still(0, 0, -1000, 1000);  // face down: quiet, and the alarm stays up
    CHECK_EQ(r.fake.brightness, 0);
    CHECK(r.app.screen() == hg::Screen::Alarm);
    const size_t quiet = r.fake.spk_samples;
    r.hold_still(0, 0, -1000, 3000);
    CHECK_EQ(r.fake.spk_samples, quiet);
    r.hold_still(0, 0, 1000, 500);  // turned over: the alarm shows
    CHECK_EQ(r.fake.brightness, 100);
    CHECK(r.app.screen() == hg::Screen::Alarm);
    r.hold_still(0, 0, 1000, 3000);  // and stays quiet: laying it face down was the hush
    CHECK_EQ(r.fake.spk_samples, quiet);
  }
  {
    // On battery, a face-down idle board dozes; the alarm's beeps are never written while dozing.
    KeyPower key;
    Rig r(Rig::motion_profile());
    r.fake.backlight = true;
    r.hal.power = &key;
    r.bring_online(true);
    CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
    r.hold_still(0, 0, -1000, 1000);
    CHECK_EQ(r.fake.brightness, 0);
    CHECK(r.fake.dozing);
    r.server(R"({"type":"action","id":"x1","name":"timer.start","args":{"seconds":1}})");
    const size_t before = r.fake.spk_samples;
    r.hold_still(0, 0, -1000, 1600);  // the timer ends face down
    CHECK(r.fake.spk_samples > before);  // an alarm should be heard: it rings
    CHECK_EQ(r.fake.brightness, 0);       // with the screen dark
    CHECK(!r.fake.spoke_while_dozing);
  }
}

TEST("face down: a sensor that stops reporting for two seconds lights the screen") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  r.hold_still(0, 0, -1000, 1000);
  CHECK_EQ(r.fake.brightness, 0);
  r.advance(2100);  // no sample arrives
  CHECK_EQ(r.fake.brightness, 100);  // never stay dark on old data
}

TEST("face down: turning the setting off while face down lights the screen on the next tick") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  r.hold_still(0, 0, -1000, 1000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK_EQ(r.app.console("set face_down_sleep off"), std::string("@ok face_down_sleep"));
  CHECK_EQ(r.fake.brightness, 0);  // the console itself lights nothing
  r.advance(50);
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("face down: boards without a motion sensor or without a backlight never offer it") {
  const auto never_offered = [](Rig& r) {
    // A full lap of the menu never lands on the item.
    CHECK(r.app.open_settings());
    for (int i = 0; i < 20; ++i) {
      r.app.on_button(hg::Button::Down, true);
      CHECK(r.app.model().detail != std::string("Face-down sleep"));
    }
    r.app.close_settings();
  };
  {
    Rig plain;  // no motion sensor: the setting is stored and does nothing
    plain.fake.backlight = true;
    plain.bring_online(true);
    CHECK_EQ(plain.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
    plain.hold_still(0, 0, -1000, 2000);
    CHECK_EQ(plain.fake.brightness, 100);
    Value status;
    CHECK(hg::json::parse(plain.app.status_json(), status));
    CHECK(!status["face_down"].is_bool());
    never_offered(plain);
  }
  {
    Rig r(Rig::motion_profile());  // a motion sensor, but no backlight to turn off
    r.bring_online(true);
    CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
    r.hold_still(0, 0, -1000, 2000);
    // Its picture stays lit, so Hermes is never told that the screen is dark.
    const Value* dark = last_display_state(r.fake);
    CHECK(!dark || !(*dark)["display_sleeping"].as_bool());
    Value status;
    CHECK(hg::json::parse(r.app.status_json(), status));
    CHECK(!status["face_down"].is_bool());
    CHECK(!status["display_sleeping"].as_bool());
    never_offered(r);
  }
}

TEST("face down: the settings item follows Screen timeout, shows Off and toggles") {
  Rig r(Rig::motion_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Screen timeout"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Screen timeout"));
  r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Face-down sleep"));  // next after Screen timeout
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  r.app.close_settings();
  r.hold_still(0, 0, -1000, 1000);  // back on Off: lying face down does nothing
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("power: an idle settings menu sleeps on every view; the wake input restores it where it was") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  CHECK(r.app.open_settings());
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 10);  // dims halfway, menu still open
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 0);   // and goes dark: no settings view holds the display awake
  CHECK(r.app.settings_open());
  // The first input only wakes; the menu is still open, on the same item.
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.settings_open());
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.start") == nullptr);
  // A running hardware check sleeps too: the meter pauses while dark and the
  // wake input brings the same check back.
  for (int i = 0; i < 3; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Microphone check"));
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.fake.mic_on);
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 10);
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 0);   // sleeps even with the meter running
  CHECK(r.fake.mic_on);             // the check itself keeps running
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK_EQ(r.app.model().detail, std::string("Microphone check"));  // same check restored
  r.app.on_button(hg::Button::Talk, false);
  r.app.console("cancel");
  // Wi-Fi setup is not the settings menu: the user is copying instructions.
  r.app.console("settings close");
  CHECK(!r.app.settings_open());
  r.app.on_wifi_setup = [] { return "Temporary setup network"; };
  CHECK(r.app.start_wifi_setup());
  r.advance(30000);
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("power: failed readings replace stale data and shutdown requires a second local selection") {
  struct Battery : hg::Power {
    bool failed = false;
    int shutdowns = 0;
    std::optional<hg::PowerStatus> read() override {
      if (failed) return std::nullopt;
      return hg::PowerStatus{true, 3850, 65, false, false};
    }
    bool power_off() override { ++shutdowns; return true; }
  } battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  Value status;
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK_EQ(status["power"]["battery_mv"].as_int(), int64_t(3850));
  r.advance(5000);
  CHECK_EQ((*r.fake.last("state"))["sensors"]["battery_percent"].as_int(), int64_t(65));
  battery.failed = true;
  r.advance(5000);
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK(!status["power"]["available"].as_bool());
  CHECK(status["power"]["battery_mv"].is_null());
  CHECK((*r.fake.last("state"))["sensors"]["battery_mv"].is_null());
  r.app.open_settings();
  for (int i = 0; i < 17; ++i) r.app.console("cancel");  // the Clock face item sits before Power off
  CHECK_EQ(r.app.model().detail, std::string("Power off"));
  r.app.console("talk"); r.app.console("release");
  CHECK_EQ(battery.shutdowns, 0);
  r.app.console("talk"); r.app.console("release");
  CHECK_EQ(battery.shutdowns, 1);
}

namespace {
// A gauge whose reading a test sets: on battery at 50% until it changes it.
struct AlertPower : hg::Power {
  hg::PowerStatus status{true, 3800, 50, false, false};
  bool failing = false;  // the next readings fail, as a driver's I2C read can
  std::optional<hg::PowerStatus> read() override {
    if (failing) return std::nullopt;
    return status;
  }
  std::optional<bool> external_power() override { return status.external_power; }
  bool power_off() override { return false; }
  void on_battery(uint8_t percent) { status.battery_percent = percent; status.charging = status.external_power = false; }
  void on_usb(uint8_t percent) { status.battery_percent = percent; status.charging = status.external_power = true; }
};
}  // namespace

TEST("battery alert: each level lights the dark screen once per discharge, and a press closes it and acts") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  // Waits in 5 s readings, with the server's heartbeat so the connection stays settled.
  auto wait = [&](uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 5000) {
      r.server(R"({"type":"ping"})");
      r.advance(std::min<uint32_t>(5000, ms - done));
    }
  };
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  battery.on_battery(8);
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);  // off by default
  CHECK_EQ(r.app.console("set battery_alert 5,10"), std::string("@ok battery_alert"));
  CHECK_EQ(r.fake.kv["battery_alert"], std::string("5,10"));
  battery.on_battery(12);
  wait(31000);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  battery.on_battery(10);
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::LowBattery);
  CHECK_EQ(r.app.model().headline, std::string("10%"));
  CHECK_EQ(r.app.model().detail, std::string("Low battery\nConnect USB to charge"));
  CHECK_EQ(int(r.app.model().face_battery), 10);
  CHECK(r.app.model().hint.empty());
  r.app.on_button(hg::Button::Talk, true);  // closes it, and the press talks as it would on Ready
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK(r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.app.on_button(hg::Button::Cancel, true);  // discards the recording
  r.app.on_button(hg::Button::Cancel, false);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.fake.mic_on);
  // A reading that wobbles across the level, or a jump past it, does not repeat it.
  for (uint8_t percent : {11, 10, 9, 11, 6}) {
    battery.on_battery(percent);
    wait(5000);
    CHECK_EQ(int(r.app.battery_alert()), 0);
  }
  battery.on_battery(5);
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 5);
  CHECK_EQ(r.app.model().headline, std::string("5%"));
  wait(10000);  // unanswered, it goes after 10 s and the screen times out as usual
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK_EQ(r.fake.brightness, 100);
  wait(31000);
  CHECK_EQ(r.fake.brightness, 0);
  battery.on_battery(3);
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK_EQ(r.fake.brightness, 0);
}

TEST("battery alert: never on USB, and only charging clear of a level rearms it") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  battery.on_usb(8);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  battery.status.external_power.reset();  // an unknown supply is no battery-only reading
  battery.status.charging.reset();
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  battery.on_battery(9);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  battery.on_usb(9);  // plugging in closes it
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  battery.on_usb(14);  // a short charge: still within 5 points of the level
  r.advance(5000);
  battery.on_battery(10);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  battery.on_usb(15);
  r.advance(5000);
  battery.on_battery(10);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
}

TEST("battery alert: waits for a quiet screen and gives way to a turn or a question") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  r.app.on_button(hg::Button::Talk, true);  // listening: the alert waits
  battery.on_battery(9);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  r.app.on_button(hg::Button::Talk, false);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  r.server(R"({"type":"prompt","id":"q","text":"Continue?"})");
  r.advance(10);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK(r.app.screen() == hg::Screen::Prompt);
}

TEST("battery alert: the power key closes it and darkens the screen as usual") {
  struct : AlertPower {
    int presses = 0;
    bool take_key_press() override {
      if (!presses) return false;
      --presses;
      return true;
    }
  } battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  battery.on_battery(10);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  battery.presses = 1;
  r.advance(200);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK_EQ(r.fake.brightness, 0);
}

TEST("battery alert: on a touchscreen a touch closes it and acts on the screen underneath") {
  AlertPower battery;
  Rig r(Rig::touch_profile());
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  battery.on_battery(10);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 200, r.fake.clock);  // touch-down closes it
  CHECK_EQ(int(r.app.battery_alert()), 0);
  r.advance(150);
  touch.tick(r.fake.clock);  // and a hold talks, as on Ready
  CHECK(r.fake.mic_on);
  touch.update(false, 0, 0, r.fake.clock);
}

TEST("battery alert: a touch that wakes a dark screen is only a wake, as before") {
  AlertPower battery;
  Rig r(Rig::touch_profile());
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  battery.on_battery(10);
  for (int i = 0; i < 10; ++i) {  // the alert goes after 10 s, and the screen sleeps 30 s later
    r.server(R"({"type":"ping"})");
    r.advance(5000);
  }
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK_EQ(r.fake.brightness, 0);
  hg::TouchGestures touch(r.app);
  touch.update(true, 200, 200, r.fake.clock);
  r.advance(150);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(!r.fake.mic_on);
}

TEST("battery alert: beeps by default, once with each alert, and battery_alert_sound off keeps it silent") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10,5"), std::string("@ok battery_alert"));
  const size_t before = r.fake.spk_samples;
  battery.on_battery(10);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  // 150 ms of lead-in silence, then 880 Hz 120 ms, 80 ms, 660 Hz 160 ms
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 360) / 1000));
  const size_t after_first = r.fake.spk_samples;
  r.advance(5000);
  CHECK_EQ(r.fake.spk_samples, after_first);  // once, not with each reading
  CHECK_EQ(r.app.console("set battery_alert_sound maybe"), std::string("@error battery_alert_sound must be on or off"));
  CHECK_EQ(r.app.console("set battery_alert_sound off"), std::string("@ok battery_alert_sound"));
  CHECK_EQ(r.fake.kv["batt_alert_snd"], std::string("off"));  // NVS keys stop at 15 characters
  r.app.on_button(hg::Button::Cancel, true);  // closes the first alert
  r.app.on_button(hg::Button::Cancel, false);
  battery.on_battery(5);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 5);
  CHECK_EQ(r.fake.spk_samples, after_first);
}

TEST("tones: each note lasts its time plus its gap, and starts and ends silent") {
  const hg::Tone tones[] = {{1000, 100, 50, 8000}, {500, 20, 0, 8000}};
  const auto pcm = hg::synth_tones(tones, 2, 16000);
  CHECK_EQ(pcm.size(), size_t(16000 * 170 / 1000));
  CHECK_EQ(pcm.front(), int16_t(0));
  int peak = 0;
  for (size_t i = 0; i < 1600; ++i) peak = std::max(peak, std::abs(int(pcm[i])));
  CHECK(peak > 7000 && peak <= 8000);
  for (size_t i = 1600; i < 2400; ++i) CHECK_EQ(pcm[i], int16_t(0));  // the gap
  // A lead-in: that much silence first, then the same notes.
  const auto led = hg::synth_tones(tones, 2, 16000, hg::kToneLeadMs);
  CHECK_EQ(led.size(), pcm.size() + size_t(16000 * 150 / 1000));
  for (size_t i = 0; i < 2400; ++i) CHECK_EQ(led[i], int16_t(0));
  CHECK(std::equal(pcm.begin(), pcm.end(), led.begin() + 2400));
}

TEST("battery alert: the indicator turns red at 15%, or at the highest alert level when higher") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_display percentage"), std::string("@ok battery_display"));
  battery.on_battery(16);
  r.advance(5000);
  CHECK(!r.app.model().battery_low);
  battery.on_battery(15);
  r.advance(5000);
  CHECK(r.app.model().battery_low);
  CHECK_EQ(r.app.console("set battery_alert 10,5"), std::string("@ok battery_alert"));
  CHECK(r.app.model().battery_low);  // lower levels keep 15%
  CHECK_EQ(r.app.console("set battery_alert 20,10"), std::string("@ok battery_alert"));
  r.app.on_button(hg::Button::Cancel, true);  // closes the alert at 20% that the new levels raise
  r.app.on_button(hg::Button::Cancel, false);
  battery.on_battery(20);
  r.advance(5000);
  CHECK(r.app.model().battery_low);
  battery.on_battery(21);
  r.advance(5000);
  CHECK(!r.app.model().battery_low);
  battery.on_usb(5);
  r.advance(5000);
  CHECK(!r.app.model().battery_low);  // never red on USB
}

TEST("charging notice: PWR that closes it leaves the reply it covered unread") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  battery.on_battery(42);
  r.bring_online(true);
  wait_quietly(r, 10000);
  r.server(R"({"type":"turn.start","turn":"t"})");
  r.server(R"({"type":"reply","turn":"t","text":"Here is the answer."})");
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  wait_quietly(r, 10000);  // the turn settles; the reply's text still shows
  CHECK(!r.app.model().hero);
  battery.on_usb(42);
  r.advance(5000);  // a lit screen reads the supply every few seconds
  CHECK(r.app.charging_notice());  // over the reply's text, which has 5 s left
  r.app.on_power_key();  // closes the notice and turns the screen off
  CHECK_EQ(r.fake.brightness, 0);
  wait_quietly(r, 30000);  // dark past the reply's 20 s
  r.app.on_power_key();  // the wake shows the reply the notice covered
  r.advance(200);
  CHECK(!r.app.model().hero);
  CHECK_EQ(r.app.model().body, std::string("Here is the answer."));
}

TEST("charging notice: plugging in lights the dark screen with the level, which then times out as usual") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  battery.on_usb(60);  // booting on USB shows nothing: no battery-only reading came first
  r.bring_online(true);
  auto wait = [&](uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 5000) {
      r.server(R"({"type":"ping"})");
      r.advance(std::min<uint32_t>(5000, ms - done));
    }
  };
  wait(5000);
  CHECK(!r.app.charging_notice());
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  battery.on_battery(42);
  wait(31000);
  CHECK_EQ(r.fake.brightness, 0);
  battery.on_usb(42);
  r.advance(200);  // a dark screen notices USB within the 100 ms supply poll
  CHECK(r.app.charging_notice());
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Charging);
  CHECK_EQ(r.app.model().headline, std::string("42%"));
  CHECK_EQ(r.app.model().detail, std::string("Charging"));
  CHECK_EQ(int(r.app.model().face_battery), 42);
  wait(5000);
  CHECK(!r.app.charging_notice());
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);  // lit until the normal timeout
  wait(26000);
  CHECK_EQ(r.fake.brightness, 0);
  battery.on_battery(42);  // unplugging shows nothing
  wait(5000);
  CHECK(!r.app.charging_notice());
  CHECK_EQ(r.fake.brightness, 0);
}

TEST("charging notice: replaces the low battery alert, a press closes it and acts, and no timeout darkens it back") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  battery.on_battery(9);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  battery.on_usb(9);
  r.advance(5000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK(r.app.charging_notice());
  r.app.on_button(hg::Button::Talk, true);  // closes it, and talks as on Ready
  CHECK(!r.app.charging_notice());
  CHECK(r.fake.mic_on);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  r.app.on_button(hg::Button::Talk, false);
  // No screen timeout: a screen the power key darkened goes dark again after the notice.
  struct : AlertPower {
    int presses = 0;
    bool take_key_press() override {
      if (!presses) return false;
      --presses;
      return true;
    }
  } keyed;
  Rig k;
  k.hal.power = &keyed;
  k.fake.backlight = true;
  k.bring_online(true);
  keyed.on_battery(50);
  k.advance(5000);
  keyed.presses = 1;
  k.advance(200);
  CHECK_EQ(k.fake.brightness, 0);
  keyed.on_usb(50);
  k.advance(200);
  CHECK(k.app.charging_notice());
  CHECK_EQ(k.fake.brightness, 100);
  k.advance(5000);
  CHECK(!k.app.charging_notice());
  CHECK_EQ(k.fake.brightness, 0);
}

TEST("charging notice: off leaves the screen dark, and the settings item toggles it") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set charging_notice maybe"), std::string("@error charging_notice must be on or off"));
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Charging screen"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Charging screen"));
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  CHECK_EQ(r.fake.kv["charging_notice"], std::string("off"));
  r.app.close_settings();
  battery.on_battery(50);
  r.advance(5000);
  battery.on_usb(50);
  r.advance(5000);
  CHECK(!r.app.charging_notice());
}

TEST("charging notice: silent by default; charging_sound on chirps once on plugging in") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  battery.on_battery(50);
  r.advance(5000);
  size_t before = r.fake.spk_samples;
  battery.on_usb(50);
  r.advance(5000);
  CHECK(r.app.charging_notice());
  CHECK_EQ(r.fake.spk_samples, before);
  CHECK_EQ(r.app.console("set charging_sound maybe"), std::string("@error charging_sound must be on or off"));
  CHECK_EQ(r.app.console("set charging_sound on"), std::string("@ok charging_sound"));
  battery.on_battery(50);
  r.advance(5000);
  before = r.fake.spk_samples;
  battery.on_usb(50);
  r.advance(5000);
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 240) / 1000));  // the lead-in, then 660 Hz 80 ms, 20 ms, 990 Hz 140 ms
  r.advance(5000);
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 240) / 1000));  // once per plug
}

TEST("full battery: once per charge, when charging ends or the gauge reaches 100%, with a chime") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set charged_sound on"), std::string("@ok charged_sound"));
  battery.on_battery(90);
  r.advance(5000);
  battery.on_usb(90);
  r.advance(5000);
  CHECK(r.app.charging_notice());
  r.advance(5000);
  CHECK(!r.app.charging_notice());
  size_t before = r.fake.spk_samples;
  battery.status.battery_percent = 100;
  battery.status.charging = false;  // charge done, still on USB
  r.advance(5000);
  CHECK(r.app.charged_notice());
  CHECK(r.app.screen() == hg::Screen::Charged);
  CHECK_EQ(r.app.model().headline, std::string("100%"));
  CHECK_EQ(r.app.model().detail, std::string("Battery full\nYou can unplug"));
  CHECK_EQ(int(r.app.model().face_battery), 100);
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 420) / 1000));  // the lead-in, then 784 Hz 140 ms, 40 ms, 1046 Hz 240 ms
  r.fake.spk_draining = true;  // its own chime still playing does not close it
  r.advance(1000);
  CHECK(r.app.charged_notice());
  r.fake.spk_draining = false;
  r.advance(4000);
  CHECK(!r.app.charged_notice());  // gone after 5 s
  battery.status.charging = true;  // the charger tops up and stops again: still the same charge
  r.advance(5000);
  battery.status.charging = false;
  r.advance(5000);
  CHECK(!r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 420) / 1000));
  // Unplugged and plugged back in full: nothing charged, nothing said.
  battery.on_battery(100);
  r.advance(5000);
  battery.on_usb(100);
  battery.status.charging = false;
  r.advance(5000);
  r.advance(5000);
  CHECK(!r.app.charged_notice());
  // A new charge whose gauge reaches 100% while still charging counts as full.
  battery.on_battery(80);
  r.advance(5000);
  battery.on_usb(80);
  r.advance(10000);
  battery.status.battery_percent = 100;
  r.advance(5000);
  CHECK(r.app.charged_notice());
}

TEST("full battery: the screen is on by default and the chime off, so a charge ending at night is silent") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  battery.on_usb(95);
  r.advance(5000);
  const size_t before = r.fake.spk_samples;
  battery.status.charging = false;
  r.advance(15000);  // three readings in a row: the charge has ended
  CHECK(r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples, before);
  r.advance(5000);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 30 && r.app.model().detail != "Full battery sound"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Full battery sound"));
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
}

TEST("battery alert: on a voltage estimate, a charger's high reading rearms nothing until the battery reads clear") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  auto wait = [&](uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 5000) {
      r.server(R"({"type":"ping"})");
      r.advance(std::min<uint32_t>(5000, ms - done));
    }
  };
  CHECK_EQ(r.app.console("set battery_estimate liion"), std::string("@ok battery_estimate"));
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  auto on_battery_mv = [&](uint16_t mv) {
    battery.status.battery_percent = std::nullopt;
    battery.status.battery_mv = mv;
    battery.status.charging = battery.status.external_power = false;
  };
  auto on_usb_mv = [&](uint16_t mv) {
    on_battery_mv(mv);
    battery.status.charging = battery.status.external_power = true;
  };
  on_battery_mv(3650);  // about 7%
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  wait(10000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  on_usb_mv(4200);  // the charger lifts the estimate to 100%
  wait(10000);
  on_battery_mv(3650);  // unplugged after a minute: the battery is where it was
  wait(10000);
  CHECK_EQ(int(r.app.battery_alert()), 0);  // still spent: the charger's reading rearmed nothing
  on_usb_mv(4200);
  wait(10000);
  on_battery_mv(3900);  // a real charge: about 63% on the battery
  wait(5000);
  on_battery_mv(3650);
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);  // rearmed once, by the battery-only reading
}

TEST("charging notice: a driver that reports only charging shows no plug-in when a charge stops and resumes") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  battery.status.battery_percent = 90;
  battery.status.external_power = std::nullopt;  // as the STC8 on the CrowPanel P4
  battery.status.charging = false;
  r.advance(5000);
  battery.status.charging = true;
  r.advance(5000);
  CHECK(!r.app.charging_notice());
  battery.status.charging = false;  // the charge ends with the cable in
  r.advance(15000);
  battery.status.charging = true;  // and tops up again
  r.advance(5000);
  CHECK(!r.app.charging_notice());
}

TEST("full battery: a failed reading keeps the charge, and the chime alone waits for a free speaker") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set charged_sound on"), std::string("@ok charged_sound"));
  battery.on_usb(95);
  r.advance(5000);
  battery.status.charging = false;
  r.advance(5000);
  battery.failing = true;  // a reading fails just as the charge ends
  r.advance(5000);
  battery.failing = false;
  r.advance(10000);
  CHECK(r.app.charged_notice());  // the charge so far was kept through the failed reading
  r.advance(5000);

  // With the screen notice off, the chime is the whole notice: a busy speaker holds it.
  CHECK_EQ(r.app.console("set charged_notice off"), std::string("@ok charged_notice"));
  battery.on_battery(95);
  r.advance(5000);
  battery.on_usb(95);
  r.advance(5000);
  const size_t before = r.fake.spk_samples;
  r.fake.spk_draining = true;  // something else is playing
  battery.status.charging = false;
  r.advance(15000);
  CHECK_EQ(r.fake.spk_samples, before);
  r.fake.spk_draining = false;
  r.advance(5000);
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 420) / 1000));  // played once it was free
  r.advance(10000);
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 420) / 1000));  // and only once
}

TEST("full battery: the screen and the chime are separate settings, and a busy screen waits") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set charged_notice maybe"), std::string("@error charged_notice must be on or off"));
  CHECK_EQ(r.app.console("set charged_sound on"), std::string("@ok charged_sound"));
  CHECK_EQ(r.app.console("set charged_notice off"), std::string("@ok charged_notice"));
  battery.on_usb(95);
  r.advance(5000);
  size_t before = r.fake.spk_samples;
  battery.status.charging = false;
  r.advance(15000);  // three readings in a row: the charge has ended
  CHECK(!r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 420) / 1000));  // the chime alone
  CHECK_EQ(r.app.console("set charged_notice on"), std::string("@ok charged_notice"));
  CHECK_EQ(r.app.console("set charged_sound off"), std::string("@ok charged_sound"));
  battery.on_battery(95);
  r.advance(5000);
  battery.on_usb(95);
  r.advance(6000);
  r.app.on_button(hg::Button::Talk, true);  // talking: the notice waits
  before = r.fake.spk_samples;
  battery.status.charging = false;
  r.advance(15000);
  CHECK(!r.app.charged_notice());
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  r.app.on_button(hg::Button::Talk, false);
  r.advance(5000);
  CHECK(r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples, before);  // charged_sound off
}

TEST("full battery: a charger still starting up, or flickering off for a reading, is no full battery") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set charged_sound on"), std::string("@ok charged_sound"));
  battery.on_battery(90);
  r.advance(5000);
  const size_t before = r.fake.spk_samples;
  // Plugged in at 90%: the first reading still says not charging, the next ones charging.
  battery.on_usb(90);
  battery.status.charging = false;
  r.advance(5000);
  CHECK(r.app.charging_notice());
  CHECK(!r.app.charged_notice());
  battery.status.charging = true;
  r.advance(10000);
  CHECK(!r.app.charged_notice());
  // One reading off, then charging again: still charging.
  battery.status.charging = false;
  r.advance(5000);
  battery.status.charging = true;
  r.advance(5000);
  CHECK(!r.app.charged_notice());
  battery.status.charging = false;  // two readings off: not yet
  r.advance(10000);
  CHECK(!r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples, before);  // no sound at any point so far
  r.advance(5000);  // the third in a row: the charge has ended
  CHECK(r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples - before, size_t(16000 * (150 + 420) / 1000));
}

TEST("power notices: every notice and sound is optional, from the settings and from the console") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  struct Item { const char* label; const char* key; const char* storage; const char* off; };
  const Item items[] = {
      {"Low battery alert", "battery_alert", "battery_alert", "off"},
      {"Low battery sound", "battery_alert_sound", "batt_alert_snd", "off"},
      {"Charging screen", "charging_notice", "charging_notice", "off"},
      {"Full battery screen", "charged_notice", "charged_notice", "off"},
      {"Full battery sound", "charged_sound", "charged_sound", "off"},
  };
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  for (const Item& item : items) {
    CHECK(r.app.open_settings());
    for (int i = 0; i < 30 && r.app.model().detail != item.label; ++i) r.app.on_button(hg::Button::Down, true);
    CHECK_EQ(r.app.model().detail, std::string(item.label));
    // Each select steps the item; one of the steps is off.
    bool reached_off = false;
    for (int i = 0; i < 6 && !reached_off; ++i) {
      r.app.on_button(hg::Button::Talk, true);
      r.app.on_button(hg::Button::Talk, false);
      reached_off = r.fake.kv[item.storage] == item.off;
    }
    CHECK(reached_off);
    r.app.close_settings();
    // The console turns each one off too (battery_alert takes levels rather than on).
    const std::string key = item.key;
    CHECK_EQ(r.app.console("set " + key + (key == "battery_alert" ? " 10" : " on")), "@ok " + key);
    CHECK_EQ(r.app.console("set " + key + " off"), "@ok " + key);
    CHECK_EQ(r.fake.kv[item.storage], std::string("off"));
  }
  // The charging sound is off by default, and its item turns it on and off again.
  CHECK(r.app.open_settings());
  for (int i = 0; i < 30 && r.app.model().detail != "Charging sound"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.kv["charging_sound"], std::string("on"));
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.kv["charging_sound"], std::string("off"));
  r.app.close_settings();
  // All off: a whole discharge and charge stays dark and silent.
  r.fake.backlight = true;
  const size_t before = r.fake.spk_samples;
  battery.on_battery(5);
  r.advance(5000);
  battery.on_usb(5);
  r.advance(5000);
  battery.status.battery_percent = 100;
  battery.status.charging = false;
  r.advance(20000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK(!r.app.charging_notice());
  CHECK(!r.app.charged_notice());
  CHECK_EQ(r.fake.spk_samples, before);
}

TEST("battery alert: the setting takes off or up to three levels, and the settings item steps through presets") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  for (const char* bad : {"0", "51", "10,10", "10,,5", "abc", "20,10,5,3", "10,"})
    CHECK_EQ(r.app.console(std::string("set battery_alert ") + bad),
             std::string("@error battery_alert must be off or up to 3 percentages from 1 to 50, like 10,5"));
  CHECK_EQ(r.app.console("set battery_alert 20,10,5"), std::string("@ok battery_alert"));
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Low battery alert"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Low battery alert"));
  CHECK(r.app.model().body.rfind("At 20%, 10%, 5%\n", 0) == 0);
  const char* expected[] = {"off", "10", "10,5", "15,10,5", "20,10", "off"};
  const char* shown[] = {"Off\n", "At 10%\n", "At 10%, 5%\n", "At 15%, 10%, 5%\n", "At 20%, 10%\n", "Off\n"};
  for (int i = 0; i < 6; ++i) {
    r.app.on_button(hg::Button::Talk, true);
    r.app.on_button(hg::Button::Talk, false);
    CHECK_EQ(r.fake.kv["battery_alert"], std::string(expected[i]));
    CHECK(r.app.model().body.rfind(shown[i], 0) == 0);
  }
  r.app.close_settings();
}

TEST("battery alert: the Low battery sound item turns the beep off and on") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Low battery sound"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Low battery sound"));
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  CHECK_EQ(r.fake.kv["batt_alert_snd"], std::string("off"));
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  r.app.close_settings();
}

TEST("battery indicator: the top band shows the level, colours charging and low, and hides without a reading") {
  for (bool round : {false, true}) {
    FakeHal display;
    if (round) display.make_round(466);
    hg::Ui ui(display);
    const int w = display.width;
    const int y0 = 0, y1 = round ? 68 : 22;  // round: the panel above the UI square; else the top bar
    const auto top = [&] {
      return std::vector<uint16_t>(display.fb.begin() + y0 * w, display.fb.begin() + y1 * w);
    };
    const auto count = [&](uint16_t color) {
      int n = 0;
      for (uint16_t px : top()) n += px == color;
      return n;
    };
    hg::UiModel m;
    m.screen = hg::Screen::Ready;
    m.link = hg::Link::Network;  // a yellow dot, so red and green below come from the battery
    ui.render(m);
    const auto none = top();
    const uint16_t red = hg::rgb565(242, 95, 92), green = hg::rgb565(61, 214, 140);
    m.battery = 65;
    ui.render(m);
    CHECK(top() != none);
    CHECK_EQ(count(red), 0);
    CHECK_EQ(count(green), 0);
    m.battery = 8;
    m.battery_low = true;
    ui.render(m);
    CHECK(count(red) > 0);  // low: red fill and text
    CHECK_EQ(count(green), 0);
    m.battery_charging = true;
    ui.render(m);
    CHECK(count(green) > 0);  // charging: green bolt, and the text is no longer red
    CHECK_EQ(count(red), 0);
    // The bolt shows however low the level: an almost empty cell still lights green.
    m.battery = 1;
    ui.render(m);
    CHECK(count(green) >= 10);
    m.battery = 8;
    // Percentage only: no icon, so charging turns the text green instead.
    m.battery_icon = false;
    ui.render(m);
    CHECK(count(green) > 0);
    const auto text_only = top();
    m.battery_icon = true;
    m.battery_text = false;
    ui.render(m);
    CHECK(top() != text_only);
    CHECK(count(green) > 0);  // icon only: the fill
    m.battery_text = true;
    m.battery = -1;
    m.battery_low = m.battery_charging = false;
    ui.render(m);
    CHECK(top() == none);
    if (round) {
      // Centred near the top edge, every lit pixel inside the circle.
      m.battery = 100;
      ui.render(m);
      int min_x = w, max_x = 0, min_y = y1;
      for (int y = 0; y < y1; ++y)
        for (int x = 0; x < w; ++x)
          if (display.fb[static_cast<size_t>(y * w + x)] != none[static_cast<size_t>(y * w + x)]) {
            min_x = std::min(min_x, x); max_x = std::max(max_x, x); min_y = std::min(min_y, y);
            const int dx = 2 * x + 1 - w, dy = 2 * y + 1 - w;
            CHECK(dx * dx + dy * dy < w * w);
          }
      CHECK(std::abs((min_x + max_x) / 2 - w / 2) <= 2);
      CHECK(min_y < 30);
      // The strip's top corners lie inside the band's inner edge (radius 219 on 466),
      // so it stays put as the edge ring comes and goes.
      for (int x : {min_x, max_x}) {
        const int dx = x - w / 2, dy = min_y - w / 2;
        CHECK(dx * dx + dy * dy <= 219 * 219);
      }
    }
  }
}

TEST("display: an emissive panel leaves its background unlit; a backlit one keeps near black") {
  for (bool emissive : {false, true}) {
    FakeHal display;
    display.make_round(466);
    display.emissive = emissive;
    hg::Ui ui(display);
    hg::UiModel m;
    m.screen = hg::Screen::Ready;
    m.hero = true;
    ui.render(m);
    const uint16_t corner = display.fb[0], centre_edge = display.fb[static_cast<size_t>(233 * 466 + 120)];
    CHECK_EQ(corner == 0, emissive);  // outside the UI square
    CHECK_EQ(centre_edge == 0, emissive);  // inside it, beside the mascot
  }
}

namespace {
struct EstimatePower : hg::Power {
  std::optional<hg::PowerStatus> status =
      hg::PowerStatus{std::nullopt, 3840, std::nullopt, false, std::nullopt};
  int reads = 0;
  std::optional<hg::PowerStatus> read() override { ++reads; return status; }
  bool power_off() override { return false; }
};
}  // namespace

TEST("battery estimate: voltage requires opt-in and selecting percentage produces text only") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK(r.fake.kv.find("batt_estimate") == r.fake.kv.end());
  CHECK_EQ(r.app.console("set battery_display percentage"), std::string("@ok battery_display"));
  CHECK_EQ(int(r.app.model().battery), -1);
  r.app.set_sensor("battery_pct", 87);  // demo telemetry is not the header power source
  r.advance(100);
  CHECK_EQ(int(r.app.model().battery), -1);
  const int reads = battery.reads;
  CHECK_EQ(r.app.console("set battery_estimate liion"), std::string("@ok battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK(!r.app.model().battery_icon);
  CHECK(r.app.model().battery_text);
  CHECK(!r.app.model().battery_low);  // unknown external power is not inferred to be battery-only
  CHECK_EQ(battery.reads, reads);  // activation uses the existing raw snapshot
  CHECK_EQ(r.fake.kv["battery_display"], std::string("percentage"));
  CHECK_EQ(r.fake.kv["batt_estimate"], std::string("liion"));
}

TEST("battery estimate: plausible limits, missing inputs and explicit absence control visibility") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();
  r.app.console("set battery_display percentage");
  r.app.console("set battery_estimate liion");
  struct Sample { std::optional<hg::PowerStatus> status; int percent; };
  for (const Sample& sample : {
           Sample{hg::PowerStatus{std::nullopt, 2000, std::nullopt, false, std::nullopt}, 0},
           Sample{hg::PowerStatus{std::nullopt, 3300, std::nullopt, false, std::nullopt}, 0},
           Sample{hg::PowerStatus{true, 4200, std::nullopt, false, std::nullopt}, 100},
           Sample{hg::PowerStatus{true, 4500, std::nullopt, false, std::nullopt}, 100},
           Sample{hg::PowerStatus{true, 1999, std::nullopt, false, false}, -1},
           Sample{hg::PowerStatus{true, 4501, std::nullopt, false, false}, -1},
           Sample{hg::PowerStatus{true, std::nullopt, std::nullopt, false, false}, -1},
           Sample{hg::PowerStatus{false, 3750, std::nullopt, false, true}, -1},
           Sample{hg::PowerStatus{false, 3750, 65, false, true}, -1},
           Sample{hg::PowerStatus{std::nullopt, 3840, std::nullopt, false, std::nullopt}, 50},
           Sample{std::nullopt, -1}}) {
    battery.status = sample.status;
    r.advance(5000);
    CHECK_EQ(int(r.app.model().battery), sample.percent);
    CHECK(!r.app.model().battery_charging);
    CHECK(!r.app.model().battery_low);
  }
}

TEST("battery estimate: voltage and charging change at normal five-second polls and failure clears them") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();  // establishes the first poll at the current fake time
  r.app.console("set battery_display percentage");
  r.app.console("set battery_estimate liion");
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK_EQ(battery.reads, 1);
  battery.status = hg::PowerStatus{std::nullopt, 4200, std::nullopt, true, std::nullopt};
  r.advance(4990);
  CHECK_EQ(battery.reads, 1);
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK(!r.app.model().battery_charging);
  r.advance(10);
  CHECK_EQ(battery.reads, 2);
  CHECK_EQ(int(r.app.model().battery), 100);
  CHECK(r.app.model().battery_charging);  // the endpoint does not mean charging has finished
  // An estimate reads high on a charger: the icon replaces the number there.
  CHECK(r.app.model().battery_icon);
  CHECK(!r.app.model().battery_text);
  battery.status = hg::PowerStatus{std::nullopt, 3300, std::nullopt, false, std::nullopt};
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), 0);
  CHECK(!r.app.model().battery_charging);
  CHECK(!r.app.model().battery_low);
  CHECK(r.app.model().battery_text);  // the chosen style returns off the charger
  CHECK(!r.app.model().battery_icon);
  battery.status->external_power = false;
  r.advance(5000);
  CHECK(r.app.model().battery_low);
  CHECK(r.app.model().battery_text);
  battery.status->external_power = true;
  r.advance(5000);
  CHECK(!r.app.model().battery_low);
  CHECK(!r.app.model().battery_text);  // external power alone counts as a charger
  CHECK(r.app.model().battery_icon);
  CHECK(r.app.model().battery_charging);  // full on USB, charger stopped: the bolt still shows
  battery.status.reset();
  r.advance(4990);
  CHECK_EQ(int(r.app.model().battery), 0);
  r.advance(10);
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK(!r.app.model().battery_charging);
  CHECK(!r.app.model().battery_low);
}

TEST("battery estimate: native percentages including zero take precedence without requiring voltage") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();
  r.app.console("set battery_display percentage");
  r.app.console("set battery_estimate liion");
  for (uint8_t percent : {uint8_t(0), uint8_t(9), uint8_t(100)}) {
    battery.status = hg::PowerStatus{std::nullopt, 3750, percent, true, std::nullopt};
    r.advance(5000);
    CHECK_EQ(int(r.app.model().battery), int(percent));
    CHECK(r.app.model().battery_charging);
    battery.status->battery_mv.reset();
    r.advance(5000);
    CHECK_EQ(int(r.app.model().battery), int(percent));
  }
  const int reads = battery.reads;
  CHECK_EQ(r.app.console("set battery_estimate off"), std::string("@ok battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), 100);
  CHECK_EQ(r.app.console("set battery_estimate"), std::string("@ok battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), 100);
  CHECK_EQ(battery.reads, reads);
}

TEST("battery estimate: disabling and clearing hide only the derived value immediately") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();
  r.app.console("set battery_display percentage");
  r.app.console("set battery_estimate liion");
  const int reads = battery.reads;
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK_EQ(r.app.console("set battery_estimate off"), std::string("@ok battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK_EQ(r.fake.kv["batt_estimate"], std::string("off"));
  r.app.console("set battery_estimate liion");
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK_EQ(r.app.console("set battery_estimate"), std::string("@ok battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK(r.fake.kv.find("batt_estimate") == r.fake.kv.end());
  r.app.console("set battery_estimate liion");
  CHECK_EQ(int(r.app.model().battery), 50);
  r.app.console("set battery_display off");
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK_EQ(r.fake.kv["batt_estimate"], std::string("liion"));
  CHECK_EQ(battery.reads, reads);
}

TEST("battery estimate: both settings survive a fresh App and factory reset removes the opt-in") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();
  r.app.console("set battery_estimate liion");
  CHECK_EQ(int(r.app.model().battery), -1);  // estimation alone does not turn on the indicator
  r.app.console("set battery_display percentage");
  CHECK_EQ(int(r.app.model().battery), 50);
  Rig reboot;
  reboot.fake.kv = r.fake.kv;
  reboot.hal.power = &battery;
  reboot.app.begin();
  reboot.app.tick();
  CHECK_EQ(int(reboot.app.model().battery), 50);
  CHECK(!reboot.app.model().battery_icon && reboot.app.model().battery_text);
  CHECK(reboot.app.console("help").find("battery_estimate") != std::string::npos);
  Value setting;
  const std::string response = reboot.app.console("get battery_estimate");
  CHECK(response.rfind("@value ", 0) == 0);
  CHECK(hg::json::parse(response.substr(7), setting));
  CHECK_EQ(setting["value"].as_string(), std::string("liion"));
  reboot.app.console("factory-reset");
  CHECK(reboot.fake.kv.find("batt_estimate") == reboot.fake.kv.end());
  CHECK(reboot.fake.kv.find("battery_display") == reboot.fake.kv.end());
  Rig reset;
  reset.fake.kv = reboot.fake.kv;
  reset.hal.power = &battery;
  reset.app.begin();
  reset.app.tick();
  reset.app.console("set battery_display percentage");
  CHECK_EQ(int(reset.app.model().battery), -1);
}

TEST("battery estimate: invalid saved values and rejected writes never activate it") {
  for (const std::string saved : {"", "off", "unknown", "LIION"}) {
    EstimatePower battery;
    Rig r;
    r.hal.power = &battery;
    r.fake.kv["battery_display"] = "percentage";
    r.fake.kv["batt_estimate"] = saved;
    r.app.begin();
    r.app.tick();
    CHECK_EQ(int(r.app.model().battery), -1);
  }
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.app.begin();
  r.app.tick();
  r.app.console("set battery_display percentage");
  for (const char* value : {"on", "LIION", "true", "liion extra"}) {
    CHECK_EQ(r.app.console(std::string("set battery_estimate ") + value),
             std::string("@error battery_estimate must be off or liion"));
    CHECK_EQ(int(r.app.model().battery), -1);
    CHECK(r.fake.kv.find("batt_estimate") == r.fake.kv.end());
  }
  r.fake.storage_full = true;
  CHECK_EQ(r.app.console("set battery_estimate liion"), std::string("@error could not save battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK(r.fake.kv.find("batt_estimate") == r.fake.kv.end());
  r.fake.storage_full = false;
  r.app.console("set battery_estimate liion");
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK_EQ(r.app.console("set battery_estimate unknown"), std::string("@error battery_estimate must be off or liion"));
  CHECK_EQ(int(r.app.model().battery), 50);
  r.fake.storage_full = true;
  CHECK_EQ(r.app.console("set battery_estimate off"), std::string("@error could not save battery_estimate"));
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK_EQ(r.fake.kv["batt_estimate"], std::string("liion"));
}

TEST("battery estimate: Power-page selection uses the display value and changes only indicator style") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  r.app.console("set battery_estimate liion");
  CHECK(r.app.open_settings());
  for (int i = 0; i < 8; ++i) r.app.console("cancel");
  r.app.console("talk"); r.app.console("release");
  CHECK_EQ(r.fake.kv["battery_display"], std::string("icon"));
  CHECK_EQ(int(r.app.model().battery), 50);
  CHECK(r.app.model().battery_icon && !r.app.model().battery_text);
  r.app.console("talk"); r.app.console("release");
  CHECK_EQ(r.fake.kv["battery_display"], std::string("percentage"));
  CHECK(!r.app.model().battery_icon && r.app.model().battery_text);
  CHECK_EQ(r.fake.kv["batt_estimate"], std::string("liion"));
}

TEST("battery estimate: raw status and gateway telemetry remain unestimated, including unavailable transitions") {
  EstimatePower battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  r.app.console("set battery_display percentage");
  r.app.console("set battery_estimate liion");
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), 50);
  Value status;
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK(status["power"]["available"].as_bool());
  CHECK_EQ(status["power"]["battery_mv"].as_int(), int64_t(3840));
  CHECK(status["power"]["battery_percent"].is_null());
  CHECK(status["power"]["battery_present"].is_null());
  CHECK(status["power"]["external_power"].is_null());
  CHECK(status["power"]["charging"].is_bool() && !status["power"]["charging"].as_bool());
  const Value* state = r.fake.last("state");
  CHECK(state != nullptr);
  if (!state) return;
  CHECK_EQ((*state)["sensors"]["power_available"].as_int(), int64_t(1));
  CHECK_EQ((*state)["sensors"]["battery_mv"].as_int(), int64_t(3840));
  CHECK((*state)["sensors"]["battery_percent"].is_null());
  CHECK((*state)["sensors"]["battery_present"].is_null());
  CHECK((*state)["sensors"]["external_power"].is_null());
  CHECK_EQ((*state)["sensors"]["charging"].as_int(), int64_t(0));
  const std::string ready_detail = r.app.model().detail;
  battery.status = hg::PowerStatus{std::nullopt, 3300, std::nullopt, false, false};
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), 0);
  CHECK(r.app.model().battery_low);
  CHECK_EQ(r.app.model().detail, ready_detail);  // a display estimate does not create a battery alarm
  battery.status = hg::PowerStatus{true, 4200, 0, true, true};
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), 0);
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK(status["power"]["battery_percent"].is_number());
  CHECK_EQ(status["power"]["battery_percent"].as_int(), int64_t(0));
  state = r.fake.last("state");
  CHECK(state && (*state)["sensors"]["battery_percent"].is_number());
  CHECK(state && (*state)["sensors"]["battery_percent"].as_int() == 0);
  battery.status.reset();
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), -1);
  CHECK(hg::json::parse(r.app.status_json(), status));
  CHECK(!status["power"]["available"].as_bool());
  CHECK(status["power"]["battery_mv"].is_null());
  CHECK(status["power"]["battery_percent"].is_null());
  state = r.fake.last("state");
  CHECK(state && (*state)["sensors"]["power_available"].is_number());
  CHECK(state && (*state)["sensors"]["power_available"].as_int() == 0);
  CHECK(state && (*state)["sensors"]["battery_mv"].is_null());
  CHECK(state && (*state)["sensors"]["battery_percent"].is_null());
}

TEST("battery indicator: off by default, follows readings once chosen, and each select picks the next style") {
  struct Battery : hg::Power {
    hg::PowerStatus status{true, 3850, 65, false, false};
    std::optional<hg::PowerStatus> read() override { return status; }
    bool power_off() override { return true; }
  } battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  CHECK_EQ(int(r.app.model().battery), -1);  // off until the user picks a style
  CHECK(r.fake.kv.find("battery_display") == r.fake.kv.end());
  CHECK_EQ(r.app.console("set battery_display both"), std::string("@ok battery_display"));
  CHECK_EQ(int(r.app.model().battery), 65);
  CHECK(r.app.model().battery_icon && r.app.model().battery_text);
  CHECK(!r.app.model().battery_low);
  battery.status = hg::PowerStatus{true, 3400, 7, false, false};
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), 7);
  CHECK(r.app.model().battery_low);
  battery.status = hg::PowerStatus{true, 3400, 7, true, true};  // on USB and charging: no longer low
  r.advance(5000);
  CHECK(r.app.model().battery_charging);
  CHECK(!r.app.model().battery_low);
  battery.status = hg::PowerStatus{false, std::nullopt, 0, false, true};  // no battery fitted
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), -1);
  battery.status = hg::PowerStatus{true, 3850, 65, false, false};
  r.advance(5000);
  CHECK_EQ(int(r.app.model().battery), 65);
  CHECK_EQ(r.app.console("set battery_display maybe"),
           std::string("@error battery_display must be icon, percentage, both or off"));
  CHECK_EQ(r.app.console("set battery_display off"), std::string("@ok battery_display"));
  CHECK_EQ(r.fake.kv["battery_display"], std::string("off"));
  CHECK_EQ(int(r.app.model().battery), -1);
  // Each select on the battery page moves to the next style, saves it, and stays on the page.
  r.app.open_settings();
  for (int i = 0; i < 8; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Battery and power"));
  CHECK(r.app.model().body.find("Indicator: Off\nTALK to change") != std::string::npos);  // a button board
  struct Step { const char* saved; const char* shown; bool icon, text; };
  for (const Step& step : {Step{"icon", "Icon", true, false}, Step{"percentage", "Percent", false, true},
                           Step{"both", "Both", true, true}, Step{"off", "Off", true, true},
                           Step{"icon", "Icon", true, false}}) {
    r.app.console("talk"); r.app.console("release");
    CHECK_EQ(r.fake.kv["battery_display"], std::string(step.saved));
    CHECK(r.app.model().body.find(std::string("Indicator: ") + step.shown + "\n") != std::string::npos);
    CHECK_EQ(r.app.model().detail, std::string("Battery and power"));
    CHECK_EQ(int(r.app.model().battery), std::string(step.saved) == "off" ? -1 : 65);
    CHECK_EQ(r.app.model().battery_icon, step.icon);
    CHECK_EQ(r.app.model().battery_text, step.text);
  }
  r.app.console("settings close");
  r.app.console("set battery_display both");
  CHECK_EQ(int(r.app.model().battery), 65);
}

TEST("battery indicator: the battery page offers it only when a percentage can appear") {
  struct Battery : hg::Power {
    std::optional<hg::PowerStatus> status;
    std::optional<hg::PowerStatus> read() override { return status; }
    bool power_off() override { return true; }
  } battery;
  Rig r;
  r.hal.power = &battery;
  r.bring_online(true);
  r.app.open_settings();
  for (int i = 0; i < 8; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Battery and power"));
  for (const auto& reading : {std::optional<hg::PowerStatus>{},                                      // no readings
                              std::optional<hg::PowerStatus>{hg::PowerStatus{false, std::nullopt, 0, false, true}},  // no battery
                              std::optional<hg::PowerStatus>{hg::PowerStatus{true, 3850, std::nullopt, false, false}}}) {  // no percentage
    battery.status = reading;
    r.advance(5000);
    CHECK(r.app.model().body.find("Indicator") == std::string::npos);
    r.app.console("talk"); r.app.console("release");
    CHECK(r.fake.kv.find("battery_display") == r.fake.kv.end());  // a select changes nothing
    CHECK_EQ(r.app.model().detail, std::string("Battery and power"));
  }
  battery.status = hg::PowerStatus{true, 3850, 65, false, false};
  r.advance(5000);
  CHECK(r.app.model().body.find("Indicator: Off") != std::string::npos);
  r.app.console("talk"); r.app.console("release");
  CHECK_EQ(r.fake.kv["battery_display"], std::string("icon"));
}

TEST("battery indicator: touch screens say tap") {
  struct Battery : hg::Power {
    std::optional<hg::PowerStatus> read() override { return hg::PowerStatus{true, 3850, 65, false, false}; }
    bool power_off() override { return true; }
  } battery;
  Rig r(Rig::touch_profile());
  r.hal.power = &battery;
  r.bring_online(true);
  r.app.open_settings();
  for (int i = 0; i < 8; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Battery and power"));
  CHECK(r.app.model().body.find("Indicator: Off\nTap to change") != std::string::npos);
}

TEST("battery indicator: boards without a power driver have no battery page") {
  Rig r;
  r.bring_online(true);
  r.app.open_settings();
  for (int i = 0; i < 20; ++i) {
    CHECK(r.app.model().detail != "Battery and power");
    r.app.console("cancel");
  }
  CHECK_EQ(int(r.app.model().battery), -1);
}

TEST("power: a peripheral rail cannot be selected as device power-off") {
  struct PeripheralRail : hg::Power {
    int shutdowns = 0;
    std::optional<hg::PowerStatus> read() override { return hg::PowerStatus{}; }
    bool can_power_off() const override { return false; }
    bool power_off() override { ++shutdowns; return true; }
  } rail;
  Rig r;
  r.hal.power = &rail;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 10; ++i) r.app.console("cancel");
  CHECK(r.app.model().detail != "Power off");
  r.app.console("talk"); r.app.console("release");
  r.app.console("talk"); r.app.console("release");
  CHECK_EQ(rail.shutdowns, 0);
}

TEST("Wi-Fi setup: bounded credentials require the current session and never replace valid output on failure") {
  hg::WifiCredentials out;
  std::string error;
  const std::string body = R"({"nonce":"current","ssid":"Kitchen","password":"example pass","server":"ws://192.168.1.20:8765/gadget"})";
  CHECK(hg::parse_wifi_setup(body, "current", out, error));
  CHECK_EQ(std::string(out.ssid), std::string("Kitchen"));
  CHECK_EQ(std::string(out.password), std::string("example pass"));
  CHECK_EQ(std::string(out.server), std::string("ws://192.168.1.20:8765/gadget"));
  CHECK(!hg::parse_wifi_setup(body, "expired", out, error));
  CHECK(!hg::parse_wifi_setup(body, "", out, error));
  for (const std::string& url : {"http://example.com", "ws://", "ws://host:0", "ws://host:65536",
                                 "ws://user:pass@host", "ws://host/#secret", "ws://host\n/path"}) {
    Value form;
    CHECK(hg::json::parse(body, form));
    form.set("server", url);
    CHECK(!hg::parse_wifi_setup(form.dump(), "current", out, error));
  }
  Value form;
  CHECK(hg::json::parse(body, form));
  form.set("ssid", std::string(33, 'x'));
  CHECK(!hg::parse_wifi_setup(form.dump(), "current", out, error));
  form.set("ssid", std::string("x\0y", 3));
  CHECK(!hg::parse_wifi_setup(form.dump(), "current", out, error));
  CHECK_EQ(std::string(out.ssid), std::string("Kitchen"));
  form.set("ssid", std::string(32, 'x')).set("password", "").set("server", "wss://[::1]:8765/gadget");
  CHECK(hg::parse_wifi_setup(form.dump(), "current", out, error));
  CHECK_EQ(std::string(out.password), std::string());
  form.set("password", "short");
  CHECK(!hg::parse_wifi_setup(form.dump(), "current", out, error));
  form.set("password", std::string(64, 'a'));
  CHECK(hg::parse_wifi_setup(form.dump(), "current", out, error));
  form.set("password", std::string(64, 'z'));
  CHECK(!hg::parse_wifi_setup(form.dump(), "current", out, error));
  CHECK(!hg::parse_wifi_setup(std::string(1025, ' '), "current", out, error));
}

TEST("Wi-Fi setup: the tailnet fields are optional and the auth key is checked") {
  hg::WifiCredentials out;
  std::string error;
  const std::string body = R"({"nonce":"n","ssid":"Phone","password":"example pass","server":"ws://100.71.189.23:8765/gadget"})";
  CHECK(hg::parse_wifi_setup(body, "n", out, error));
  CHECK(!out.tailnet_given);  // a page without the tailnet section changes nothing
  CHECK_EQ(std::string(out.ts_key), std::string());

  Value form;
  CHECK(hg::json::parse(body, form));
  form.set("tailnet", true).set("ts_key", "tskey-auth-kTEST123-abcdefghijklmnop");
  CHECK(hg::parse_wifi_setup(form.dump(), "n", out, error));
  CHECK(out.tailnet_given && out.tailnet_on);
  CHECK_EQ(std::string(out.ts_key), std::string("tskey-auth-kTEST123-abcdefghijklmnop"));

  form.set("tailnet", false).set("ts_key", "");  // off, keeping whatever key is saved
  CHECK(hg::parse_wifi_setup(form.dump(), "n", out, error));
  CHECK(out.tailnet_given && !out.tailnet_on);
  CHECK_EQ(std::string(out.ts_key), std::string());

  hg::WifiCredentials kept;
  CHECK(hg::parse_wifi_setup(body, "n", kept, error));
  for (const std::string& bad : {std::string("hunter2"), std::string("tskey-auth-x y"), std::string("tskey-") + std::string(120, 'a')}) {
    form.set("ts_key", bad);
    CHECK(!hg::parse_wifi_setup(form.dump(), "n", kept, error));
    CHECK_EQ(error, std::string("Enter a Tailscale auth key (tskey-...), or leave it empty"));
  }
  form.set("ts_key", "").set("tailnet", "yes");
  CHECK(!hg::parse_wifi_setup(form.dump(), "n", kept, error));
  CHECK_EQ(std::string(kept.server), std::string("ws://100.71.189.23:8765/gadget"));  // untouched on failure
}

TEST("Wi-Fi setup: private instructions stay out of diagnostics and prompts close the temporary network") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  int closed = 0;
  r.app.on_wifi_setup = [] { return "Network: Hermes-test\nPassword: private-setup-key"; };
  r.app.on_wifi_setup_close = [&] { ++closed; };
  CHECK(r.app.start_wifi_setup());
  CHECK(r.app.screen() == hg::Screen::Setup);
  CHECK(r.app.model().body.find("private-setup-key") != std::string::npos);
  CHECK(r.app.console("diag").find("private-setup-key") == std::string::npos);
  CHECK(r.app.status_json().find("private-setup-key") == std::string::npos);
  r.app.console("talk"); r.app.console("release");
  CHECK(!r.fake.mic_on);
  hg::TouchGestures touch(r.app);
  touch.set_swipe_cancel(false);
  touch.update(true, 100, 50, r.fake.clock);
  touch.update(true, 100, 150, r.fake.clock + 30);
  touch.update(false, 0, 0, r.fake.clock + 40);
  CHECK_EQ(closed, 1);
  CHECK(!r.app.wifi_setup_open());
  CHECK(r.app.start_wifi_setup());
  r.server(R"({"type":"prompt","id":"setup-test","text":"Continue?"})");
  CHECK_EQ(closed, 2);
  CHECK(!r.app.wifi_setup_open());
  CHECK(r.app.screen() == hg::Screen::Prompt);
  CHECK(!r.app.start_wifi_setup());
}

TEST("Wi-Fi setup: the screen carries a scannable code for the temporary network") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  r.app.on_wifi_setup = [] { return "Network: Hermes-test\nPassword: private-setup-key"; };
  r.app.on_wifi_setup_ap = [] {
    hg::WifiSetupAp ap;
    ap.ssid = "Hermes-test";
    ap.password = "private-setup-key";
    return ap;
  };
  CHECK(r.app.start_wifi_setup());
  CHECK(r.app.screen() == hg::Screen::Setup);
  const hg::qr::Code* code = r.app.model().qr;
  CHECK(code != nullptr && code->ok());
  // The code encodes exactly the credentials the screen prints.
  if (code) {
    CHECK_EQ(code->size, hg::qr::encode(hg::qr::payload("Hermes-test", "private-setup-key")).size);
  }
  // The credentials still reach neither diagnostics nor status.
  CHECK(r.app.console("diag").find("private-setup-key") == std::string::npos);
  CHECK(r.app.status_json().find("private-setup-key") == std::string::npos);
  // Closing the screen drops the code.
  r.app.close_wifi_setup();
  CHECK(r.app.model().qr == nullptr);
}

TEST("Wi-Fi setup: a port without credentials still shows the text screen") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  r.app.on_wifi_setup = [] { return "Temporary setup network"; };
  // No on_wifi_setup_ap: the text screen must still work, just without a code.
  CHECK(r.app.start_wifi_setup());
  CHECK(r.app.screen() == hg::Screen::Setup);
  CHECK(r.app.model().qr == nullptr);
  CHECK(r.app.model().body.find("Temporary setup network") != std::string::npos);
}

TEST("Wi-Fi setup: the code is drawn inside the content band, dark on light") {
  Rig r(Rig::touch_profile());
  r.fake.make_round(466);
  r.bring_online(true);
  r.app.on_wifi_setup = [] { return "Network: Hermes-test\nPassword: private-setup-key"; };
  r.app.on_wifi_setup_ap = [] {
    hg::WifiSetupAp ap;
    ap.ssid = "Hermes-test";
    ap.password = "private-setup-key";
    return ap;
  };
  CHECK(r.app.start_wifi_setup());
  r.advance(200);
  const hg::qr::Code* code = r.app.model().qr;
  CHECK(code != nullptr);
  if (!code) return;
  // The code is drawn: count pure-black and pure-white pixels. The renderer
  // draws modules as black on a white panel, which a camera needs.
  int black = 0, white = 0;
  for (int y = 0; y < 466; ++y) {
    for (int x = 0; x < 466; ++x) {
      uint16_t p = r.fake.fb[static_cast<size_t>(y * 466 + x)];
      if (p == 0x0000) ++black;
      if (p == 0xFFFF) ++white;
    }
  }
  CHECK(black > 100);        // the modules
  CHECK(white > black / 2);  // the panel and quiet zone around them
}

// The setup screen as the ESP32 port fills it, rendered with or without a code.
static std::vector<uint16_t> setup_screen(int width, int height, bool round, const hg::qr::Code* code) {
  FakeHal display;
  if (round) {
    display.make_round(width);
  } else {
    display.width = width;
    display.height = height;
    display.fb.assign(static_cast<size_t>(width * height), 0);
  }
  hg::Ui ui(display);
  hg::UiModel m;
  m.screen = hg::Screen::Setup;
  m.headline = "Wi-Fi setup";
  m.detail = "Connect your phone";
  m.body = "Network: Hermes-A1B2\nPassword: 3f9c2a1b4d5e6f70\nOpen http://192.168.4.1\nAvailable for 10 minutes.";
  m.qr = code;
  ui.render(m);
  return display.fb;
}

TEST("Wi-Fi setup: a code that does not fit beside the instructions leaves the text screen") {
  const hg::qr::Code code = hg::qr::encode(hg::qr::payload("Hermes-A1B2", "3f9c2a1b4d5e6f70"));
  // 128x128 (AIPI Lite) and 320x240 (BOX-3, CoreS3): the instructions take the room
  // a scannable code needs, so the screen is the text one, pixel for pixel.
  CHECK(setup_screen(128, 128, false, &code) == setup_screen(128, 128, false, nullptr));
  CHECK(setup_screen(320, 240, false, &code) == setup_screen(320, 240, false, nullptr));
  // 240x240 and the 466 round panel fit the code beside every instruction line.
  CHECK(setup_screen(240, 240, false, &code) != setup_screen(240, 240, false, nullptr));
  CHECK(setup_screen(466, 466, true, &code) != setup_screen(466, 466, true, nullptr));
}

// A banded framebuffer (a board with no PSRAM) must put the same pixels on the
// panel as a whole-panel one. Both rigs run the same session; after each step the
// rows each display has flushed are compared.
static void wifi_setup_with_code(Rig& r) {
  r.app.on_wifi_setup = [] { return "Network: Hermes-test\nPassword: private-setup-key"; };
  r.app.on_wifi_setup_ap = [] {
    hg::WifiSetupAp ap;
    ap.ssid = "Hermes-test";
    ap.password = "private-setup-key";
    return ap;
  };
  CHECK(r.app.start_wifi_setup());
}

static void same_screens_banded(int width, int height, bool round, int band_rows) {
  Rig full(Rig::touch_profile()), banded(Rig::touch_profile());
  for (Rig* r : {&full, &banded}) {
    if (round) r->fake.make_round(width);
    else r->fake.make_panel(width, height);
    r->fake.keep_panel = true;
  }
  banded.fake.make_banded(band_rows);
  auto both = [&](void (*act)(Rig&), const char* what) {
    act(full);
    act(banded);
    const bool same = full.fake.panel == banded.fake.panel;
    if (!same)
      std::printf("  %dx%d%s in %d-row bands: the panels differ after %s\n", width, height, round ? " round" : "",
                  band_rows, what);
    CHECK(same);
    CHECK(full.app.screen() == banded.app.screen());
  };
  both([](Rig& r) { r.bring_online(true); r.advance(300); }, "ready");
  CHECK(banded.app.screen() == hg::Screen::Ready);
  both([](Rig& r) { r.app.on_button(hg::Button::Talk, true); r.advance(2000); }, "listening");
  both([](Rig& r) { r.app.on_button(hg::Button::Talk, false); r.advance(300); }, "thinking");
  CHECK(banded.app.screen() == hg::Screen::Thinking);  // the blue comet repaints the bands
  both([](Rig& r) {
    r.server(R"({"type":"turn.start","turn":"a1"})");
    r.server(R"({"type":"audio.start","stream":5,"rate":16000,"format":"pcm16"})");
    r.advance(300);
  }, "speaking");
  CHECK(banded.app.screen() == hg::Screen::Responding);  // the amber comet, one lap in 4 s
  both([](Rig& r) {
    r.server(R"({"type":"reply","turn":"a1","text":"A reply long enough to wrap over several lines, so the content band fills up."})");
    r.server(R"({"type":"audio.end","stream":5})");
    r.server(R"({"type":"turn.end","turn":"a1","outcome":"success"})");
    r.advance(300);
  }, "a reply");
  both([](Rig& r) {
    r.server(R"({"type":"prompt","id":"q1","title":"Allow command?","text":"rm -rf build"})");
    r.advance(300);
  }, "a prompt");
  CHECK(banded.app.screen() == hg::Screen::Prompt);
  both([](Rig& r) {
    r.server(R"({"type":"prompt.close","id":"q1"})");
    r.advance(300);
  }, "the prompt closing");
  CHECK(banded.app.screen() != hg::Screen::Prompt);
  both([](Rig& r) {
    r.server(R"({"type":"action","id":"t1","name":"timer.start","args":{"seconds":3,"label":"Pasta"}})");
    r.advance(1500);
  }, "a running timer");
  both([](Rig& r) { r.advance(1700); }, "the alarm");
  CHECK(banded.app.screen() == hg::Screen::Alarm);
  both([](Rig& r) { r.advance(400); }, "the alarm flashing");
  both([](Rig& r) {
    r.app.on_button(hg::Button::Talk, true);  // dismisses it
    r.app.on_button(hg::Button::Talk, false);
    r.advance(300);
  }, "the alarm dismissed");
  CHECK(banded.app.screen() != hg::Screen::Alarm);
  both([](Rig& r) {
    r.fake.wall = hg::WallTime{2026, 10, 24, 6, 9, 41, 0};
    r.fake.wall_ok = true;
    r.app.console("set clock 5");
    r.advance(40000);  // the reply's linger runs out, then the 5 s rest
  }, "the clock");
  CHECK(banded.app.screen() == hg::Screen::Clock);
  both([](Rig& r) {
    r.server(R"({"type":"action","id":"t2","name":"timer.start","args":{"seconds":1}})");
    r.advance(1200);  // the timer takes the screen back from the clock
  }, "the timer over the clock");
  CHECK(banded.app.screen() != hg::Screen::Clock);
  both([](Rig& r) { r.advance(400); }, "the alarm over the clock");
  CHECK(banded.app.screen() == hg::Screen::Alarm);
  both([](Rig& r) {
    r.app.on_button(hg::Button::Talk, true);
    r.app.on_button(hg::Button::Talk, false);
    r.advance(300);
  }, "the alarm dismissed again");
  CHECK(banded.app.screen() != hg::Screen::Alarm);
  both([](Rig& r) { r.advance(6000); }, "the clock again");
  CHECK(banded.app.screen() == hg::Screen::Clock);  // the same minute: the face must repaint
  both([](Rig& r) {
    r.server(R"({"type":"notice","text":"Back soon","ttl_s":30})");
    r.advance(300);
  }, "the clock leaving");
  CHECK(banded.app.screen() != hg::Screen::Clock);
  both([](Rig& r) { wifi_setup_with_code(r); r.advance(300); }, "Wi-Fi setup");
  CHECK(banded.app.screen() == hg::Screen::Setup);
  CHECK_EQ(full.fake.stray_flushes, 0);
  CHECK_EQ(banded.fake.stray_flushes, 0);
  // Something was drawn, so equal panels mean something.
  int lit = 0;
  for (uint16_t p : banded.fake.panel) lit += p != banded.fake.panel[0];
  CHECK(lit > 1000);
}

TEST("display: a banded framebuffer shows the same screens as a whole-panel one") {
  same_screens_banded(320, 240, false, 120);  // the CYD: two windows
  same_screens_banded(320, 240, false, 70);   // windows that cut through bands, the last one short
  same_screens_banded(240, 240, false, 100);  // room for the Wi-Fi code beside the instructions
  same_screens_banded(466, 466, true, 120);   // round: the UI square starts inside a window
}

TEST("display: the Wi-Fi code reaches the panel from a banded framebuffer") {
  Rig r(Rig::touch_profile());
  r.fake.make_panel(240, 240);
  r.fake.make_banded(100);
  r.bring_online(true);
  wifi_setup_with_code(r);
  r.advance(300);
  CHECK(r.app.model().qr != nullptr);
  int black = 0;
  for (uint16_t p : r.fake.panel) black += p == 0x0000;
  CHECK(black > 100);  // the modules
}

TEST("display: a banded framebuffer doesn't offer images, and refuses one sent anyway") {
  Rig full, banded;
  banded.fake.make_banded(120);
  full.bring_online(true);
  banded.bring_online(true);
  const Value* hello = full.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["display"]["image"]["format"].as_string() == "rgb565");
  hello = banded.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["display"].is_object());
  CHECK(hello && !(*hello)["caps"]["display"]["image"].is_object());
  CHECK(hello && (*hello)["caps"]["display"]["text_cols"].as_int() > 0);  // text still is offered
  banded.server(R"({"type":"image.start","stream":3,"width":64,"height":64,"format":"rgb565"})");
  bool refused = false;
  for (const auto& l : banded.fake.logs) refused |= l.find("rejecting image") != std::string::npos;
  CHECK(refused);
  CHECK_EQ(banded.fake.stray_flushes, 0);
}

TEST("Wi-Fi setup: opening from USB releases an active talk button") {
  Rig r;
  r.bring_online(true);
  r.app.console("set screen_timeout 30");
  r.app.on_wifi_setup = [] { return "Temporary setup network"; };
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.mic_on);
  CHECK(r.app.start_wifi_setup());
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, false);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(!r.app.wifi_setup_open());
  r.advance(30000);
  CHECK_EQ(r.fake.brightness, 0);
}

TEST("app: a setting that can't be saved is reported, not silently kept in memory") {
  Rig r;
  r.app.begin();
  r.fake.storage_full = true;
  CHECK_EQ(r.app.console("set name Kitchen"), std::string("@error could not save name"));
  CHECK(r.app.console("get name").find("\"value\":\"\"") != std::string::npos);
  r.fake.storage_full = false;
  CHECK_EQ(r.app.console("set name Kitchen"), std::string("@ok name"));
  CHECK(r.app.console("get name").find("Kitchen") != std::string::npos);
}

TEST("app: the device still gets an identity when its key can't be saved") {
  Rig r;
  r.fake.storage_full = true;
  r.app.begin();
  CHECK(!r.app.device_id().empty());
  CHECK(r.fake.kv.count("device_key") == 0);
}

TEST("app: wifi saves, lists, joins and forgets networks without showing passwords") {
  Rig r;
  std::vector<std::string> changed;
  r.app.on_setting_changed = [&](std::string_view key) { changed.emplace_back(key); };
  r.app.current_wifi = [] { return std::string("My Home"); };
  CHECK_EQ(r.app.console(R"(wifi add "My Home" "pass word 1")"), std::string("@ok wifi add"));
  CHECK_EQ(r.app.console("wifi add Phone"), std::string("@ok wifi add"));
  CHECK_EQ(r.fake.kv["wifi_ssid"], std::string("My Home"));  // the first one, for older firmware
  const std::string list = r.app.console("wifi list");
  CHECK(list.rfind("@wifi ", 0) == 0);
  Value v;
  CHECK(hg::json::parse(list.substr(6), v));
  CHECK_EQ(v["networks"].size(), size_t(2));
  CHECK_EQ(v["networks"][0]["ssid"].as_string(), std::string("My Home"));
  CHECK(!v["networks"][0]["tested"].as_bool(true));
  CHECK(!v["networks"][0]["open"].as_bool(true));
  CHECK(v["networks"][1]["open"].as_bool());
  CHECK_EQ(v["current"].as_string(), std::string("My Home"));
  CHECK_EQ(v["max"].as_int(), int64_t(5));
  CHECK(list.find("pass word") == std::string::npos);
  CHECK_EQ(r.app.console("wifi"), list);

  CHECK_EQ(r.app.console("wifi join Phone"), std::string("@ok wifi join"));
  CHECK_EQ(r.fake.kv["wifi_ssid"], std::string("Phone"));
  CHECK(!r.fake.kv.count("wifi_pass"));
  CHECK_EQ(r.app.console(R"(wifi forget "My Home")"), std::string("@ok wifi forget"));
  CHECK_EQ(changed, std::vector<std::string>({"wifi_nets", "wifi_nets", "wifi_nets", "wifi_nets"}));
  CHECK_EQ(r.app.console("wifi forget Elsewhere"), std::string("@error no saved network Elsewhere"));
  CHECK_EQ(r.app.console("wifi join Elsewhere"), std::string("@error no saved network Elsewhere"));
  CHECK_EQ(changed.size(), size_t(4));
}

TEST("app: wifi refuses bad networks, a sixth network and malformed commands") {
  Rig r;
  CHECK(r.app.console("wifi add Home short").rfind("@error Use an 8..63", 0) == 0);
  CHECK(r.app.console(R"(wifi add "")").rfind("@error Network name", 0) == 0);
  for (int i = 0; i < 5; ++i) CHECK_EQ(r.app.console("wifi add Net" + std::to_string(i)), std::string("@ok wifi add"));
  CHECK_EQ(r.app.console("wifi add Net5"), std::string("@error 5 networks saved; forget one first"));
  CHECK(r.app.console("wifi add").rfind("@error usage", 0) == 0);
  CHECK(r.app.console("wifi add a b c").rfind("@error usage", 0) == 0);
  CHECK(r.app.console("wifi join a b").rfind("@error usage", 0) == 0);
  CHECK(r.app.console("wifi list extra").rfind("@error usage", 0) == 0);
  CHECK(r.app.console("wifi rename a").rfind("@error usage", 0) == 0);
  r.fake.storage_full = true;
  CHECK_EQ(r.app.console("wifi forget Net0"), std::string("@error could not save the network list"));
}

TEST("app: set wifi_ssid and wifi_pass put a network first and keep the others") {
  Rig r;
  r.app.console("wifi add Office password2");
  CHECK_EQ(r.app.console("set wifi_ssid Home"), std::string("@ok wifi_ssid"));
  CHECK_EQ(r.app.console("set wifi_pass password1"), std::string("@ok wifi_pass"));
  Value v;
  CHECK(hg::json::parse(r.app.console("wifi list").substr(6), v));
  CHECK_EQ(v["networks"].size(), size_t(2));
  CHECK_EQ(v["networks"][0]["ssid"].as_string(), std::string("Home"));
  CHECK_EQ(v["networks"][1]["ssid"].as_string(), std::string("Office"));
  CHECK(r.fake.kv["wifi_nets"].find("password1") != std::string::npos);  // saved, not just mirrored
  // Erasing the name forgets that network; the next one takes its place.
  CHECK_EQ(r.app.console("set wifi_ssid"), std::string("@ok wifi_ssid"));
  CHECK(hg::json::parse(r.app.console("wifi list").substr(6), v));
  CHECK_EQ(v["networks"].size(), size_t(1));
  CHECK_EQ(r.fake.kv["wifi_ssid"], std::string("Office"));
  CHECK_EQ(r.app.console("get wifi_pass"), std::string(R"(@value {"key":"wifi_pass","value":"<set>"})"));
}

TEST("app: factory-reset forgets every saved network") {
  Rig r;
  r.app.console("wifi add Home password1");
  r.app.console("wifi add Office password2");
  r.app.console("factory-reset");
  CHECK(!r.fake.kv.count("wifi_nets"));
  CHECK(!r.fake.kv.count("wifi_ssid"));
}

namespace {

// The port's side of phone pairing, as the hooks see it.
struct FakePhone {
  int pair_calls = 0, close_calls = 0, forget_calls = 0;
  bool can_pair = true;
  void attach(hg::App& app) {
    app.on_phone_pair = [this] { ++pair_calls; return can_pair; };
    app.on_phone_pair_close = [this] { ++close_calls; };
    app.on_phone_forget = [this] { ++forget_calls; };
  }
};

// Steps through the settings menu until `detail` shows; false if it never does.
bool find_setting(Rig& r, const std::string& detail) {
  for (int i = 0; i < 20; ++i) {
    if (r.app.model().detail == detail) return true;
    r.app.console("cancel");
  }
  return false;
}

}  // namespace

TEST("phone: boards without Bluetooth have no phone item, command or status") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  CHECK(!find_setting(r, "Phone app"));
  CHECK(find_setting(r, "Back to Hermes"));  // the items around it are still reachable
  r.app.console("settings close");
  CHECK_EQ(r.app.console("phone"), std::string("@error Bluetooth is not enabled on this board"));
  CHECK_EQ(r.app.console("phone pair"), std::string("@error Bluetooth is not enabled on this board"));
  CHECK(r.app.console("status").find("\"phone\"") == std::string::npos);
}

TEST("phone: the settings item opens pairing, shows the passkey and closes when paired") {
  Rig r;
  FakePhone phone;
  phone.attach(r.app);
  r.bring_online(true);
  CHECK(r.app.open_settings());
  CHECK(find_setting(r, "Phone app"));
  CHECK(r.app.model().body.find("No phone paired") == 0);
  r.app.console("talk");
  r.app.console("release");
  CHECK_EQ(phone.pair_calls, 1);
  CHECK(r.app.phone_pairing_open());
  CHECK(!r.app.settings_open());
  CHECK(r.app.screen() == hg::Screen::Setup);
  CHECK_EQ(r.app.model().headline, std::string("Pair a phone"));
  CHECK(!r.app.open_settings());  // the pairing screen owns the display
  CHECK_EQ(r.app.console("phone"), std::string("@phone {\"paired\":0,\"connected\":false,\"pairing\":true}"));

  r.app.on_phone_passkey(42);
  CHECK_EQ(r.app.model().headline, std::string("Code 000042"));
  // TALK doesn't record over the passkey.
  r.app.on_button(hg::Button::Talk, true);
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, false);

  r.app.on_phone_paired(true);
  r.app.on_phone_status(1, true);
  CHECK(!r.app.phone_pairing_open());
  CHECK_EQ(phone.close_calls, 1);
  CHECK_EQ(r.app.model().hint, std::string("Phone paired"));
  CHECK_EQ(r.app.console("phone"), std::string("@phone {\"paired\":1,\"connected\":true,\"pairing\":false}"));
  Value status;
  CHECK(hg::json::parse(r.app.console("status").substr(8), status));
  CHECK_EQ(status["phone"]["paired"].as_int(), int64_t(1));
  CHECK(r.app.open_settings());
  CHECK(find_setting(r, "Phone app"));
  CHECK(r.app.model().body.find("Phone connected.") == 0);
}

TEST("phone: the window closes on cancel and after two minutes; a late passkey is ignored") {
  Rig r(Rig::touch_profile());
  FakePhone phone;
  phone.attach(r.app);
  r.bring_online(true);
  CHECK_EQ(r.app.console("phone pair"), std::string("@ok phone pairing open"));
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(!r.app.phone_pairing_open());
  CHECK_EQ(phone.close_calls, 1);

  CHECK(r.app.start_phone_pairing());
  r.advance(hg::App::kPhonePairMs - 100);
  CHECK(r.app.phone_pairing_open());
  r.advance(200);
  CHECK(!r.app.phone_pairing_open());
  CHECK_EQ(phone.close_calls, 2);
  CHECK_EQ(r.app.model().hint, std::string("Phone pairing closed"));
  r.app.on_phone_passkey(123456);  // outside the window: nothing to show it on
  CHECK(r.app.screen() != hg::Screen::Setup);

  CHECK(r.app.start_phone_pairing());
  CHECK_EQ(r.app.console("phone pair close"), std::string("@ok phone pairing closed"));
  CHECK(!r.app.phone_pairing_open());
}

TEST("phone: Wi-Fi setup, a question and failed pairing each end the window") {
  Rig r;
  FakePhone phone;
  phone.attach(r.app);
  r.app.on_wifi_setup = [] { return std::string("Temporary setup network"); };
  r.bring_online(true);
  CHECK(r.app.start_phone_pairing());
  CHECK(r.app.start_wifi_setup());
  CHECK(!r.app.phone_pairing_open());
  CHECK(r.app.wifi_setup_open());
  CHECK(r.app.start_phone_pairing());  // and the other way round
  CHECK(!r.app.wifi_setup_open());

  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"Run it?"})");
  CHECK(!r.app.phone_pairing_open());
  CHECK(!r.app.start_phone_pairing());  // not over a question
  r.app.console("yes");

  CHECK(r.app.start_phone_pairing());
  r.app.on_phone_paired(false);
  CHECK(!r.app.phone_pairing_open());
  CHECK_EQ(r.app.model().hint, std::string("Phone pairing failed"));
}

TEST("phone: Bluetooth that won't start leaves no window; forget and factory-reset remove phones") {
  Rig r;
  FakePhone phone;
  phone.can_pair = false;
  phone.attach(r.app);
  r.bring_online(true);
  CHECK_EQ(r.app.console("phone pair"), std::string("@error phone pairing unavailable"));
  CHECK(!r.app.phone_pairing_open());
  CHECK_EQ(r.app.model().hint, std::string("Bluetooth unavailable"));

  r.app.on_phone_status(2, true);
  CHECK_EQ(r.app.console("phone forget"), std::string("@ok phone forget"));
  CHECK_EQ(phone.forget_calls, 1);
  CHECK_EQ(r.app.console("phone"), std::string("@phone {\"paired\":0,\"connected\":false,\"pairing\":false}"));
  r.app.console("factory-reset");
  CHECK_EQ(phone.forget_calls, 2);
  CHECK_EQ(r.app.console("phone bogus"), std::string("@error usage: phone [pair [close] | forget]"));
}

TEST("phone: the pairing screen stays lit through the screen timeout") {
  Rig r;
  FakePhone phone;
  phone.attach(r.app);
  r.fake.backlight = true;
  r.bring_online(true);
  r.app.console("set screen_timeout 10");
  r.advance(12000);
  CHECK_EQ(r.fake.brightness, 0);  // idle, the screen goes dark
  CHECK(r.app.start_phone_pairing());
  CHECK_EQ(r.fake.brightness, 100);
  r.advance(30000);  // three timeouts, still inside the server's heartbeat
  CHECK(r.app.phone_pairing_open());
  CHECK_EQ(r.fake.brightness, 100);
  // A passkey after the window has closed doesn't wake the screen.
  r.app.close_phone_pairing();
  r.advance(12000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_phone_passkey(123456);
  CHECK_EQ(r.fake.brightness, 0);
}

namespace {
// The last state message about the screen (sensor readings also travel as state).
const Value* last_screen_state(const FakeHal& fake) {
  for (auto it = fake.sent.rbegin(); it != fake.sent.rend(); ++it)
    if ((*it)["type"].as_string() == "state" && (*it)["display_sleeping"].is_bool()) return &*it;
  return nullptr;
}
}  // namespace

TEST("power: Hermes is told at once when the screen goes dark and lights up") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  CHECK(!(*r.fake.last("hello"))["display_sleeping"].as_bool());  // omitted while lit
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  r.fake.sent.clear();
  r.advance(29000);
  CHECK(last_screen_state(r.fake) == nullptr);  // dimmed is still lit
  r.advance(1100);
  CHECK_EQ(r.fake.brightness, 0);
  const Value* dark = last_screen_state(r.fake);
  CHECK(dark && (*dark)["display_sleeping"].as_bool());
  r.fake.sent.clear();
  r.advance(5000);
  CHECK(last_screen_state(r.fake) == nullptr);  // only changes are sent
  key.presses = 1;
  r.advance(100);
  const Value* lit = last_screen_state(r.fake);
  CHECK(lit && (*lit)["display_sleeping"].is_bool() && !(*lit)["display_sleeping"].as_bool());
}

TEST("power: a device that reconnects in the dark says so in its hello") {
  KeyPower key;
  Rig r;
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  key.presses = 1;
  r.advance(100);
  r.app.on_transport_closed("gone");
  r.fake.sent.clear();
  r.advance(5000);
  r.app.on_transport_open();
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["display_sleeping"].as_bool());
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true})");
  r.advance(100);
  CHECK(last_screen_state(r.fake) == nullptr);  // the hello already said it
}

namespace {
const char* kDashboard =
    R"({"type":"dashboard","v":1,"sessions":[{"label":"telegram 3f2a","state":"busy"},)"
    R"({"label":"gadget 91c0","state":"waiting"},{"label":"cli 77de","state":"idle"}],)"
    R"("tiles":[{"label":"NAS","value":"up","state":"ok"},{"label":"Backups","value":"2 days","state":"warn"}]})";

void tap_cancel(Rig& r) {
  r.app.on_button(hg::Button::Cancel, true);
  r.advance(50);
  r.app.on_button(hg::Button::Cancel, false);
}

size_t count_pixels(const FakeHal& fake, uint16_t color) {
  size_t n = 0;
  for (uint16_t px : fake.fb) n += px == color;
  return n;
}
}  // namespace

TEST("dashboard: the hello offers it, versioned, with the rows that fit") {
  Rig r;
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["dashboard"].is_object());
  if (!hello) return;
  CHECK_EQ((*hello)["caps"]["dashboard"]["v"].as_int(), int64_t(1));
  CHECK((*hello)["caps"]["dashboard"]["rows"].as_int() >= 4);

  hg::DeviceProfile talk_only = Rig::profile("ws://hermes.local:8765/gadget");
  talk_only.has_cancel_button = false;  // nothing could open it
  Rig t(talk_only);
  t.bring_online(true);
  CHECK(!(*t.fake.last("hello"))["caps"]["dashboard"].is_object());
}

TEST("dashboard: arrives quietly, opens with CANCEL on the idle screen and closes with it") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.screen() == hg::Screen::Ready);
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Ready);  // nothing to show yet
  r.server(kDashboard);
  CHECK(r.app.screen() == hg::Screen::Ready);  // a snapshot doesn't take the screen
  CHECK_EQ(r.app.model().detail, std::string("CANCEL shows sessions"));
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  const auto& rows = r.app.model().dash;
  CHECK_EQ(rows.size(), size_t(7));  // heading, 3 sessions, heading, 2 tiles
  if (rows.size() != 7) return;
  CHECK(rows[0].heading);
  CHECK_EQ(rows[1].label, std::string("telegram 3f2a"));
  CHECK(rows[1].value == "busy" && rows[1].tone == hg::DashRow::Tone::Busy);
  CHECK(rows[2].value == "approval" && rows[2].tone == hg::DashRow::Tone::Waiting);
  CHECK(rows[3].value == "idle" && rows[3].tone == hg::DashRow::Tone::Dim);
  CHECK(rows[4].heading);
  CHECK(rows[5].value == "up" && rows[5].tone == hg::DashRow::Tone::Good);
  CHECK(rows[6].tone == hg::DashRow::Tone::Warn);
  CHECK_EQ(r.app.model().headline, std::string("1 waiting"));
  CHECK_EQ(r.app.model().hint, std::string("CANCEL to close"));

  // A newer snapshot replaces it while it is open.
  r.server(R"({"type":"dashboard","v":1,"sessions":[{"label":"cli 77de","state":"idle"}]})");
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  CHECK_EQ(r.app.model().dash.size(), size_t(2));
  CHECK_EQ(r.app.model().headline, std::string("All idle"));
  r.server(R"({"type":"dashboard","v":2,"sessions":[]})");  // a layout this firmware doesn't know
  CHECK_EQ(r.app.model().dash.size(), size_t(2));

  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("dashboard: talking or a question takes over; a lost connection forgets it") {
  Rig r;
  r.bring_online(true);
  r.server(kDashboard);
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"ls"})");
  CHECK(r.app.screen() == hg::Screen::Prompt);
  r.server(R"({"type":"prompt.close","id":"q1"})");
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.advance(50);
  r.app.on_button(hg::Button::Talk, false);  // too short: discarded
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.advance(3000);

  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  r.app.on_transport_closed("gone");
  CHECK(r.app.screen() == hg::Screen::Connecting);
  r.advance(3000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true})");
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.app.model().detail, std::string("Ask me anything"));
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Ready);  // nothing until Hermes sends it again
}

TEST("dashboard: a session's state shows in its colour, and long lists page") {
  Rig r;
  r.bring_online(true);
  const uint16_t blue = hg::rgb565(90, 169, 242);
  r.server(R"({"type":"dashboard","v":1,"sessions":[{"label":"telegram","state":"idle"}]})");
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  const size_t idle_blue = count_pixels(r.fake, blue);
  r.server(R"({"type":"dashboard","v":1,"sessions":[{"label":"telegram","state":"busy"}]})");
  CHECK(count_pixels(r.fake, blue) > idle_blue);  // the busy dot and its word

  std::string many = R"({"type":"dashboard","v":1,"sessions":[)";
  for (int i = 0; i < 12; ++i) many += std::string(i ? "," : "") + R"({"label":"s)" + std::to_string(i) + R"(","state":"idle"})";
  many += "]}";
  r.server(many);
  CHECK_EQ(r.app.model().scroll, 0);
  r.advance(12000);
  CHECK(r.app.model().scroll > 0);  // pages itself
  for (int i = 0; i < 6 && r.app.model().scroll != 0; ++i) r.advance(12000);
  CHECK_EQ(r.app.model().scroll, 0);  // ...and starts over at the top
  r.app.on_button(hg::Button::Down, true);
  r.app.on_button(hg::Button::Down, false);
  CHECK_EQ(r.app.model().scroll, 1);
}

TEST("battery alert and charging notice: show over an open dashboard, which is still there after") {
  AlertPower battery;
  Rig r;
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  auto wait = [&](uint32_t ms) {
    for (uint32_t done = 0; done < ms; done += 5000) {
      r.server(R"({"type":"ping"})");
      r.advance(std::min<uint32_t>(5000, ms - done));
    }
  };
  CHECK_EQ(r.app.console("set battery_alert 10"), std::string("@ok battery_alert"));
  battery.on_battery(20);
  wait(5000);
  r.server(kDashboard);
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  battery.on_battery(10);  // a list the user opened is not busy: the alert does not wait for it
  wait(5000);
  CHECK_EQ(int(r.app.battery_alert()), 10);
  CHECK(r.app.screen() == hg::Screen::LowBattery);
  wait(10000);
  CHECK_EQ(int(r.app.battery_alert()), 0);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  battery.on_usb(10);
  wait(5000);
  CHECK(r.app.charging_notice());
  CHECK(r.app.screen() == hg::Screen::Charging);
  r.advance(1000);
  CHECK(r.app.charging_notice());  // the open list does not close it
  wait(5000);
  CHECK(!r.app.charging_notice());
  CHECK(r.app.screen() == hg::Screen::Dashboard);
}

TEST("dashboard: touch boards open it with a swipe down") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  r.server(kDashboard);
  CHECK_EQ(r.app.model().detail, std::string("Swipe down for sessions"));
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  CHECK_EQ(r.app.model().hint, std::string("Swipe down to close"));
}

// ---------------------------------------------------------------------------
// Language of the device's own text (hg/i18n.hpp)

TEST("language: English by default, and set language switches the device's own text") {
  Rig r;
  r.bring_online(true);
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Hermes"));
  CHECK_EQ(r.app.model().hint, std::string("hold TALK to talk"));
  CHECK(r.app.model().lang == hg::i18n::Lang::En);
  CHECK_EQ(r.app.console("set language es"), std::string("@ok language"));
  CHECK_EQ(r.fake.kv["language"], std::string("es"));
  CHECK(r.app.model().lang == hg::i18n::Lang::Es);
  CHECK_EQ(r.app.model().headline, std::string("Hola, soy Hermes"));
  CHECK_EQ(r.app.model().detail, std::string("Pregúntame algo"));  // drawn with its accent (Latin-1 font)
  CHECK_EQ(r.app.model().hint, std::string("mantén TALK y habla"));
  CHECK_EQ(r.app.console("get language"), std::string(R"(@value {"key":"language","value":"es"})"));
  // An empty value goes back to English.
  CHECK_EQ(r.app.console("set language"), std::string("@ok language"));
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Hermes"));
}

TEST("language: an unknown code is refused and changes nothing") {
  Rig r;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set language xx"), std::string("@error language must be one of: en es auto"));
  CHECK_EQ(r.app.console("set language ES"), std::string("@error language must be one of: en es auto"));
  CHECK(r.fake.kv.find("language") == r.fake.kv.end());
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Hermes"));
  // A stored value this firmware doesn't know (from a newer one) reads as English.
  r.fake.kv["language"] = "xx";
  r.app.console("set volume 50");  // reloads the settings
  CHECK(r.app.model().lang == hg::i18n::Lang::En);
}

TEST("language: the choice survives a restart") {
  Rig r;
  r.bring_online(true);
  r.app.console("set language es");
  hg::App again(r.hal, Rig::profile("ws://hermes.local:8765/gadget"));
  again.begin();
  CHECK(again.screen() == hg::Screen::Boot);
  CHECK_EQ(again.model().detail, std::string("iniciando - fw 1.2.3"));
}

TEST("language: the settings menu has a Language item that steps through the languages") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  CHECK(find_setting(r, "Language"));
  CHECK(r.app.model().body.find("English") != std::string::npos);
  r.app.console("talk");
  r.app.console("release");
  CHECK_EQ(r.fake.kv["language"], std::string("es"));
  CHECK(r.app.settings_open());  // still on the same item, now in Spanish
  CHECK_EQ(r.app.model().headline, std::string("Ajustes"));
  CHECK_EQ(r.app.model().detail, std::string("Idioma / Language"));  // the way back stays readable
  CHECK(r.app.model().body.find("Español") != std::string::npos);
  r.app.console("talk");
  r.app.console("release");
  CHECK_EQ(r.fake.kv["language"], std::string("auto"));  // after the last translation: follow Hermes
  CHECK_EQ(r.app.model().detail, std::string("Language"));  // this Hermes named no language: English
  CHECK(r.app.model().body.find("Automatic, as Hermes: English") != std::string::npos);
  r.app.console("talk");
  r.app.console("release");
  CHECK_EQ(r.fake.kv["language"], std::string("en"));
  CHECK_EQ(r.app.model().detail, std::string("Language"));
  // Every other item is still reachable after it.
  CHECK(find_setting(r, "Back to Hermes"));
  CHECK(find_setting(r, "Speaker volume"));
}

TEST("language: the agent can set it through the screen.language action") {
  Rig r;
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  bool listed = false;
  if (hello) {
    for (const auto& a : (*hello)["actions"].elements()) {
      if (a["name"].as_string() != "screen.language") continue;
      listed = true;
      const auto& codes = a["params"]["properties"]["language"]["enum"];
      CHECK_EQ(codes.size(), hg::i18n::kLangCount + 1);  // and "auto"
    }
  }
  CHECK(listed);
  r.server(R"({"type":"action","id":"l1","name":"screen.language","args":{"language":"es"}})");
  const Value* res = r.fake.last("action.result");
  CHECK(res && (*res)["ok"].as_bool() && (*res)["result"]["language"].as_string() == "es");
  CHECK_EQ(r.app.model().headline, std::string("Hola, soy Hermes"));
  r.server(R"({"type":"action","id":"l2","name":"screen.language","args":{"language":"klingon"}})");
  res = r.fake.last("action.result");
  CHECK(res && !(*res)["ok"].as_bool() && (*res)["error"].as_string() == "language must be one of: en es auto");
  r.server(R"({"type":"action","id":"l3","name":"screen.language","args":{}})");
  res = r.fake.last("action.result");
  CHECK(res && !(*res)["ok"].as_bool());
  CHECK_EQ(r.fake.kv["language"], std::string("es"));
}

TEST("language: a touch board's control phrase is translated with the rest") {
  Rig r(Rig::touch_profile());
  r.bring_online(true);
  r.app.console("set language es");
  CHECK_EQ(r.app.model().hint, std::string("Mantén la pantalla y habla"));
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  CHECK_EQ(r.app.model().headline, std::string("Escuchando"));
  CHECK_EQ(r.app.model().detail, std::string("Suelta para enviar"));
  CHECK_EQ(r.app.model().hint, std::string("Desliza: descartar"));
}

TEST("language: the dashboard's headings and states are translated") {
  Rig r;
  r.bring_online(true);
  r.app.console("set language es");
  r.server(kDashboard);
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  const auto& rows = r.app.model().dash;
  CHECK_EQ(rows.size(), size_t(7));
  if (rows.size() != 7) return;
  CHECK_EQ(rows[0].label, std::string("Sesiones de Hermes"));
  CHECK_EQ(rows[1].value, std::string("ocupada"));
  CHECK(rows[2].value == "aprobar" && rows[2].tone == hg::DashRow::Tone::Waiting);
  CHECK_EQ(rows[3].value, std::string("en reposo"));
  CHECK_EQ(rows[4].label, std::string("Estado"));
  CHECK_EQ(rows[5].value, std::string("up"));  // a tile's value is the user's own text
  CHECK_EQ(r.app.model().headline, std::string("Esperan: 1"));
  CHECK_EQ(r.app.model().hint, std::string("CANCEL: cerrar"));
}

TEST("language: the status report keeps its English reason while the screen translates it") {
  Rig r("");
  r.app.console("set language es");
  r.app.begin();
  r.app.on_network(true);
  r.advance(1000);
  CHECK(r.app.screen() == hg::Screen::Error);
  CHECK_EQ(r.app.model().headline, std::string("Falta configurar"));
  CHECK(r.app.model().detail.rfind("No hay servidor de Hermes configurado", 0) == 0);
  CHECK(r.app.console("status").find("No Hermes server configured") != std::string::npos);
}

TEST("profiles: hello offers switching; welcome brings the roster") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  const Value* hello = r.fake.last("hello");
  CHECK(hello != nullptr);
  if (!hello) return;
  CHECK((*hello)["caps"]["profiles"].as_bool());
  CHECK(!hello->has("profile"));  // default is implied
  CHECK_EQ(r.app.agent_profile(), std::string("ops"));
  CHECK_EQ(r.app.agent_profile_name(), std::string("Ops"));
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Ops"));
  CHECK(r.app.status_json().find("\"profile\":\"ops\"") != std::string::npos);
  CHECK_EQ(r.fake.kv["profile"], std::string("ops"));
}

TEST("profiles: settings ask Hermes for the next profile and keep what it confirms") {
  Rig r;
  r.bring_online_with_profiles();
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Hermes"));
  CHECK(r.app.open_settings());
  CHECK(find_setting(r, "Hermes profile"));
  CHECK(r.app.model().body.find("Hermes") != std::string::npos);
  r.app.console("talk");
  r.app.console("release");
  const Value* select = r.fake.last("profile.select");
  CHECK(select != nullptr);
  if (select) CHECK_EQ((*select)["profile"].as_string(), std::string("ops"));
  CHECK(r.fake.kv.count("profile") == 0);  // nothing changes until Hermes confirms
  r.server(R"({"type":"profile","profile":"ops"})");
  CHECK_EQ(r.fake.kv["profile"], std::string("ops"));
  CHECK(r.app.model().body.find("Ops") != std::string::npos);
  r.server(R"({"type":"profile","profile":"ops","error":"busy"})");  // a refusal keeps the current one
  CHECK_EQ(r.app.agent_profile(), std::string("ops"));

  Rig reboot;
  reboot.fake.kv = r.fake.kv;
  reboot.app.begin();
  reboot.app.on_network(true, "wifi");
  reboot.advance(1000);
  reboot.app.on_transport_open();
  const Value* hello = reboot.fake.last("hello");
  CHECK(hello != nullptr);
  if (hello) CHECK_EQ((*hello)["profile"].as_string(), std::string("ops"));
}

TEST("profiles: without a roster the settings menu has no profile entry") {
  Rig r;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  CHECK(!find_setting(r, "Hermes profile"));
  CHECK(!r.app.next_agent_profile());
}

TEST("profiles: no switching while a turn runs or before pairing") {
  Rig r;
  r.bring_online_with_profiles();
  r.server(R"({"type":"turn.start","turn":"t"})");
  CHECK(!r.app.next_agent_profile());
  Rig unpaired;  // a roster, but not approved yet: only the pairing guard can refuse
  unpaired.bring_online_with_profiles(false);
  CHECK(!unpaired.app.next_agent_profile());
}

TEST("profiles: a stored profile Hermes no longer offers is dropped; the console cannot set one") {
  Rig r;
  r.fake.kv["profile"] = "gone";
  r.bring_online_with_profiles();
  const Value* hello = r.fake.last("hello");
  CHECK(hello != nullptr);
  if (hello) CHECK_EQ((*hello)["profile"].as_string(), std::string("gone"));
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  CHECK(r.fake.kv.count("profile") == 0);
  CHECK_EQ(r.app.console("set profile ops"), std::string("@error unknown key"));
  r.server(R"({"type":"profile","profile":"default","error":"busy"})");
  CHECK_EQ(r.app.model().detail, std::string("Finish this turn first"));  // refusals are visible on Ready
  r.server(R"({"type":"profile","profile":"ops"})");
  CHECK_EQ(r.fake.kv["profile"], std::string("ops"));
  r.app.console("factory-reset");
  CHECK(r.fake.kv.count("profile") == 0);  // fails without the explicit erase
}

TEST("profiles: hello names only the stored choice, through reconnects and settings edits") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  CHECK_EQ(r.app.console("set volume 60"), std::string("@ok volume"));
  CHECK_EQ(r.app.agent_profile(), std::string("ops"));  // reloading settings keeps the confirmed profile
  r.server(R"({"type":"profile","profile":"default","error":"unpaired"})");  // a failed switch away
  CHECK_EQ(r.app.agent_profile(), std::string("ops"));
  CHECK_EQ(r.fake.kv["profile"], std::string("ops"));
  r.fake.sent.clear();
  r.app.on_transport_closed("network flap");
  r.advance(5000);
  r.app.on_transport_open();
  const Value* hello = r.fake.last("hello");
  CHECK(hello != nullptr);
  if (hello) CHECK_EQ((*hello)["profile"].as_string(), std::string("ops"));

  Rig routed;  // a gadget route: no roster, and the welcome names no profile
  routed.bring_online(true);
  routed.fake.sent.clear();
  routed.app.on_transport_closed("network flap");
  routed.advance(5000);
  routed.app.on_transport_open();
  hello = routed.fake.last("hello");
  CHECK(hello != nullptr);
  if (hello) CHECK(!hello->has("profile"));
}

namespace {
// The middle of the first panel row where `hit` holds, or {-1, -1}.
template <class Hit>
std::pair<int, int> find_point(const Rig& r, Hit hit) {
  for (int y = 0; y < r.fake.height; y += 2) {
    int x0 = -1, x1 = -1;
    for (int x = 0; x < r.fake.width; ++x)
      if (hit(x, y)) (x0 < 0 ? x0 : x1) = x, x1 = x;
    if (x0 >= 0) return {(x0 + x1) / 2, y + 2};
  }
  return {-1, -1};
}
std::pair<int, int> name_point(Rig& r) { return find_point(r, [&](int x, int y) { return r.app.profile_name_hit(x, y); }); }
std::pair<int, int> row_point(Rig& r, int row) {
  return find_point(r, [&](int x, int y) { return r.app.profile_row_hit(x, y) == row; });
}
}  // namespace

TEST("profiles: a quick tap on the name opens the grid; a hold on it talks") {
  for (int round : {0, 466}) {
    Rig r(Rig::touch_profile());
    if (round) r.fake.make_round(round);
    r.bring_online_with_profiles();
    CHECK_EQ(r.app.model().detail.rfind("Swipe to change", 0), size_t(0));
    auto [x, y] = name_point(r);
    CHECK(x >= 0);
    int shared = 0;  // the name and the SETTINGS target never overlap
    for (int py = 0; py < r.fake.height; ++py)
      for (int px = 0; px < r.fake.width; ++px) shared += r.app.profile_name_hit(px, py) && r.app.settings_title_hit(px, py);
    CHECK_EQ(shared, 0);
    hg::TouchGestures touch(r.app);
    touch.update(true, x, y, r.fake.clock);
    r.advance(500);
    touch.tick(r.fake.clock);
    CHECK(r.fake.mic_on);  // the name sits where people hold to talk
    touch.update(false, 0, 0, r.fake.clock);
    CHECK(!r.app.profile_picker_open());
    r.advance(30000);  // past the reply's dwell
    touch.update(true, x, y, r.fake.clock);
    r.advance(50);
    touch.update(false, 0, 0, r.fake.clock);
    CHECK(r.app.profile_picker_open());
    CHECK_EQ(r.app.status_json().find("\"screen\":\"profiles\"") != std::string::npos, true);
    CHECK_EQ(r.app.model().dash.size(), size_t(3));  // two profiles, then Settings
    CHECK_EQ(r.app.model().settings_row, 2);
    CHECK_EQ(r.app.model().dash[0].value, std::string("default"));  // the id, which colours an initial
    CHECK(r.app.model().dash[0].tone == hg::DashRow::Tone::Waiting);  // the current one
    CHECK_EQ(r.app.model().mascot_row, 0);  // the default profile wears the mascot
    CHECK_EQ(r.app.model().face_row, -1);
  }
}

TEST("profiles: a tap on a row asks Hermes for it; a hold or a swipe down picks nothing") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles();
  hg::TouchGestures touch(r.app);
  CHECK(r.app.open_profile_picker());
  auto [x, y] = row_point(r, 1);
  CHECK(x >= 0);
  touch.update(true, x, y, r.fake.clock);
  r.advance(800);
  touch.tick(r.fake.clock);
  CHECK(!r.fake.mic_on);  // a hold in the list never records
  touch.update(true, x, y + 70, r.fake.clock);  // and becomes a swipe down
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.app.profile_picker_open());
  CHECK(r.fake.last("profile.select") == nullptr);
  CHECK(r.app.model().screen != hg::Screen::Dashboard);

  CHECK(r.app.open_profile_picker());
  touch.update(true, x, y, r.fake.clock);
  r.advance(50);
  touch.update(false, 0, 0, r.fake.clock);
  const Value* select = r.fake.last("profile.select");
  CHECK(select != nullptr);
  if (select) CHECK_EQ((*select)["profile"].as_string(), std::string("ops"));
  CHECK(!r.app.profile_picker_open());
  CHECK_EQ(r.app.model().detail, std::string("Switching to Ops..."));
  r.server(R"({"type":"profile","profile":"ops"})");
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Ops"));
}

TEST("profiles: a refused switch keeps the profile; no list while a turn runs") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles();
  CHECK(r.app.open_profile_picker());
  CHECK(r.app.select_agent_profile(1));
  r.server(R"({"type":"profile","profile":"default","error":"unpaired"})");
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  CHECK_EQ(r.app.model().detail, std::string("Switch not allowed"));
  CHECK(r.fake.kv.count("profile") == 0);
  r.server(R"({"type":"turn.start","turn":"t"})");
  CHECK(!r.app.open_profile_picker());
  CHECK(!r.app.select_agent_profile(1));
  CHECK(!r.app.profile_name_hit(name_point(r).first, name_point(r).second));
}

TEST("profiles: a long name never runs under the name panel's marker") {
  Rig r(Rig::touch_profile());
  r.fake.make_round(466);
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true,"profile":"r",)"
           R"("profiles":[{"id":"default","name":"Hermes"},{"id":"r","name":"Research Assistant"}]})");
  r.advance(100);
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Research Assistant"));
  const int w = r.fake.width;
  auto px = [&](int x, int y) { return r.fake.fb[static_cast<size_t>(y * w + x)]; };
  int top = -1, bottom = -1, left = w;
  for (int y = 0; y < r.fake.height; ++y)
    for (int x = 0; x < w; ++x)
      if (r.app.profile_name_hit(x, y)) top = top < 0 ? y : top, bottom = y, left = std::min(left, x);
  CHECK(top >= 0);
  if (top < 0) return;
  // Where each colour is drawn in the panel; rows that do not start in the panel's colour are its rounded edge.
  struct Box { int x0, y0, x1, y1; };
  const uint16_t panel = px(left + 1, (top + bottom) / 2);
  std::map<uint16_t, Box> boxes;
  for (int y = top; y <= bottom; ++y) {
    if (px(left, y) != panel) continue;
    for (int x = left; x < w && r.app.profile_name_hit(x, y); ++x) {
      if (px(x, y) == panel) continue;
      auto [it, added] = boxes.try_emplace(px(x, y), Box{x, y, x, y});
      Box& b = it->second;
      b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y), b.x1 = std::max(b.x1, x), b.y1 = std::max(b.y1, y);
    }
  }
  // The headline is the widest line, so it starts furthest left; the marker ends furthest right of the rest.
  CHECK(boxes.size() >= 3);
  if (boxes.size() < 3) return;
  auto head = std::min_element(boxes.begin(), boxes.end(), [](auto& a, auto& b) { return a.second.x0 < b.second.x0; });
  const Box h = head->second;
  boxes.erase(head);
  const Box m =
      std::max_element(boxes.begin(), boxes.end(), [](auto& a, auto& b) { return a.second.x1 < b.second.x1; })->second;
  CHECK(h.x1 < m.x0 || m.y1 < h.y0 || h.y1 < m.y0);
}

TEST("profiles: buttons show Change in Settings, tap-to-talk the swipe; one profile shows nothing new") {
  Rig buttons;
  buttons.bring_online_with_profiles();
  CHECK_EQ(buttons.app.model().detail, std::string("Change in Settings"));
  CHECK(!buttons.app.model().name_target);
  CHECK(!buttons.app.open_profile_picker());  // the settings item steps through profiles instead

  Rig tap(Rig::touch_profile());
  tap.bring_online_with_profiles();
  tap.app.console("set talk_mode tap");
  CHECK_EQ(tap.app.model().detail, std::string("Swipe to change"));  // a tap records, but a swipe still opens the grid
  CHECK_EQ(name_point(tap).first, -1);
  CHECK(tap.app.profile_swipe_opens());

  Rig single(Rig::touch_profile());
  single.bring_online(true);
  single.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true,"profile":"default",)"
                R"("profiles":[{"id":"default","name":"Hermes"}]})");
  CHECK_EQ(single.app.model().headline, std::string("Hi, I'm Hermes"));
  CHECK(single.app.model().detail.find("name") == std::string::npos);
  CHECK(single.app.model().detail.find("Settings") == std::string::npos);
  CHECK_EQ(name_point(single).first, -1);
}

TEST("profiles: with touch_talk off, the name is a tap target in tap-to-talk mode too; a hold on it records nothing") {
  hg::DeviceProfile p = Rig::touch_profile();
  p.talk_key = true;
  p.talk_label = "BOOT";
  Rig r(p);
  r.bring_online_with_profiles();
  r.app.console("set talk_mode tap");
  CHECK_EQ(name_point(r).first, -1);
  CHECK_EQ(r.app.console("set touch_talk off"), std::string("@ok touch_talk"));
  // The hint names the swipe, which opens the grid in every mode; a tap on the name does too.
  CHECK_EQ(r.app.model().detail.rfind("Swipe to change", 0), size_t(0));
  auto [x, y] = name_point(r);
  CHECK(x >= 0);
  hg::TouchGestures touch(r.app);
  touch.update(true, x, y, r.fake.clock);
  r.advance(40);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.app.profile_picker_open());
  CHECK(!r.fake.mic_on);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(!r.app.profile_picker_open());
  CHECK_EQ(r.app.console("set talk_mode hold"), std::string("@ok talk_mode"));
  const auto held = name_point(r);
  CHECK(held.first >= 0);
  touch.update(true, held.first, held.second, r.fake.clock);  // a hold on the name in hold mode: no recording
  r.advance(500);
  touch.tick(r.fake.clock);
  CHECK(!r.fake.mic_on);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.fake.last("audio.start") == nullptr);
  CHECK(!r.app.profile_picker_open());  // the hold's lift is no tap: the list stays shut
  CHECK_EQ(r.app.console("set talk_mode tap"), std::string("@ok talk_mode"));
  touch.update(true, held.first, held.second, r.fake.clock);  // the same hold in tap mode
  r.advance(500);
  touch.tick(r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(!r.app.profile_picker_open());
  CHECK(!r.fake.mic_on);
}

TEST("profiles: one switch at a time; a later pick waits for Hermes's answer") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles();
  CHECK(r.app.open_profile_picker());
  CHECK(r.app.select_agent_profile(1));  // Ops, then back to the list before Hermes answers
  CHECK(r.app.profile_switch_pending());
  CHECK(!r.app.open_profile_picker());
  CHECK(!r.app.select_agent_profile(0));
  CHECK(!r.app.next_agent_profile());
  int selects = 0;
  for (const Value& v : r.fake.sent) selects += v["type"].as_string() == "profile.select";
  CHECK_EQ(selects, 1);
  r.advance(5000);
  CHECK_EQ(r.app.model().detail, std::string("Switching to Ops..."));
  CHECK_EQ(r.app.agent_profile(), std::string("default"));  // nothing changes until Hermes confirms
  CHECK(r.fake.kv.count("profile") == 0);
  r.server(R"({"type":"profile","profile":"default","error":"busy"})");
  CHECK(!r.app.profile_switch_pending());
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  CHECK(r.app.open_profile_picker());  // a refusal frees the next choice
}

TEST("profiles: no answer to a switch reconnects, and the welcome says which profile answers") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles();
  CHECK(r.app.select_agent_profile(1));
  r.advance(hg::App::kProfileReplyMs - 1000);
  CHECK(r.app.profile_switch_pending());
  r.advance(2000);
  CHECK(!r.app.profile_switch_pending());
  CHECK(r.app.status_json().find("\"phase\":\"connecting\"") != std::string::npos);  // reconnecting
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  r.advance(5000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true,"profile":"ops",)"
           R"("profiles":[{"id":"default","name":"Hermes"},{"id":"ops","name":"Ops"}]})");
  CHECK_EQ(r.app.agent_profile(), std::string("ops"));  // the switch had happened after all
  CHECK(r.app.open_profile_picker());
}

namespace {
std::string roster_of(int n) {
  std::string roster;
  for (int i = 0; i < n; ++i)
    roster += std::string(i ? "," : "") + R"({"id":"p)" + std::to_string(i) + R"(","name":"Profile )" + std::to_string(i) + "\"}";
  return roster;
}
void welcome_with_roster(Rig& r, int n, const std::string& current = "p0") {
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true,"profile":")" + current +
           R"(","profiles":[)" + roster_of(n) + "]}");
}
}  // namespace

TEST("profiles: the name and every grid cell are about 7 mm tall and wide on each touch board, any grid") {
  struct Board {
    const char* name;
    int w, h;
    bool round;
    uint8_t font_scale;
    uint16_t ppi;  // the estimates in firmware/esp32/main/board.cpp
    int mm10 = 70;  // the smallest target, in tenths of a millimetre
  };
  const Board boards[] = {
      {"lcd-1.54", 240, 240, false, 1, 220},   {"lcd-1.83", 240, 284, false, 1, 203},
      {"amoled-1.75", 466, 466, true, 1, 266}, {"amoled-1.32", 466, 466, true, 1, 353},
      {"amoled-1.43c", 466, 466, true, 1, 326},
      {"amoled-1.8", 368, 432, false, 1, 322}, {"amoled-2.06", 410, 486, false, 1, 315},
      {"amoled-2.16", 480, 480, false, 1, 314}, {"lcd-1.85c", 360, 360, true, 1, 195},
      {"knob-1.8", 360, 360, true, 2, 200},     {"box-3", 320, 240, false, 1, 167},
      {"box-1.28", 240, 240, true, 1, 188, 64},  // Spotpear BOX 1.28: its 169 px square holds ~6.4 mm cells
      {"crowpanel-2.1", 480, 480, true, 1, 229},
      {"crowpanel-5.0", 800, 480, false, 1, 187}, {"p4-10.1", 1024, 600, false, 1, 118},
      {"cores3", 320, 240, false, 1, 200},
  };
  for (const Board& b : boards) {
    for (int grid : {4, 6, 9, 16}) {
      for (int n : {2, 8, 32}) {
        hg::DeviceProfile p = Rig::touch_profile();
        p.touch_ppi = b.ppi;
        Rig r(p);
        r.fake.make_panel(b.w, b.h, b.round);
        r.fake.font_scale = b.font_scale;
        r.fake.kv["profile_grid"] = std::to_string(grid);
        welcome_with_roster(r, n);
        const int min_px = (b.mm10 * b.ppi + 253) / 254;
        if (grid == 4) {
          int name_rows = 0;
          for (int y = 0; y < b.h; ++y) name_rows += r.app.profile_name_hit(b.w / 2, y);
          if (name_rows < min_px) std::printf("    %s, %d profiles: name %d px < %d\n", b.name, n, name_rows, min_px);
          CHECK(name_rows >= min_px);
        }
        CHECK(r.app.open_profile_picker());
        const int per = r.app.profile_per_page();
        CHECK(per >= 1 && per <= grid);
        std::vector<bool> seen(static_cast<size_t>(n) + 1);
        const int pages = (n + 1 + per - 1) / std::max(1, per);
        for (int page = 0; page < pages; ++page) {
          // Each cell's extent through its middle, across and down.
          std::map<int, std::pair<int, int>> across, down;  // item: first, last
          for (int y = 0; y < b.h; y += 1)
            for (int x = 0; x < b.w; x += 1) {
              const int hit = r.app.profile_row_hit(x, y);
              if (hit < 0) continue;
              auto [ita, a_new] = across.try_emplace(hit, std::make_pair(x, x));
              auto [itd, d_new] = down.try_emplace(hit, std::make_pair(y, y));
              ita->second.second = std::max(ita->second.second, x);
              itd->second.second = std::max(itd->second.second, y);
            }
          for (const auto& [item, ext] : across) {
            const int wide = ext.second - ext.first + 1, tall = down[item].second - down[item].first + 1;
            if (wide < min_px || tall < min_px)
              std::printf("    %s, grid %d, %d profiles: cell %d is %dx%d px < %d\n", b.name, grid, n, item, wide, tall, min_px);
            CHECK(wide >= min_px && tall >= min_px);
            CHECK(item >= 0 && item <= n);  // n: the Settings cell after the profiles
            if (item >= 0 && item <= n) seen[static_cast<size_t>(item)] = true;
          }
          if (pages > 1) CHECK(r.app.profile_page_step(1));
          CHECK(r.app.profile_picker_open());
        }
        for (int i = 0; i <= n; ++i) CHECK(seen[static_cast<size_t>(i)]);
      }
    }
  }
}

TEST("profiles: the 2.06 and the round 1.75 show the grid asked for; few profiles get big icons") {
  struct Case { int w, h; bool round; uint16_t ppi; int grid, n, per; };
  const Case cases[] = {
      {410, 486, false, 315, 0, 16, 9},  // the default: 3 x 3
      {466, 466, true, 266, 0, 16, 9},
      {410, 486, false, 315, 4, 16, 4},  {410, 486, false, 315, 6, 16, 6},
      {410, 486, false, 315, 9, 16, 9},  {410, 486, false, 315, 16, 16, 16},
      {466, 466, true, 266, 4, 16, 4},   {466, 466, true, 266, 9, 16, 9},
      {466, 466, true, 266, 16, 16, 9},  // the round 1.75's inscribed square: 4x4 names would need smaller letters
      {410, 486, false, 315, 16, 3, 4},  // three profiles in a 2x2, not a quarter of a 4x4
      {410, 486, false, 315, 16, 7, 9},
  };
  for (const Case& c : cases) {
    hg::DeviceProfile p = Rig::touch_profile();
    p.touch_ppi = c.ppi;
    Rig r(p);
    r.fake.make_panel(c.w, c.h, c.round);
    if (c.grid) r.fake.kv["profile_grid"] = std::to_string(c.grid);
    welcome_with_roster(r, c.n);
    CHECK(r.app.open_profile_picker());
    const int per = r.app.profile_per_page();
    if (c.grid == 6) {  // 6 is two columns of three on these portrait and square panels
      int cols = 0;
      for (int i = 0; i < c.n && i < per; ++i) cols += row_point(r, i).second == row_point(r, 0).second;
      CHECK_EQ(cols, 2);
    }
    if (per != c.per) std::printf("    %dx%d grid %d, %d profiles: %d per page, want %d\n", c.w, c.h, c.grid, c.n, per, c.per);
    CHECK_EQ(per, c.per);
  }
}

TEST("profiles: a swipe from right to left on Ready opens the grid at the current profile's page; on the grid it turns the page") {
  hg::DeviceProfile p = Rig::touch_profile();
  Rig r(p);
  r.fake.make_panel(410, 486, false);
  r.fake.kv["profile_grid"] = "4";
  welcome_with_roster(r, 16, "p9");  // 2x2: page 3 of 5 holds p8..p11; the fifth, Settings
  hg::TouchGestures touch(r.app);
  auto swipe = [&](int dx, int dy = 0) {
    const int x = r.fake.width / 2 - dx / 2, y = r.fake.height / 2;
    touch.update(true, x, y, r.fake.clock);
    for (int i = 1; i <= 4; ++i) {
      r.advance(15);
      touch.update(true, x + dx * i / 4, y + dy * i / 4, r.fake.clock);
    }
    touch.update(false, 0, 0, r.fake.clock);
  };
  CHECK(r.app.profile_swipe_opens());
  swipe(120);  // from left to right: nothing
  CHECK(!r.app.profile_picker_open());
  CHECK(!r.fake.mic_on);
  swipe(-120);
  CHECK(r.app.profile_picker_open());
  CHECK(!r.fake.mic_on);
  CHECK_EQ(r.app.model().scroll, 8);
  swipe(-120);  // the next page comes in from the right
  CHECK_EQ(r.app.model().scroll, 12);
  swipe(-120);  // the last page: Settings alone
  CHECK_EQ(r.app.model().scroll, 16);
  swipe(-120);  // and wraps around
  CHECK_EQ(r.app.model().scroll, 0);
  swipe(120);
  CHECK_EQ(r.app.model().scroll, 16);
  swipe(-30);  // too short: nothing, and no profile picked
  CHECK_EQ(r.app.model().scroll, 16);
  swipe(30, 120);  // more down than sideways: a swipe down closes the grid
  CHECK(!r.app.profile_picker_open());
  CHECK(r.fake.last("profile.select") == nullptr);

  // A tap on a cell asks Hermes for that profile and goes back to Ready.
  swipe(-120);
  CHECK(r.app.profile_picker_open());
  auto [x, y] = row_point(r, 10);
  CHECK(x >= 0);
  touch.update(true, x, y, r.fake.clock);
  r.advance(50);
  touch.update(false, 0, 0, r.fake.clock);
  const Value* select = r.fake.last("profile.select");
  CHECK(select != nullptr);
  if (select) CHECK_EQ((*select)["profile"].as_string(), std::string("p10"));
  CHECK(!r.app.profile_picker_open());
  CHECK_EQ(r.app.model().screen, hg::Screen::Ready);
  r.server(R"({"type":"profile","profile":"p10"})");
  CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Profile 10"));
}

TEST("profiles: a sideways swipe opens nothing while a turn runs, on other screens or with one profile; a hold still talks") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles();
  hg::TouchGestures touch(r.app);
  const int x = r.fake.width / 2, y = r.fake.height / 2;
  // A still finger past the hold time talks, and a drift sideways afterwards changes nothing.
  touch.update(true, x, y, r.fake.clock);
  r.advance(300);
  touch.tick(r.fake.clock);
  CHECK(r.fake.mic_on);
  touch.update(true, x + 100, y, r.fake.clock);
  CHECK(r.fake.mic_on);
  CHECK(!r.app.profile_picker_open());
  touch.update(false, 0, 0, r.fake.clock);
  r.advance(30000);
  CHECK(r.app.profile_swipe_opens());
  r.server(R"({"type":"turn.start","turn":"t"})");
  CHECK(!r.app.profile_swipe_opens());  // a turn runs
  r.server(R"({"type":"turn.end","turn":"t"})");
  r.app.console("settings");
  CHECK(!r.app.profile_swipe_opens());

  Rig single(Rig::touch_profile());
  single.bring_online(true);
  CHECK(!single.app.profile_swipe_opens());
  Rig buttons;
  buttons.bring_online_with_profiles();
  CHECK(!buttons.app.profile_swipe_opens());
}

TEST("profiles: a profile's initial is always a capital letter") {
  CHECK_EQ(hg::profile_initial("fiscal"), std::string("F"));
  CHECK_EQ(hg::profile_initial("garagem"), std::string("G"));
  CHECK_EQ(hg::profile_initial("Saude"), std::string("S"));
  CHECK_EQ(hg::profile_initial(" work"), std::string("W"));
  CHECK_EQ(hg::profile_initial("\xc3\xa9tica"), std::string("\xc3\x89"));  // é becomes É
  CHECK_EQ(hg::profile_initial("\xc3\xb7x"), std::string("\xc3\xb7"));  // ÷ has no capital
  CHECK_EQ(hg::profile_initial("2fa"), std::string("2"));
  CHECK_EQ(hg::profile_initial(""), std::string("?"));
}

TEST("profiles: the grid dims and darkens on the screen timeout; a touch on it keeps it lit") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online_with_profiles();
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  hg::TouchGestures touch(r.app);
  CHECK(r.app.open_profile_picker());
  r.advance(10000);
  CHECK_EQ(r.fake.brightness, 100);
  // A touch on the grid (here a swipe too short to turn the page) counts as activity.
  touch.update(true, 100, 200, r.fake.clock);
  r.advance(20);
  touch.update(true, 120, 200, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock);
  r.advance(14000);
  CHECK_EQ(r.fake.brightness, 100);  // 14 s after the touch: still lit
  CHECK(r.app.profile_picker_open());
  r.advance(2000);
  CHECK_EQ(r.fake.brightness, 10);  // half the timeout: dimmed, the grid still up
  CHECK(r.app.profile_picker_open());
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 0);  // the whole timeout: dark, and the grid closed
  CHECK(!r.app.profile_picker_open());
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(r.fake.last("profile.select") == nullptr);
}

TEST("profiles: PWR on the grid goes back to Ready with the screen lit; the next press darkens it") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online_with_profiles();
  CHECK(r.app.open_profile_picker());
  r.app.on_power_key();
  CHECK(!r.app.profile_picker_open());
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.fake.last("profile.select") == nullptr);
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.app.profile_picker_open());
}

TEST("profiles: the grid ends with a Settings cell that opens Settings, pages like a profile and is never current") {
  for (int round : {0, 466}) {
    Rig r(Rig::touch_profile());
    if (round) r.fake.make_round(round);
    r.fake.kv["profile_grid"] = "4";
    welcome_with_roster(r, 4, "p1");  // four profiles and Settings: two pages of 2x2
    CHECK(r.app.open_profile_picker());
    const hg::UiModel& m = r.app.model();
    CHECK_EQ(m.settings_row, 4);
    CHECK_EQ(m.dash.size(), size_t(5));
    CHECK_EQ(m.dash[4].label, std::string("Settings"));
    CHECK(m.dash[4].tone != hg::DashRow::Tone::Waiting);  // no ring
    CHECK_EQ(r.app.profile_per_page(), 4);
    CHECK_EQ(row_point(r, 4).first, -1);  // not on the first page
    CHECK(r.app.profile_page_step(1));
    CHECK_EQ(r.app.model().scroll, 4);
    auto [x, y] = row_point(r, 4);
    CHECK(x >= 0);
    hg::TouchGestures touch(r.app);
    touch.update(true, x, y, r.fake.clock);
    r.advance(50);
    touch.update(false, 0, 0, r.fake.clock);
    CHECK(!r.app.profile_picker_open());
    CHECK(r.app.settings_open());
    CHECK(r.fake.last("profile.select") == nullptr);  // no switch asked for
    CHECK_EQ(r.app.agent_profile(), std::string("p1"));
  }
  // In Spanish it reads Ajustes, as the settings menu does.
  Rig es(Rig::touch_profile());
  es.fake.kv["language"] = "es";
  welcome_with_roster(es, 3);
  CHECK(es.app.open_profile_picker());
  CHECK_EQ(es.app.model().dash.back().label, std::string("Ajustes"));
  // Few profiles: Settings takes a cell of the small grid too (three and Settings fill a 2x2).
  Rig few(Rig::touch_profile());
  few.fake.make_panel(410, 486, false);
  welcome_with_roster(few, 3);
  CHECK(few.app.open_profile_picker());
  CHECK_EQ(few.app.profile_per_page(), 4);
}

TEST("profiles: a welcome brings up to 32 profiles; the rest are dropped") {
  Rig r(Rig::touch_profile());
  welcome_with_roster(r, 40);
  r.app.console("settings");
  CHECK(r.app.open_profile_picker());
  CHECK_EQ(r.app.model().dash.size(), hg::App::kMaxProfiles + 1);  // and Settings
  CHECK_EQ(r.app.model().dash[hg::App::kMaxProfiles - 1].value, std::string("p31"));
}

TEST("profiles: profile_grid is 4, 6, 9 or 16, and the settings item steps through them") {
  Rig r(Rig::touch_profile());
  welcome_with_roster(r, 16);
  CHECK_EQ(r.app.console("set profile_grid 5").rfind("@error", 0), size_t(0));
  CHECK_EQ(r.app.console("set profile_grid 9"), std::string("@ok profile_grid"));
  CHECK(r.app.open_profile_picker());
  CHECK_EQ(r.app.model().grid, 9);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);
  r.app.console("settings");
  CHECK(find_setting(r, "Profiles per page"));
  CHECK(r.app.model().body.rfind("3 x 3", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.kv["profile_grid"], std::string("16"));
  CHECK(r.app.model().body.rfind("4 x 4", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.kv["profile_grid"], std::string("4"));

  Rig buttons;  // no touchscreen: no grid, so no item
  buttons.bring_online_with_profiles();
  buttons.app.console("settings");
  CHECK(!find_setting(buttons, "Profiles per page"));
}

TEST("profiles: a reset Hermes sends on its own does not end the wait for a pending switch") {
  Rig r(Rig::touch_profile());
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true,"profile":"ops","profiles":[)"
           R"({"id":"default","name":"Hermes"},{"id":"ops","name":"Ops"},{"id":"lab","name":"Lab"}]})");
  CHECK(r.app.select_agent_profile(2));  // Lab; its grant is slow
  r.server(R"({"type":"profile","profile":"default"})");  // meanwhile Ops is parked and the device reset
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  CHECK(r.app.profile_switch_pending());
  CHECK_EQ(r.app.model().detail, std::string("Switching to Lab..."));
  CHECK(!r.app.open_profile_picker());
  CHECK(!r.app.select_agent_profile(0));
  r.server(R"({"type":"profile","profile":"lab"})");  // the answer to the Lab request
  CHECK(!r.app.profile_switch_pending());
  CHECK_EQ(r.app.agent_profile(), std::string("lab"));
  CHECK(r.app.open_profile_picker());
  int selects = 0;
  for (const Value& v : r.fake.sent) selects += v["type"].as_string() == "profile.select";
  CHECK_EQ(selects, 1);
}

// The language Hermes names in `welcome`, for a device left on auto.
namespace {
void welcome_with_language(Rig& r, const std::string& language) {
  r.app.begin();
  r.app.on_network(true, "test-wifi");
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  std::string welcome = R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true)";
  if (!language.empty()) welcome += R"(,"language":")" + language + "\"";
  r.server(welcome + "}");
}
}  // namespace

TEST("language: auto follows the language Hermes names in welcome") {
  Rig r;
  r.fake.kv["language"] = "auto";
  welcome_with_language(r, "es");
  CHECK(r.app.model().lang == hg::i18n::Lang::Es);
  CHECK_EQ(r.app.model().headline, std::string("Hola, soy Hermes"));
  // A region or script falls back to its language; case and underscores don't matter.
  for (const char* id : {"es-mx", "ES_MX", "es-419"}) {
    Rig other;
    other.fake.kv["language"] = "auto";
    welcome_with_language(other, id);
    CHECK(other.app.model().lang == hg::i18n::Lang::Es);
  }
}

TEST("language: auto is English when Hermes names none, or one this firmware lacks") {
  for (const char* id : {"", "pt-br", "zh-hant", "x"}) {
    Rig r;
    r.fake.kv["language"] = "auto";
    welcome_with_language(r, id);
    CHECK(r.app.model().lang == hg::i18n::Lang::En);
    CHECK_EQ(r.app.model().headline, std::string("Hi, I'm Hermes"));
  }
}

TEST("language: a chosen language ignores Hermes's, and auto picks it up when chosen later") {
  Rig r;  // English by default: welcome doesn't change it
  welcome_with_language(r, "es");
  CHECK(r.app.model().lang == hg::i18n::Lang::En);
  r.fake.kv["language"] = "en";
  CHECK_EQ(r.app.console("set language auto"), std::string("@ok language"));
  CHECK(r.app.model().lang == hg::i18n::Lang::Es);  // remembered from this connection's welcome
  CHECK_EQ(r.app.console("set language en"), std::string("@ok language"));
  CHECK(r.app.model().lang == hg::i18n::Lang::En);
}

TEST("language: a later welcome with another language switches an auto device") {
  Rig r;
  r.fake.kv["language"] = "auto";
  welcome_with_language(r, "es");
  CHECK(r.app.model().lang == hg::i18n::Lang::Es);
  r.app.on_transport_closed("bye");
  r.advance(3000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bjI=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true,"language":"en"})");
  CHECK(r.app.model().lang == hg::i18n::Lang::En);
}

TEST("language: the agent can set auto through screen.language") {
  Rig r;
  welcome_with_language(r, "es");
  r.server(R"({"type":"action","id":"l9","name":"screen.language","args":{"language":"auto"}})");
  const Value* res = r.fake.last("action.result");
  CHECK(res && (*res)["ok"].as_bool() && (*res)["result"]["language"].as_string() == "auto");
  CHECK_EQ(r.fake.kv["language"], std::string("auto"));
  CHECK(r.app.model().lang == hg::i18n::Lang::Es);
}

namespace {
// A face message (docs/protocol.md#faces) whose every bitmap byte is `ink`.
std::string face_message(const std::string& profile, const std::string& hash, uint8_t ink = 0xff,
                         std::vector<int> anchors = {607, 293, 262, 620, 801, 107, 396, 508, 586, 649}) {
  std::vector<uint8_t> data(3 * (512 + 1152 + 2592 + 4608), ink);
  for (int v : anchors) {
    data.push_back(static_cast<uint8_t>(v & 0xff));
    data.push_back(static_cast<uint8_t>(v >> 8));
  }
  return R"({"type":"face","profile":")" + profile + R"(","hash":")" + hash + R"(","bits":")" +
         hg::crypto::base64_encode(data.data(), data.size()) + R"("})";
}
size_t pixels_changed(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
  return n;
}
// Saturday, 24 October 2026 at 09:41 — the wall time behind the clock tests.
const hg::WallTime kWallSaturday{2026, 10, 24, 6, 9, 41, 0};
// Sunday, 25 October 2026 at 00:05 — midnight in both time formats.
const hg::WallTime kWallMidnight{2026, 10, 25, 0, 0, 5, 0};
}  // namespace

TEST("profiles: the names of cells side by side never touch, on the 2.06 and the round 1.75") {
  for (int round : {0, 466}) {
    Rig r(Rig::touch_profile());
    if (round) r.fake.make_round(round);
    else r.fake.make_panel(410, 486);
    r.fake.kv["profile_grid"] = "4";
    // Long one-line names: thirteen letters fit a 2 x 2 cell of the round panel's inscribed square.
    std::string roster;
    for (int i = 0; i < 4; ++i)
      roster += std::string(i ? "," : "") + R"({"id":"p)" + std::to_string(i) + R"(","name":"My Health Bot"})";
    r.app.begin();
    r.app.on_network(true, "wifi");
    r.advance(1000);
    r.app.on_transport_open();
    r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
    r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true,"profile":"p0","profiles":[)" + roster +
             "]}");
    CHECK(r.app.open_profile_picker());
    r.advance(100);
    CHECK_EQ(r.app.profile_per_page(), 4);
    const auto [x0, y0] = row_point(r, 0);
    int edge = x0;  // the first column of the cell to the right of profile 0
    while (edge < r.fake.width && r.app.profile_row_hit(edge, y0) != 1) ++edge;
    CHECK(edge < r.fake.width);
    int top = y0, bottom = y0;
    while (top > 0 && r.app.profile_row_hit(edge - 1, top - 1) == 0) --top;
    while (bottom + 1 < r.fake.height && r.app.profile_row_hit(edge - 1, bottom + 1) == 0) ++bottom;
    // Below the icons, where the names are: two dots either side of the edge stay background, so two
    // names in a row read as two, not as one name with a space in it.
    const int dot = 2;  // both panels draw text at scale 2
    const uint16_t bg = r.fake.fb[static_cast<size_t>(bottom * r.fake.width + edge)];
    int lit = 0;
    for (int y = (top + bottom) / 2; y <= bottom; ++y)
      for (int x = edge - 2 * dot; x < edge + 2 * dot; ++x) lit += r.fake.fb[static_cast<size_t>(y * r.fake.width + x)] != bg;
    if (lit) std::printf("    %s: %d name pixels at the edge between two cells\n", round ? "round" : "2.06", lit);
    CHECK_EQ(lit, 0);
  }
}

TEST("profiles: the current profile's face is its icon in the grid; the default one keeps the mascot otherwise") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles(true, "ops");
  CHECK(r.app.open_profile_picker());
  CHECK_EQ(r.app.model().face_row, -1);  // no face yet: Ops shows its initial
  CHECK_EQ(r.app.model().mascot_row, 0);
  const std::vector<uint16_t> before = r.fake.fb;
  r.server(face_message("ops", "f1"));
  r.advance(100);
  CHECK_EQ(r.app.model().face_row, 1);
  CHECK_EQ(r.app.model().mascot_row, 0);
  CHECK(pixels_changed(before, r.fake.fb) > 100);  // the grid redrew with the face

  Rig d(Rig::touch_profile());
  d.bring_online_with_profiles(true, "default");
  d.server(face_message("default", "f2"));
  CHECK(d.app.open_profile_picker());
  CHECK_EQ(d.app.model().face_row, 0);  // the default profile's own face, not the mascot
  CHECK_EQ(d.app.model().mascot_row, -1);
}

TEST("profiles: after switching away from a profile with a face, its cell shows its initial again") {
  Rig r(Rig::touch_profile());
  r.bring_online_with_profiles(true, "ops");
  r.server(face_message("ops", "f1"));
  CHECK(r.app.open_profile_picker());
  CHECK_EQ(r.app.model().face_row, 1);
  CHECK_EQ(r.app.model().mascot_row, 0);
  CHECK(r.app.select_agent_profile(0));  // back to the default profile, which has no face of its own
  r.server(R"({"type":"profile","profile":"default"})");
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  CHECK(r.app.profile_picker_open() || r.app.open_profile_picker());
  CHECK_EQ(r.app.model().face_row, -1);  // Ops shows its initial, not a face it no longer wears
  CHECK_EQ(r.app.model().mascot_row, 0);
}

TEST("faces: a face replaces the mascot until a face without bits brings it back") {
  Rig r;
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["faces"].as_int() == 1);
  r.advance(100);
  const std::vector<uint16_t> mascot = r.fake.fb;
  r.server(face_message("default", "a1"));
  r.advance(100);
  CHECK(r.app.model().face && r.app.model().face->hash == "a1");
  CHECK(pixels_changed(mascot, r.fake.fb) > 5000);  // a solid square where the mascot was
  r.server(R"({"type":"face","profile":"default"})");
  r.advance(100);
  CHECK(r.app.model().face == nullptr);
  CHECK(pixels_changed(mascot, r.fake.fb) < 300);  // at most a blink apart
}

TEST("faces: a malformed face shows the mascot") {
  Rig r;
  r.bring_online(true);
  r.server(face_message("default", "a1"));
  r.server(R"({"type":"face","profile":"default","hash":"b2","bits":"AAAA"})");
  CHECK(r.app.model().face == nullptr);
  r.server(face_message("default", ""));
  CHECK(r.app.model().face == nullptr);
  r.server(R"({"type":"face","profile":"default","hash":"c3","bits":")" + std::string(400000, 'A') + R"("})");
  CHECK(r.app.model().face == nullptr);
  r.server(face_message("default", "d4", 0xff, {607, 293, 262, 620, 801, 107, 32767, 32767, 586, 649}));
  CHECK(r.app.model().face == nullptr);  // eye rows off the bitmap: the blink would never repaint
  r.server(face_message("default", "e5", 0xff, {607, 293, 262, 620, 801, 107, 508, 396, 586, 649}));
  CHECK(r.app.model().face == nullptr);  // eye rows upside down
  r.server(face_message("default", "f6", 0xff, {-5, 293, 262, 620, 801, 107, 396, 508, 586, 649}));
  CHECK(r.app.model().face == nullptr);
  r.server(face_message("default", "a7", 0xff, {0, 0, 1000, 1000, 0, 1000, 0, 1000, 500, 500}));
  CHECK(r.app.model().face != nullptr);  // the edges are fine
}

TEST("faces: a late face or clear for the previous profile leaves this one's alone") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  r.server(R"({"type":"profile","profile":"default"})");
  r.server(face_message("ops", "a1"));  // sent before the switch, read after it
  CHECK(r.app.model().face == nullptr);
  r.server(face_message("default", "b2"));
  r.server(R"({"type":"face","profile":"ops"})");
  CHECK(r.app.model().face && r.app.model().face->hash == "b2");
}

TEST("faces: an unpaired device shows the mascot and names no face") {
  Rig r;
  r.bring_online(true);
  r.server(face_message("default", "a1"));
  r.server(R"({"type":"unpaired"})");
  CHECK(r.app.model().face == nullptr);
  r.server(face_message("default", "a1"));  // a face still on its way
  CHECK(r.app.model().face == nullptr);

  Rig away;  // revoked while offline: the welcome says so
  away.bring_online(true);
  away.server(face_message("default", "a1"));
  away.app.on_transport_closed("network flap");
  away.advance(5000);
  away.app.on_transport_open();
  away.server(R"({"type":"challenge","nonce":"bjI=","enrolled":true})");
  away.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":false,"face":"a1"})");  // even named
  CHECK(away.app.model().face == nullptr);
  away.fake.sent.clear();
  away.app.on_transport_closed("network flap");
  away.advance(5000);
  away.app.on_transport_open();
  const Value* hello = away.fake.last("hello");
  CHECK(hello && !hello->has("face"));
}

TEST("faces: a board without the memory for a face offers none and ignores one") {
  hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");
  p.faces = false;
  Rig r(p);
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && !(*hello)["caps"].has("faces"));
  r.server(face_message("default", "a1"));
  CHECK(r.app.model().face == nullptr);
}

TEST("faces: a reconnect keeps the face only when the welcome names it") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  // Reconnects with `named` in the welcome; returns the face the hello named.
  auto reconnect = [&](const std::string& named) {
    r.fake.sent.clear();
    r.app.on_transport_closed("network flap");
    r.advance(5000);
    r.app.on_transport_open();
    const Value* hello = r.fake.last("hello");
    const std::string held = hello ? (*hello)["face"].as_string() : "";
    r.server(R"({"type":"challenge","nonce":"bjI=","enrolled":true})");
    r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true,)" + named +
             R"("profile":"ops","profiles":[{"id":"default","name":"Hermes"},{"id":"ops","name":"Ops"}]})");
    return held;
  };
  r.server(face_message("ops", "a1"));
  CHECK_EQ(reconnect(R"("face":"a1",)"), std::string("a1"));
  CHECK(r.app.model().face && r.app.model().face->hash == "a1");  // the same face stays
  CHECK_EQ(reconnect(R"("face":"b2",)"), std::string("a1"));
  CHECK(r.app.model().face == nullptr);  // another one is coming: the mascot meanwhile
  r.server(face_message("ops", "a1"));
  CHECK_EQ(reconnect(""), std::string("a1"));
  CHECK(r.app.model().face == nullptr);  // none named: a new route, or a server without faces
}

TEST("faces: another profile shows the mascot until its own face arrives") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  r.server(face_message("ops", "a1"));
  r.server(R"({"type":"profile","profile":"default","error":"busy"})");  // a refusal keeps it
  CHECK(r.app.model().face != nullptr);
  r.server(R"({"type":"profile","profile":"default"})");
  CHECK(r.app.model().face == nullptr);
  r.server(face_message("default", "b2"));
  CHECK(r.app.model().face && r.app.model().face->hash == "b2");

  // A welcome that names another profile than the face's drops it too.
  r.app.on_transport_closed("network flap");
  r.advance(5000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bjI=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true,"profile":"ops",)"
           R"("profiles":[{"id":"default","name":"Hermes"},{"id":"ops","name":"Ops"}]})");
  CHECK(r.app.model().face == nullptr);
}

TEST("faces: a face made of the mascot's own bitmaps draws the mascot") {
  std::vector<uint8_t> data;
  for (int size : {64, 96, 144, 192})
    for (auto frame : {hg::mascot::Frame::Idle, hg::mascot::Frame::Blink, hg::mascot::Frame::Talk}) {
      const hg::mascot::Bitmap* b = hg::mascot::pick(size, frame);
      data.insert(data.end(), b->bits, b->bits + (size + 7) / 8 * size);
    }
  const hg::mascot::Anchors& a = hg::mascot::anchors();
  for (int v : {a.ear_cup.x, a.ear_cup.y, a.mouth.x, a.mouth.y, a.head_top_right.x, a.head_top_right.y, a.eyes_y0,
                a.eyes_y1, a.mouth_y0, a.mouth_y1}) {
    data.push_back(static_cast<uint8_t>(v & 0xff));
    data.push_back(static_cast<uint8_t>(v >> 8));
  }
  Rig r;
  r.bring_online(true);
  r.advance(100);
  const std::vector<uint16_t> mascot = r.fake.fb;
  r.server(R"({"type":"face","profile":"default","hash":"m1","bits":")" +
           hg::crypto::base64_encode(data.data(), data.size()) + R"("})");
  r.advance(10);
  CHECK(r.app.model().face != nullptr);
  CHECK_EQ(pixels_changed(mascot, r.fake.fb), size_t(0));
}

namespace {

hg::DeviceProfile colour_profile() {
  hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");
  p.colour_face_alloc = std::malloc;
  return p;
}

int colour_px(Rig& r) {
  const Value* hello = r.fake.last("hello");
  return hello ? static_cast<int>((*hello)["caps"]["faces"]["px"].as_int(0)) : 0;
}

// A colour face's face.start (docs/protocol.md#faces); `tail` is everything after the size.
std::string colour_start(const std::string& profile, const std::string& hash, int px, int stream = 7,
                         const std::string& tail = R"("format":"rgb565","anchors":[607,293,262,620,801,107,396,508,586,649])") {
  return R"({"type":"face.start","stream":)" + std::to_string(stream) + R"(,"profile":")" + profile + R"(","hash":")" +
         hash + R"(","width":)" + std::to_string(px) + R"(,"height":)" + std::to_string(px) + "," + tail + "}";
}

// Bytes [from, to) of a colour face's RGB565 on binary channel 4, in 4096-byte chunks. Every pixel is
// `pixel` except a transparent (0x0000) square in the middle. Chunk i (counted from the face's first
// byte) goes with sequence number `seq(i)`, i itself by default.
void push_pixel(std::vector<uint8_t>& data, uint16_t v) {
  data.push_back(static_cast<uint8_t>(v & 0xff));
  data.push_back(static_cast<uint8_t>(v >> 8));
}

std::vector<uint8_t> idle_pixels(int px, uint16_t pixel) {
  std::vector<uint8_t> data;
  for (int y = 0; y < px; ++y)
    for (int x = 0; x < px; ++x) {
      const bool hole = x >= px / 4 && x < 3 * px / 4 && y >= px / 4 && y < 3 * px / 4;
      push_pixel(data, hole ? 0 : pixel);
    }
  return data;
}

// Bytes [from, to) of `data` on binary channel 4, in 4096-byte chunks, as send_pixels sends them.
void send_bytes(Rig& r, const std::vector<uint8_t>& data, int stream = 7, size_t from = 0, size_t to = SIZE_MAX,
                uint16_t (*seq)(uint16_t) = nullptr) {
  to = std::min(to, data.size());
  for (size_t off = from; off < to; off += 4096) {
    const size_t len = std::min<size_t>(4096, to - off);
    const auto i = static_cast<uint16_t>(off / 4096);
    std::vector<uint8_t> frame(hg::proto::kBinaryHeader + len);
    hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Face, static_cast<uint8_t>(stream),
                                   seq ? seq(i) : i);
    std::copy(data.begin() + static_cast<long>(off), data.begin() + static_cast<long>(off + len),
              frame.begin() + hg::proto::kBinaryHeader);
    r.app.on_transport_binary(frame.data(), frame.size());
  }
}

void send_pixels(Rig& r, int px, uint16_t pixel, int stream = 7, size_t from = 0, size_t to = SIZE_MAX,
                 uint16_t (*seq)(uint16_t) = nullptr) {
  send_bytes(r, idle_pixels(px, pixel), stream, from, to, seq);
}

std::string colour_end(int stream = 7) { return R"({"type":"face.end","stream":)" + std::to_string(stream) + "}"; }

// A colour face as the server sends it: face.start, the pixels, face.end. Only the first `send` bytes
// go; `end` false leaves out face.end.
void send_colour_face(Rig& r, const std::string& profile, const std::string& hash, int px, uint16_t pixel,
                      size_t send = SIZE_MAX, bool end = true, int stream = 7) {
  r.server(colour_start(profile, hash, px, stream));
  send_pixels(r, px, pixel, stream, 0, send);
  if (end) r.server(colour_end(stream));
}

size_t count_px(const std::vector<uint16_t>& fb, uint16_t v) {
  return static_cast<size_t>(std::count(fb.begin(), fb.end(), v));
}

constexpr uint16_t kRose = hg::rgb565(220, 60, 120);

}  // namespace

TEST("colour faces: a board with room offers its Ready face size and draws the face in colour") {
  Rig r(colour_profile());
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["faces"]["v"].as_int() == 2);
  const int px = colour_px(r);
  CHECK(px >= 64 && px <= 192);
  r.advance(100);
  send_colour_face(r, "default", "c1", px, kRose);
  r.advance(100);
  CHECK(r.app.model().face && r.app.model().face->hash == "c1");
  // The ring of colour shows; the transparent middle shows the background, not black.
  CHECK_EQ(count_px(r.fake.fb, kRose), static_cast<size_t>(px * px - (px / 2) * (px / 2)));
  r.server(R"({"type":"face","profile":"default"})");
  r.advance(100);
  CHECK(r.app.model().face == nullptr);
  CHECK_EQ(count_px(r.fake.fb, kRose), size_t(0));
}

TEST("colour faces: a board without room keeps one-colour faces") {
  Rig r;  // no colour_face_alloc: a board without PSRAM to spare
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["faces"].as_int() == 1);
  send_colour_face(r, "default", "c1", 144, kRose);
  CHECK(r.app.model().face == nullptr);

  hg::DeviceProfile full = colour_profile();  // PSRAM with no room left after the framebuffer
  full.colour_face_alloc = [](size_t) -> void* { return nullptr; };
  Rig no_room(full);
  no_room.bring_online(true);
  hello = no_room.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["faces"].as_int() == 1);
  no_room.server(face_message("default", "a1"));
  CHECK(no_room.app.model().face && no_room.app.model().face->hash == "a1");  // one-colour faces still
}

TEST("colour faces: a bad start or a broken transfer leaves the mascot") {
  Rig r(colour_profile());
  r.bring_online_with_profiles(true, "default");
  const int px = colour_px(r);
  send_colour_face(r, "default", "c1", px, kRose);
  CHECK(r.app.model().face != nullptr);
  r.server(colour_start("default", "c2", px + 1));  // not the size the device asked for,
  send_pixels(r, px, kRose);                        // even when the device's size follows
  r.server(colour_end());
  CHECK(r.app.model().face == nullptr);
  send_colour_face(r, "default", "", px, kRose);  // no hash
  CHECK(r.app.model().face == nullptr);
  send_colour_face(r, "default", "c3", px, kRose, 4096 * 3);  // cut short, then face.end
  CHECK(r.app.model().face == nullptr);
  send_colour_face(r, "default", "c4", px, kRose, SIZE_MAX, false);  // no face.end
  CHECK(r.app.model().face == nullptr);
  r.server(R"({"type":"face.end","stream":8})");  // another stream's end
  CHECK(r.app.model().face == nullptr);
  r.server(R"({"type":"face.end","stream":7})");
  CHECK(r.app.model().face && r.app.model().face->hash == "c4");

  // Anchors off the face, or not ten of them, or another format: refused like a malformed one-bit face,
  // even when a whole face follows on the start's stream.
  for (const char* bad : {R"("anchors":[607,293,262,620,801,107,508,396,586,649],"format":"rgb565")",
                          R"("anchors":[607,293,262,620,801,107,396,508,586],"format":"rgb565")",
                          R"("anchors":[607,293,262,620,801,107,396,508,586,1001],"format":"rgb565")",
                          R"("anchors":[607,293,262,620,801,107,396,508,586,649],"format":"jpeg")"}) {
    send_colour_face(r, "default", "c5", px, kRose);
    r.server(colour_start("default", "c6", px, 9, bad));
    CHECK(r.app.model().face == nullptr);
    send_pixels(r, px, kRose, 9);
    r.server(colour_end(9));
    CHECK(r.app.model().face == nullptr);
  }

  // A chunk skipped or sent twice drops the transfer, even when the bytes add up to a whole face.
  for (uint16_t (*seq)(uint16_t) : {+[](uint16_t i) -> uint16_t { return i < 2 ? i : i + 1; },
                                    +[](uint16_t i) -> uint16_t { return i < 2 ? i : i - 1; }}) {
    r.server(colour_start("default", "c9", px));
    send_pixels(r, px, kRose, 7, 0, SIZE_MAX, seq);
    r.server(colour_end());
    CHECK(r.app.model().face == nullptr);
  }

  // One byte too many drops the transfer.
  send_colour_face(r, "default", "c7", px, kRose, SIZE_MAX, false);
  std::vector<uint8_t> extra(hg::proto::kBinaryHeader + 2, 0);
  const auto next_seq = static_cast<uint16_t>((px * px * 2 + 4095) / 4096);
  hg::proto::write_binary_header(extra.data(), hg::proto::Channel::Face, 7, next_seq);
  r.app.on_transport_binary(extra.data(), extra.size());
  r.server(R"({"type":"face.end","stream":7})");
  CHECK(r.app.model().face == nullptr);

  // Another profile's face, late after a switch, is not this profile's.
  send_colour_face(r, "ops", "c8", px, kRose);
  CHECK(r.app.model().face == nullptr);
}

TEST("colour faces: a new start replaces a transfer, and nothing resumes one that was cut off") {
  constexpr uint16_t kTeal = hg::rgb565(40, 180, 170);
  Rig r(colour_profile());
  r.bring_online(true);
  const int px = colour_px(r);
  const size_t half = static_cast<size_t>(px * px) / 4096 * 4096;  // whole chunks, about half the face

  // A new face takes over mid-way; the old stream's late chunks and end change nothing.
  r.server(colour_start("default", "c1", px, 7));
  send_pixels(r, px, kRose, 7, 0, half);
  r.server(colour_start("default", "c2", px, 8));
  send_pixels(r, px, kRose, 7, half);  // the rest of c1, late
  r.server(colour_end(7));
  CHECK(r.app.model().face == nullptr);
  send_pixels(r, px, kTeal, 8);
  r.server(colour_end(8));
  CHECK(r.app.model().face && r.app.model().face->hash == "c2");
  r.advance(100);
  CHECK_EQ(count_px(r.fake.fb, kRose), size_t(0));
  CHECK_EQ(count_px(r.fake.fb, kTeal), static_cast<size_t>(px * px - (px / 2) * (px / 2)));

  // A reconnect cuts a transfer: its remaining chunks and end on the new connection install nothing.
  r.server(colour_start("default", "c3", px));
  send_pixels(r, px, kRose, 7, 0, half);
  r.app.on_transport_closed("network flap");
  r.advance(5000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bjI=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true,"face":"c2"})");
  send_pixels(r, px, kRose, 7, half);
  r.server(colour_end());
  CHECK(r.app.model().face == nullptr);

  // So does an unpair, even when the device is paired again before the rest arrives.
  r.server(colour_start("default", "c4", px));
  send_pixels(r, px, kRose, 7, 0, half);
  r.server(R"({"type":"unpaired"})");
  r.server(R"({"type":"paired"})");
  send_pixels(r, px, kRose, 7, half);
  r.server(colour_end());
  CHECK(r.app.model().face == nullptr);
}

TEST("colour faces: stream ids, sizes and anchors must be whole numbers in range") {
  Rig r(colour_profile());
  r.bring_online(true);
  const int px = colour_px(r);
  const std::string size = std::to_string(px);
  const std::string anchors = R"("anchors":[607,293,262,620,801,107,396,508,586,649])";
  // Each start is refused, so the whole face that follows on stream 7 never shows.
  for (const std::string& bad : {
           R"({"type":"face.start","stream":7.5,"profile":"default","hash":"c1","width":)" + size + R"(,"height":)" +
               size + R"(,"format":"rgb565",)" + anchors + "}",
           R"({"type":"face.start","stream":1e100,"profile":"default","hash":"c1","width":)" + size + R"(,"height":)" +
               size + R"(,"format":"rgb565",)" + anchors + "}",
           R"({"type":"face.start","stream":7,"profile":"default","hash":"c1","width":)" + size + R"(.5,"height":)" +
               size + R"(,"format":"rgb565",)" + anchors + "}",
           colour_start("default", "c1", px, 7, R"("format":"rgb565","anchors":[607.5,293,262,620,801,107,396,508,586,649])"),
           colour_start("default", "c1", px, 7, R"("format":"rgb565","anchors":[1e100,293,262,620,801,107,396,508,586,649])")}) {
    r.server(bad);
    send_pixels(r, px, kRose);
    r.server(colour_end());
    CHECK(r.app.model().face == nullptr);
  }
  // face.end names its stream exactly too.
  r.server(colour_start("default", "c2", px));
  send_pixels(r, px, kRose);
  r.server(R"({"type":"face.end","stream":7.5})");
  CHECK(r.app.model().face == nullptr);
  r.server(colour_end());
  CHECK(r.app.model().face && r.app.model().face->hash == "c2");
}

TEST("colour faces: a switch mid-transfer shows the mascot, and a reconnect keeps a named face") {
  Rig r(colour_profile());
  r.bring_online_with_profiles(true, "ops");
  const int px = colour_px(r);
  send_colour_face(r, "ops", "c1", px, kRose, SIZE_MAX, false);
  r.server(R"({"type":"profile","profile":"default"})");
  r.server(R"({"type":"face.end","stream":7})");
  CHECK(r.app.model().face == nullptr);
  send_colour_face(r, "default", "c2", px, kRose);
  CHECK(r.app.model().face && r.app.model().face->hash == "c2");

  r.fake.sent.clear();
  r.app.on_transport_closed("network flap");
  r.advance(5000);
  r.app.on_transport_open();
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["face"].as_string() == "c2");
  r.server(R"({"type":"challenge","nonce":"bjI=","enrolled":true})");
  r.server(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":true,"face":"c2"})");
  CHECK(r.app.model().face && r.app.model().face->hash == "c2");
  r.advance(100);
  CHECK(count_px(r.fake.fb, kRose) > 0);

  // A one-colour face file replaces it, and a colour face replaces that.
  r.server(face_message("default", "a1"));
  CHECK(r.app.model().face && r.app.model().face->hash == "a1" && !r.app.model().face->rgb565);
  send_colour_face(r, "default", "c3", px, kRose);
  CHECK(r.app.model().face && r.app.model().face->hash == "c3" && r.app.model().face->rgb565);

  // A transfer cut by a reconnect is not named in the hello.
  send_colour_face(r, "default", "c4", px, kRose, 4096, false);
  r.fake.sent.clear();
  r.app.on_transport_closed("network flap");
  r.advance(5000);
  r.app.on_transport_open();
  hello = r.fake.last("hello");
  CHECK(hello && !hello->has("face"));
}

namespace {

constexpr uint16_t kTeal = hg::rgb565(40, 180, 170);
constexpr uint16_t kAmber = hg::rgb565(240, 160, 40);
const std::string kAnchors = R"("format":"rgb565","anchors":[607,293,262,620,801,107,396,508,586,649])";

// A moving colour face's pixels: the idle picture send_pixels sends, then `blink_rows` rows of kTeal, then
// `talk_rows` rows of kAmber.
std::vector<uint8_t> moving_pixels(int px, int blink_rows, int talk_rows) {
  std::vector<uint8_t> data = idle_pixels(px, kRose);
  for (int i = 0; i < blink_rows * px; ++i) push_pixel(data, kTeal);
  for (int i = 0; i < talk_rows * px; ++i) push_pixel(data, kAmber);
  return data;
}

}  // namespace

TEST("colour frames: a board with room for the bands offers frames; one with room for the still face only does not") {
  Rig r(colour_profile());
  r.bring_online(true);
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["faces"]["frames"].as_bool());

  hg::DeviceProfile still = colour_profile();  // the 320x240 fake draws the face at 144: room for that alone
  still.colour_face_alloc = [](size_t n) -> void* { return n > 144 * 144 * 2 ? nullptr : std::malloc(n); };
  Rig s(still);
  s.bring_online(true);
  hello = s.fake.last("hello");
  CHECK(hello && (*hello)["caps"]["faces"]["v"].as_int() == 2 && (*hello)["caps"]["faces"]["px"].as_int() == 144);
  CHECK(hello && !(*hello)["caps"]["faces"].has("frames"));
  s.server(colour_start("default", "m1", 144, 7, kAnchors + R"(,"blink":[10,4])"));  // refused: no room for it
  send_bytes(s, moving_pixels(144, 4, 0));
  s.server(colour_end());
  CHECK(s.app.model().face == nullptr);
  send_colour_face(s, "default", "c1", 144, kRose);
  CHECK(s.app.model().face && s.app.model().face->hash == "c1");
}

TEST("colour frames: face.start names the bands, and the device takes exactly their bytes") {
  Rig r(colour_profile());
  r.bring_online(true);
  const int px = colour_px(r);
  const std::string bands = R"(,"blink":[10,6],"talk":[100,4])";
  const std::vector<uint8_t> whole = moving_pixels(px, 6, 4);
  auto start = [&](const std::string& hash, const std::string& tail) {
    r.server(colour_start("default", hash, px, 7, kAnchors + tail));
  };

  start("m1", bands);
  send_bytes(r, whole);
  r.server(colour_end());
  const hg::mascot::Face* f = r.app.model().face;
  CHECK(f && f->hash == "m1");
  CHECK(f && f->blink.y == 10 && f->blink.rows == 6 && f->talk.y == 100 && f->talk.rows == 4);
  CHECK(f && f->blink.rgb565[0] == kTeal && f->blink.rgb565[6 * px - 1] == kTeal);
  CHECK(f && f->talk.rgb565[0] == kAmber && f->talk.rgb565[4 * px - 1] == kAmber);

  // Short of the bands, or a byte past them: dropped.
  for (size_t len : {static_cast<size_t>(px * px * 2), whole.size() - 2 * px}) {
    start("m2", bands);
    send_bytes(r, whole, 7, 0, len);
    r.server(colour_end());
    CHECK(r.app.model().face == nullptr);
  }
  start("m3", bands);
  std::vector<uint8_t> extra = whole;
  extra.push_back(0);
  send_bytes(r, extra);
  r.server(colour_end());
  CHECK(r.app.model().face == nullptr);

  // A band that is not two whole numbers inside the face, or bands past half the face's rows together:
  // the start is refused, so even the bytes it names, sent after it, show nothing.
  const std::string half = std::to_string(px / 2);
  for (auto [bad, rows] : std::initializer_list<std::pair<std::string, int>>{
           {R"(,"blink":[10,0])", 0}, {R"(,"blink":[)" + std::to_string(px) + ",1]", 1},
           {R"(,"blink":[)" + std::to_string(px - 3) + ",4]", 4}, {R"(,"blink":[-1,4])", 4},
           {R"(,"blink":[10.5,4])", 4}, {R"(,"blink":[10,4,1])", 4}, {R"(,"blink":"10,4")", 4}, {R"(,"blink":[10])", 0},
           {R"(,"blink":[0,)" + half + R"(],"talk":[100,1])", px / 2 + 1}}) {
    start("m4", bad);
    send_bytes(r, moving_pixels(px, rows, 0));
    r.server(colour_end());
    CHECK(r.app.model().face == nullptr);
  }
  start("m5", R"(,"blink":[0,)" + std::to_string(px / 2) + "]");  // half the rows exactly: fits
  send_bytes(r, moving_pixels(px, px / 2, 0));
  r.server(colour_end());
  CHECK(r.app.model().face && r.app.model().face->blink.rows == px / 2);

  // Either band alone.
  start("m6", R"(,"talk":[100,4])");
  send_bytes(r, moving_pixels(px, 0, 4));
  r.server(colour_end());
  f = r.app.model().face;
  CHECK(f && f->blink.rows == 0 && f->talk.rows == 4 && f->talk.rgb565[0] == kAmber);
}

TEST("colour frames: the device blinks a colour face that has a blink band, and never one without") {
  Rig r(colour_profile());
  r.bring_online(true);
  const int px = colour_px(r);
  auto blinks = [&]() {
    int seen = 0;
    for (int i = 0; i < 90; ++i) {
      r.advance(100);
      seen += count_px(r.fake.fb, kTeal) > 0;
    }
    return seen;
  };
  r.server(colour_start("default", "m1", px, 7, kAnchors + R"(,"blink":[30,6])"));
  send_bytes(r, moving_pixels(px, 6, 0));
  r.server(colour_end());
  const int with_band = blinks();
  CHECK(with_band > 0 && with_band <= 6);  // two frames in every 45, as the mascot blinks
  r.server(colour_start("default", "m2", px, 7, kAnchors + R"(,"talk":[30,6])"));
  send_bytes(r, moving_pixels(px, 0, 6));
  r.server(colour_end());
  CHECK(r.app.model().face && r.app.model().face->hash == "m2");
  CHECK_EQ(blinks(), 0);
  CHECK_EQ(count_px(r.fake.fb, kAmber), size_t(0));  // not speaking: the mouth stays shut
}

TEST("clock: off by default, a long rest keeps the Ready screen") {
  Rig r;
  r.fake.backlight = true;
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  r.bring_online(true);
  CHECK(!r.app.clock_enabled());
  r.advance(40000);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);
}

TEST("clock: the console checks clock and time_format, and both survive a restart") {
  Rig r;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set clock 60"), std::string("@ok clock"));
  CHECK(r.app.clock_enabled());
  CHECK_EQ(r.app.console("get clock"), std::string(R"(@value {"key":"clock","value":"60"})"));
  CHECK_EQ(r.app.console("set clock 4"), std::string("@error clock must be 0 (off) or 5..3600 seconds"));
  CHECK_EQ(r.app.console("set clock 3601"), std::string("@error clock must be 0 (off) or 5..3600 seconds"));
  CHECK_EQ(r.app.console("set clock soon"), std::string("@error clock must be 0 (off) or 5..3600 seconds"));
  CHECK_EQ(r.app.console("set clock 0"), std::string("@ok clock"));
  CHECK(!r.app.clock_enabled());
  CHECK_EQ(r.app.console("set clock 3600"), std::string("@ok clock"));
  CHECK_EQ(r.app.console("set time_format 12h"), std::string("@ok time_format"));
  CHECK_EQ(r.app.console("set time_format 25h"), std::string("@error time_format must be 12h or 24h"));
  CHECK_EQ(r.app.console("set time_format hour"), std::string("@error time_format must be 12h or 24h"));
  hg::Hal hal = r.fake.hal();
  hg::App again(hal, Rig::profile("ws://hermes.local:8765/gadget"));
  again.begin();
  CHECK(again.clock_enabled());
  CHECK_EQ(again.console("get clock"), std::string(R"(@value {"key":"clock","value":"3600"})"));
  CHECK_EQ(again.console("get time_format"), std::string(R"(@value {"key":"time_format","value":"12h"})"));
}

TEST("clock: it needs the time, and the settings body says so until then") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  r.advance(10000);  // no time of day yet
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Clock face"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Clock face"));
  CHECK_EQ(r.app.model().body, std::string("5 seconds\nThe clock starts once the gadget knows the time."));
  r.app.close_settings();
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  // It also needs a paired session: the pairing screen keeps it away.
  Rig pairing;
  pairing.fake.backlight = true;
  pairing.fake.wall = kWallSaturday;
  pairing.fake.wall_ok = true;
  pairing.bring_online(false);
  CHECK_EQ(pairing.app.console("set clock 5"), std::string("@ok clock"));
  pairing.advance(10000);
  CHECK(pairing.app.screen() == hg::Screen::Pairing);
  CHECK(!pairing.app.clock_showing());  // no dim face behind the pairing screen
  CHECK_EQ(pairing.fake.brightness, 100);
  // A board without a display never runs the clock at all.
  Rig blind;
  blind.hal.display = nullptr;
  blind.bring_online(true);
  blind.fake.wall = kWallSaturday;
  blind.fake.wall_ok = true;
  CHECK_EQ(blind.app.console("set clock 5"), std::string("@ok clock"));
  blind.advance(6000);
  CHECK(!blind.app.clock_showing());
  CHECK(blind.app.status_json().find("\"screen\":\"clock\"") == std::string::npos);
}

TEST("clock: after a rest it shows at 70% of the brightness setting") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.fake.brightness, 70);
  CHECK_EQ(r.app.model().clock_time, std::string("09:41"));
  CHECK_EQ(r.app.model().clock_date, std::string("SAT OCT 24"));
  CHECK_EQ(int(r.app.model().clock_minute), 41);
  CHECK_EQ(int(r.app.model().clock_second), 0);  // kWallSaturday's second: the dot's place
  r.fake.wall = hg::WallTime{2026, 10, 24, 6, 9, 41, 42};  // the next second, the same minute
  r.advance(200);
  CHECK_EQ(int(r.app.model().clock_second), 42);  // the ring's dot moves every second
  CHECK_EQ(r.app.model().clock_time, std::string("09:41"));  // the face's text does not
  CHECK(r.app.status_json().find("\"screen\":\"clock\"") != std::string::npos);
  // A lit clock is a lit screen: Hermes is never told it is dark.
  const Value* dark = last_display_state(r.fake);
  CHECK(!dark || !(*dark)["display_sleeping"].as_bool());
  // A very low brightness setting keeps a floor of 5.
  Rig dim;
  dim.fake.backlight = true;
  dim.bring_online(true);
  dim.fake.wall = kWallSaturday;
  dim.fake.wall_ok = true;
  CHECK_EQ(dim.app.console("set brightness 5"), std::string("@ok brightness"));
  CHECK_EQ(dim.app.console("set clock 5"), std::string("@ok clock"));
  dim.advance(6000);
  CHECK(dim.app.screen() == hg::Screen::Clock);
  CHECK_EQ(dim.fake.brightness, 5);
}

TEST("clock: a tap ends it and records nothing; the name opens nothing; a hold talks; TALK records") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online_with_profiles();
  const auto [nx, ny] = name_point(r);  // where the name sits on Ready, before any clock
  CHECK(nx >= 0);
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  // In tap mode a quick tap on Ready records, so a tap that does nothing proves the swallow.
  CHECK_EQ(r.app.console("set talk_mode tap"), std::string("@ok talk_mode"));
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.fake.brightness, 70);
  hg::TouchGestures touch(r.app);
  // A quick tap brings Ready back at full brightness and records nothing.
  touch.update(true, 160, 120, r.fake.clock);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);
  touch.update(false, 0, 0, r.fake.clock + 30);
  CHECK(r.fake.last("audio.start") == nullptr);
  CHECK(!r.fake.mic_on);
  // The name is no target on the clock: a tap there opens nothing either.
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  touch.update(true, nx, ny, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock + 30);
  CHECK(!r.app.profile_picker_open());
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.app.console("set talk_mode hold"), std::string("@ok talk_mode"));
  // The TALK button ends the clock and the press goes on, as on Ready.
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  CHECK(r.fake.mic_on);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.start") != nullptr);
  r.server(R"({"type":"reply","text":"Done."})");
  for (int i = 0; i < 40 && r.app.screen() != hg::Screen::Clock; ++i) r.advance(1000);  // reply, linger, rest
  CHECK(r.app.screen() == hg::Screen::Clock);
  // A hold on the clock still talks: the touch-down ends the clock, the hold records.
  touch.update(true, 160, 120, r.fake.clock);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.advance(200);
  touch.tick(r.fake.clock);
  CHECK(r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Listening);
  touch.update(false, 0, 0, r.fake.clock);
}

TEST("clock: it gives way to whatever needs the screen, and comes back after a new rest") {
  struct Battery : hg::Power {
    std::optional<hg::PowerStatus> status = hg::PowerStatus{true, 3850, 65, false, false};
    std::optional<hg::PowerStatus> read() override { return status; }
    bool power_off() override { return true; }
  } battery;
  Rig r(Rig::motion_profile());
  r.hal.power = &battery;
  r.fake.backlight = true;
  r.bring_online(true);
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  CHECK_EQ(r.app.console("set face_down_sleep on"), std::string("@ok face_down_sleep"));
  const auto showing = [&] {
    for (int i = 0; i < 40 && r.app.screen() != hg::Screen::Clock; ++i) r.advance(1000);
    CHECK(r.app.screen() == hg::Screen::Clock);
  };
  showing();
  // A card shown until dismissed
  r.server(R"({"type":"display","title":"Timer","body":"Pasta: 9 min","ttl_s":0})");
  r.advance(200);
  CHECK(r.app.screen() == hg::Screen::Card);
  CHECK(!r.app.clock_showing());  // no dim face behind it
  CHECK_EQ(r.fake.brightness, 100);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);  // dismisses the card
  showing();
  // A reply, spoken and then read
  r.server(R"({"type":"reply","text":"It is sunny."})");
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.in_turn());
  r.fake.spk_draining = true;
  r.advance(3000);
  CHECK(r.app.screen() != hg::Screen::Clock);
  r.fake.spk_draining = false;
  r.server(R"({"type":"audio.end","stream":3})");
  showing();
  // A prompt
  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"rm -rf build"})");
  CHECK(r.app.screen() == hg::Screen::Prompt);
  r.server(R"({"type":"prompt.close","id":"q1"})");
  showing();
  // A turn that stays thinking, and the hint its cancel flashes
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.advance(400);  // long enough to be sent
  r.app.on_button(hg::Button::Talk, false);
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.advance(6000);  // past the rest: the turn still holds the screen, lit
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK(!r.app.clock_showing());
  CHECK_EQ(r.fake.brightness, 100);
  r.app.on_button(hg::Button::Cancel, true);
  r.app.on_button(hg::Button::Cancel, false);  // cancelled: a hint flashes
  r.advance(2000);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.advance(4000);
  CHECK(r.app.screen() == hg::Screen::Ready);  // the hint flash holds the rest off
  showing();
  // A timer, and the alarm it becomes (never drawn at the clock's dim level)
  r.server(R"({"type":"action","id":"t1","name":"timer.start","args":{"seconds":1,"label":"Tea"}})");
  r.advance(200);
  CHECK(r.app.screen() != hg::Screen::Clock);
  r.advance(1500);
  CHECK(r.app.screen() == hg::Screen::Alarm);
  CHECK_EQ(r.fake.brightness, 100);  // the alarm wakes the screen, so never 70%
  r.app.on_button(hg::Button::Talk, true);  // dismisses it
  r.app.on_button(hg::Button::Talk, false);
  showing();
  // The settings menu
  CHECK(r.app.open_settings());
  r.advance(6000);  // a held-open menu keeps the clock off it
  CHECK(r.app.screen() == hg::Screen::Settings);
  CHECK(!r.app.clock_showing());
  CHECK_EQ(r.fake.brightness, 100);
  r.app.close_settings();
  showing();
  // A notice
  r.server(R"({"type":"notice","text":"Back soon","ttl_s":1})");
  r.advance(200);
  CHECK(r.app.screen() != hg::Screen::Clock);
  showing();
  // The low-battery caption (10% or less, not on USB)
  battery.status = hg::PowerStatus{true, 3600, 8, false, false};
  r.advance(5100);  // the next power reading
  CHECK(r.app.screen() == hg::Screen::Ready);
  battery.status = hg::PowerStatus{true, 3850, 65, false, false};
  r.advance(5100);
  showing();
  // Face down
  r.hold_still(0, 0, -1000, 1200);
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(r.app.screen() != hg::Screen::Clock);
  r.hold_still(0, 0, 1000, 500);  // turn it back over
  CHECK_EQ(r.fake.brightness, 100);
  showing();
  // A dropped connection ends a showing clock; its screens take over, lit.
  r.app.on_transport_closed("bye");
  r.advance(300);
  CHECK(!r.app.clock_showing());
  CHECK_EQ(r.fake.brightness, 100);
  // A fatal server error keeps the Error screen up, also lit, with no clock under it.
  Rig err;
  err.fake.backlight = true;
  err.bring_online(true);
  err.fake.wall = kWallSaturday;
  err.fake.wall_ok = true;
  CHECK_EQ(err.app.console("set clock 5"), std::string("@ok clock"));
  err.advance(6000);
  CHECK(err.app.screen() == hg::Screen::Clock);
  err.server(R"({"type":"error","code":"protocol","message":"bad handshake"})");
  CHECK(err.app.screen() == hg::Screen::Error);
  err.advance(6000);
  CHECK(err.app.screen() == hg::Screen::Error);
  CHECK(!err.app.clock_showing());
  CHECK_EQ(err.fake.brightness, 100);
}

TEST("clock: rest counts from the end of the speech and of the reply text") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  CHECK_EQ(r.app.console("set clock 10"), std::string("@ok clock"));
  r.server(R"({"type":"reply","text":"It is sunny."})");
  r.server(R"({"type":"audio.start","stream":3,"rate":16000,"format":"pcm16"})");
  CHECK(r.app.in_turn());
  r.fake.spk_draining = true;  // the speech is still playing
  r.advance(15000);
  CHECK(r.app.screen() != hg::Screen::Clock);
  r.fake.spk_draining = false;
  r.server(R"({"type":"audio.end","stream":3})");
  r.advance(100);
  CHECK(!r.app.in_turn());
  // The reply text stays up for 20 s; the clock's own 10 s never start under it.
  r.advance(9000);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.advance(11000);  // the linger runs out here
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.advance(11000);  // and only now the 10 s of rest have passed
  CHECK(r.app.screen() == hg::Screen::Clock);
}

TEST("clock: the screen timeout dims and darkens it, and what wakes it comes back at the setting") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  CHECK_EQ(r.app.console("set clock 10"), std::string("@ok clock"));
  CHECK_EQ(r.app.console("set screen_timeout 60"), std::string("@ok screen_timeout"));
  r.advance(11000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.fake.brightness, 70);
  r.advance(20000);  // half of the timeout: dim
  CHECK_EQ(r.fake.brightness, 10);
  // The session sits silent for over a minute: keep the heartbeat fed.
  r.server(R"({"type":"ping"})");
  r.advance(30000);  // the timeout: dark, and the dark ends the clock
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(r.app.screen() == hg::Screen::Ready);
  const Value* dark = last_display_state(r.fake);
  CHECK(dark && (*dark)["display_sleeping"].as_bool());
  r.app.on_button(hg::Button::Talk, true);  // the next input wakes to Ready
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.app.on_button(hg::Button::Talk, false);
  // A clock that starts on a dimmed screen keeps the dim level.
  CHECK_EQ(r.app.console("set screen_timeout 40"), std::string("@ok screen_timeout"));
  CHECK_EQ(r.app.console("set clock 25"), std::string("@ok clock"));
  r.server(R"({"type":"ping"})");
  r.advance(21000);
  CHECK_EQ(r.fake.brightness, 10);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.advance(5000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.fake.brightness, 10);
  r.advance(15000);  // the timeout darkens the dimmed clock
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.app.on_button(hg::Button::Talk, false);
  // The PWR key turns a lit clock's screen off; the next press wakes to Ready.
  CHECK_EQ(r.app.console("set screen_timeout 3600"), std::string("@ok screen_timeout"));
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.fake.brightness, 70);
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 0);
  r.advance(100);
  CHECK(r.app.screen() != hg::Screen::Clock);
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  // A settings change on a dark screen lights it at the brightness setting.
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 0);
  CHECK_EQ(r.app.console("set volume 40"), std::string("@ok volume"));
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("clock: a screen.brightness action on the clock keeps 70% of the new level") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.fake.brightness, 70);
  r.server(R"({"type":"action","id":"b1","name":"screen.brightness","args":{"percent":50}})");
  CHECK_EQ(r.fake.brightness, 35);
  CHECK(r.app.screen() == hg::Screen::Clock);
}

TEST("clock: the settings item cycles Off, 30 s, 1 min, 2 min and 5 min") {
  Rig r;
  r.fake.backlight = true;
  r.fake.wall = kWallSaturday;
  r.fake.wall_ok = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 20 && r.app.model().detail != "Clock face"; ++i) r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().detail, std::string("Clock face"));
  CHECK_EQ(r.app.model().body, std::string("Off\nSelect to show a clock after a rest."));
  const auto select = [&] {
    r.app.on_button(hg::Button::Talk, true);
    r.app.on_button(hg::Button::Talk, false);
  };
  select();
  CHECK_EQ(r.fake.kv["clock"], std::string("30"));
  CHECK_EQ(r.app.model().body, std::string("30 seconds\nA dim clock appears after this much rest."));
  select();
  CHECK_EQ(r.fake.kv["clock"], std::string("60"));
  select();
  CHECK_EQ(r.fake.kv["clock"], std::string("120"));
  select();
  CHECK_EQ(r.fake.kv["clock"], std::string("300"));
  CHECK_EQ(r.app.model().body, std::string("300 seconds\nA dim clock appears after this much rest."));
  select();
  CHECK_EQ(r.fake.kv["clock"], std::string("0"));
  CHECK_EQ(r.app.model().body, std::string("Off\nSelect to show a clock after a rest."));
}

TEST("clock: the face's strings in 12- and 24-hour time and in Spanish") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  r.fake.wall_ok = true;
  CHECK_EQ(r.app.console("set clock 5"), std::string("@ok clock"));
  CHECK_EQ(r.app.console("set time_format 12h"), std::string("@ok time_format"));
  r.fake.wall = kWallSaturday;  // Saturday 09:41
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.app.model().clock_time, std::string("9:41"));
  CHECK_EQ(r.app.model().clock_date, std::string("SAT OCT 24"));
  r.fake.wall = kWallMidnight;  // Sunday 00:05
  r.advance(200);
  CHECK_EQ(r.app.model().clock_time, std::string("12:05"));
  CHECK_EQ(r.app.model().clock_date, std::string("SUN OCT 25"));
  CHECK_EQ(r.app.console("set time_format 24h"), std::string("@ok time_format"));
  CHECK(r.app.screen() != hg::Screen::Clock);  // a setting change ends the clock
  r.advance(6000);
  CHECK_EQ(r.app.model().clock_time, std::string("00:05"));
  CHECK_EQ(r.app.console("set language es"), std::string("@ok language"));
  r.advance(6000);
  CHECK(r.app.screen() == hg::Screen::Clock);
  CHECK_EQ(r.app.model().clock_date, std::string("DOM 25 OCT"));
  r.fake.wall = kWallSaturday;
  r.advance(200);
  CHECK_EQ(r.app.model().clock_date, std::string("SÁB 24 OCT"));
  CHECK_EQ(r.app.model().clock_time, std::string("09:41"));
}

namespace {

const char* const kTsKey = "tskey-auth-kTEST123-abcdefghijklmnopqrstuvwxyz";

// A board that can join a tailnet (DeviceProfile::tailnet), aimed at a tailnet address.
hg::DeviceProfile tailnet_profile() {
  hg::DeviceProfile p = Rig::profile("ws://100.71.189.23:8765/gadget");
  p.tailnet = true;
  return p;
}

}  // namespace

TEST("tailnet: a board without it has no tailnet settings") {
  Rig r;
  r.app.begin();
  CHECK_EQ(r.app.console("set tailnet on"), std::string("@error unknown key"));
  CHECK_EQ(r.app.console("set ts_key tskey-auth-x-y"), std::string("@error unknown key"));
  CHECK(r.app.console("help").find("ts_key") == std::string::npos);
  CHECK(r.app.console("status").find("tailnet") == std::string::npos);
  CHECK(!r.app.tailnet_wanted());
}

TEST("tailnet: off, a tailnet board connects as soon as the network is up") {
  Rig r(tailnet_profile());
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.advance(1000);
  CHECK(!r.app.tailnet_wanted());
  CHECK_EQ(r.fake.connects, 1);
}

TEST("tailnet: on, the device waits for the tunnel and says why") {
  Rig r(tailnet_profile());
  r.fake.kv["tailnet"] = "on";
  r.fake.kv["ts_key"] = kTsKey;
  r.app.begin();
  CHECK(r.app.tailnet_wanted());
  CHECK_EQ(r.app.tailnet_key(), std::string(kTsKey));
  r.app.on_network(true, "wifi");
  r.advance(5000);
  CHECK_EQ(r.fake.connects, 0);
  CHECK(r.app.screen() == hg::Screen::Connecting);
  CHECK(r.app.model().detail.find("Joining tailnet") != std::string::npos);

  r.app.on_tunnel(false, "Tailnet: auth key rejected");
  r.advance(100);
  CHECK(r.app.model().detail.find("Tailnet: auth key rejected") != std::string::npos);
  CHECK_EQ(r.fake.connects, 0);

  r.app.on_tunnel(true);
  r.advance(20);  // no backoff left to wait out: the wait was for the tunnel
  CHECK_EQ(r.fake.connects, 1);
  CHECK_EQ(r.fake.url, std::string("ws://100.71.189.23:8765/gadget"));
  Value st;
  CHECK(hg::json::parse(r.app.console("status").substr(8), st));
  CHECK(st["tailnet"]["on"].as_bool() && st["tailnet"]["key"].as_bool() && st["tailnet"]["up"].as_bool());
}

TEST("connect: a connection isn't timed out as it opens when the clock moves on mid-tick") {
  // tick() reads the clock once; connect_now() stamps phase_since_ later in the same tick.
  // On a device a few milliseconds pass in between, and the connect timeout fired at once.
  Rig r(tailnet_profile());
  r.fake.kv["tailnet"] = "on";
  r.fake.kv["ts_key"] = kTsKey;
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.advance(5000);
  r.fake.drift = 1;
  r.app.on_tunnel(true);
  r.advance(20);
  CHECK_EQ(r.fake.connects, 1);
  CHECK_EQ(r.fake.closes, 0);
  Value d;
  CHECK(hg::json::parse(r.app.console("diag").substr(6), d));
  CHECK_EQ(d["connection"]["closes_total"].as_int(), 0);
  CHECK(!d["connection"].has("last_close"));
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true})");
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.connects, 1);
}

TEST("tailnet: losing the tunnel ends the session until it is back") {
  Rig r(tailnet_profile());
  r.fake.kv["tailnet"] = "on";
  r.fake.kv["ts_key"] = kTsKey;
  r.app.begin();
  r.app.on_network(true, "wifi");
  r.app.on_tunnel(true);
  r.advance(1000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s1","heartbeat_s":20,"paired":true})");
  CHECK(r.app.screen() == hg::Screen::Ready);
  const int closes = r.fake.closes, connects = r.fake.connects;

  r.app.on_tunnel(false, "Joining tailnet");
  CHECK_EQ(r.fake.closes, closes + 1);
  CHECK(r.app.screen() == hg::Screen::Connecting);
  r.advance(40000);  // past every backoff step and the connect timeout
  CHECK_EQ(r.fake.connects, connects);
  Value d;
  CHECK(hg::json::parse(r.app.console("diag").substr(6), d));
  CHECK_EQ(d["connection"]["tailnet"].as_string(), std::string("Joining tailnet"));

  r.app.on_tunnel(true);
  r.advance(20);
  CHECK_EQ(r.fake.connects, connects + 1);

  // Back before the reconnect backoff is over: connect at once instead of waiting it out.
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":true})");
  r.app.on_tunnel(false, "Joining tailnet");
  r.app.on_tunnel(true);
  r.advance(20);
  CHECK_EQ(r.fake.connects, connects + 2);
}

TEST("tailnet: turning it on reconnects through the tunnel; off connects directly") {
  Rig r(tailnet_profile());
  r.bring_online(true);
  CHECK(r.app.screen() == hg::Screen::Ready);
  const int closes = r.fake.closes, connects = r.fake.connects;
  CHECK_EQ(r.app.console("set ts_key " + std::string(kTsKey)), std::string("@ok ts_key"));
  CHECK_EQ(r.app.console("set tailnet on"), std::string("@ok tailnet"));
  CHECK(r.fake.closes > closes);
  r.advance(5000);
  CHECK_EQ(r.fake.connects, connects);  // no tunnel yet
  CHECK_EQ(r.app.console("set tailnet off"), std::string("@ok tailnet"));
  r.advance(20);
  CHECK_EQ(r.fake.connects, connects + 1);
}

TEST("tailnet: the auth key is checked, kept secret and never logged") {
  Rig r(tailnet_profile());
  r.app.begin();
  CHECK_EQ(r.app.console("set tailnet maybe"), std::string("@error tailnet must be on or off"));
  CHECK_EQ(r.app.console("set ts_key hunter2"), std::string("@error ts_key must be a Tailscale auth key (tskey-...)"));
  CHECK_EQ(r.app.console("set ts_key tskey-auth-has space"), std::string("@error ts_key must be a Tailscale auth key (tskey-...)"));
  CHECK_EQ(r.app.console("set ts_key " + std::string(kTsKey)), std::string("@ok ts_key"));
  CHECK(r.app.console("get ts_key").find("<set>") != std::string::npos);
  CHECK(r.app.console("get ts_key").find("abcdefghij") == std::string::npos);
  CHECK(r.app.console("status").find("abcdefghij") == std::string::npos);
  CHECK(r.app.console("diag").find("abcdefghij") == std::string::npos);
  for (const auto& line : r.fake.logs) CHECK(line.find("abcdefghij") == std::string::npos);
  CHECK(r.app.console("help").find("tailnet ts_key") != std::string::npos);
  CHECK_EQ(r.app.console("set ts_key"), std::string("@ok ts_key"));  // empty clears it
  CHECK(r.app.tailnet_key().empty());
}

TEST("tailnet: factory reset forgets the settings and the tailnet identity") {
  Rig r(tailnet_profile());
  r.fake.kv["tailnet"] = "on";
  r.fake.kv["ts_key"] = kTsKey;
  int forgot = 0;
  r.app.on_tailnet_forget = [&] { ++forgot; return true; };
  r.app.begin();
  r.app.console("factory-reset");
  CHECK(r.fake.kv.find("tailnet") == r.fake.kv.end());
  CHECK(r.fake.kv.find("ts_key") == r.fake.kv.end());
  CHECK_EQ(forgot, 1);
  r.app.console("forget-key");
  CHECK_EQ(forgot, 1);  // forgetting the device key keeps the tailnet
}

TEST("tailnet: factory reset says so when the tailnet identity can't be scheduled for erasing") {
  Rig r(tailnet_profile());
  r.fake.kv["ts_key"] = kTsKey;
  r.app.on_tailnet_forget = [] { return false; };
  r.app.begin();
  const std::string reply = r.app.console("factory-reset");
  CHECK(reply.rfind("@error factory-reset", 0) == 0);
  CHECK(reply.find("tailnet identity") != std::string::npos);
  CHECK(r.fake.kv.find("ts_key") == r.fake.kv.end());  // the settings are erased either way
}

namespace {

hg::DeviceProfile thumb_profile() {
  hg::DeviceProfile p = Rig::touch_profile();
  p.colour_face_alloc = std::malloc;
  return p;
}

int thumb_px(Rig& r) {
  const Value* hello = r.fake.last("hello");
  return hello ? static_cast<int>((*hello)["caps"]["faces"]["thumbs"].as_int(0)) : 0;
}

std::string thumb_start(const std::string& profile, const std::string& hash, int px, int stream = 9) {
  return R"({"type":"face.thumb.start","stream":)" + std::to_string(stream) + R"(,"profile":")" + profile +
         R"(","hash":")" + hash + R"(","width":)" + std::to_string(px) + R"(,"height":)" + std::to_string(px) +
         R"(,"format":"rgb565"})";
}

std::string thumb_end(int stream = 9) { return R"({"type":"face.thumb.end","stream":)" + std::to_string(stream) + "}"; }

void send_thumb(Rig& r, const std::string& profile, const std::string& hash, uint16_t pixel, int stream = 9) {
  const int px = thumb_px(r);
  r.server(thumb_start(profile, hash, px, stream));
  send_pixels(r, px, pixel, stream);
  r.server(thumb_end(stream));
}

// The thumbnails the device names in its last hello: profile id -> hash.
std::map<std::string, std::string> held_thumbs(Rig& r) {
  std::map<std::string, std::string> out;
  const Value* hello = r.fake.last("hello");
  if (!hello) return out;
  for (const auto& member : (*hello)["thumbs"].members()) out[member.first] = member.second.as_string();
  return out;
}

void reconnect_with_roster(Rig& r, int n, bool paired = true) {
  r.app.on_transport_closed("bye");
  r.fake.sent.clear();  // so the hello checked is this connection's
  for (int i = 0; i < 120 && !r.fake.last("hello"); ++i) {  // whenever the device dials again
    r.advance(250);
    r.app.on_transport_open();
  }
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":true})");
  r.server(std::string(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":)") + (paired ? "true" : "false") +
           R"(,"profile":"p0","profiles":[)" + roster_of(n) + "]}");
}

}  // namespace

TEST("profile thumbnails: a touch board with room asks for the grid's largest icon and draws them") {
  Rig r(thumb_profile());
  r.fake.make_panel(410, 486, false);
  welcome_with_roster(r, 8);
  const int px = thumb_px(r);
  CHECK(px >= hg::App::kMinThumbPx && px <= hg::App::kMaxThumbPx);
  CHECK_EQ(px, r.app.thumb_px());
  const Value* hello = r.fake.last("hello");
  CHECK(hello && (*hello)["thumbs"].is_object() && (*hello)["thumbs"].members().empty());
  CHECK(r.app.open_profile_picker());
  r.advance(50);
  CHECK_EQ(count_px(r.fake.fb, kRose), size_t(0));
  send_thumb(r, "p3", "t3", kRose);
  r.advance(50);
  CHECK(count_px(r.fake.fb, kRose) > 0);  // p3's cell shows its picture, the others their initials
  CHECK_EQ(r.app.model().thumbs.size(), size_t(8));
  CHECK(r.app.model().thumbs[3] != nullptr && r.app.model().thumbs[2] == nullptr);
}

TEST("profile thumbnails: kept by hash through reconnects; a new one replaces only its profile's") {
  Rig r(thumb_profile());
  welcome_with_roster(r, 8);
  send_thumb(r, "p3", "t3", kRose);
  send_thumb(r, "p4", "t4", kTeal);
  reconnect_with_roster(r, 8);
  auto held = held_thumbs(r);
  CHECK_EQ(held.size(), size_t(2));
  CHECK_EQ(held["p3"], std::string("t3"));
  CHECK_EQ(held["p4"], std::string("t4"));
  send_thumb(r, "p3", "t3b", kTeal);  // p3's avatar changed in Hermes
  CHECK(r.app.open_profile_picker());
  CHECK(r.app.model().thumbs[3] != nullptr && r.app.model().thumbs[4] != nullptr);
  reconnect_with_roster(r, 8);
  held = held_thumbs(r);
  CHECK_EQ(held.size(), size_t(2));
  CHECK_EQ(held["p3"], std::string("t3b"));
  CHECK_EQ(held["p4"], std::string("t4"));
}

TEST("profile thumbnails: a clear, a profile no longer offered and unpairing bring the initials back") {
  Rig r(thumb_profile());
  welcome_with_roster(r, 8);
  send_thumb(r, "p3", "t3", kRose);
  send_thumb(r, "p6", "t6", kTeal);
  send_thumb(r, "p2", "t2", kTeal);
  r.server(R"({"type":"face.thumb","profile":"p3"})");
  CHECK(r.app.open_profile_picker());
  CHECK(r.app.model().thumbs[3] == nullptr && r.app.model().thumbs[6] != nullptr);
  reconnect_with_roster(r, 5);  // p6 is no longer offered: the welcome drops it
  CHECK_EQ(held_thumbs(r).count("p6"), size_t(1));  // the hello, before the welcome, still named it
  CHECK(r.app.open_profile_picker());
  CHECK_EQ(r.app.model().thumbs.size(), size_t(5));
  CHECK(r.app.model().thumbs[2] != nullptr);
  reconnect_with_roster(r, 5);
  CHECK_EQ(held_thumbs(r).size(), size_t(1));  // p2's alone
  r.server(R"({"type":"unpaired"})");  // unpairing drops them at once
  r.server(R"({"type":"paired"})");
  CHECK(r.app.open_profile_picker());
  for (const uint16_t* t : r.app.model().thumbs) CHECK(t == nullptr);
  send_thumb(r, "p2", "t2", kTeal);
  CHECK(r.app.model().thumbs[2] != nullptr);
  reconnect_with_roster(r, 5, false);  // an unpaired welcome drops them too
  reconnect_with_roster(r, 5);
  CHECK(held_thumbs(r).empty());
}

TEST("profile thumbnails: a malformed or interrupted one is ignored, and a colour face still arrives beside it") {
  Rig r(thumb_profile());
  welcome_with_roster(r, 8);
  const int px = thumb_px(r);
  r.server(thumb_start("nosuch", "t", px));  // not offered
  send_pixels(r, px, kRose, 9);
  r.server(thumb_end());
  r.server(thumb_start("p1", "t", px + 1));  // another size
  r.server(thumb_end());
  r.server(thumb_start("p1", "", px));  // no hash
  r.server(thumb_end());
  r.server(thumb_start("p2", "t2", px));
  send_pixels(r, px, kRose, 9, 0, 4096);  // ends short
  r.server(thumb_end());
  CHECK(r.app.open_profile_picker());
  for (const uint16_t* t : r.app.model().thumbs) CHECK(t == nullptr);
  // A thumbnail's chunks on their own stream leave a colour face arriving on another intact.
  r.server(colour_start("p0", "c1", colour_px(r), 7));
  r.server(thumb_start("p5", "t5", px, 9));
  send_pixels(r, colour_px(r), kRose, 7);
  send_pixels(r, px, kTeal, 9);
  r.server(colour_end(7));
  r.server(thumb_end(9));
  r.advance(10);
  CHECK(r.app.model().thumbs[5] != nullptr);
  CHECK_EQ(r.app.model().face_row, 0);  // the current profile shows its own face
}

TEST("profile thumbnails: with room only at 32 px, the last fallback is 32 px") {
  hg::DeviceProfile p = thumb_profile();  // the colour face fits, and thumbnails only at 32 px
  p.colour_face_alloc = [](size_t n) -> void* {
    static int calls = 0;
    const size_t at32 = hg::App::kMaxProfiles * 32 * 32 * 2;
    return calls++ == 0 || n <= at32 ? std::malloc(n) : nullptr;
  };
  Rig r(p);
  r.fake.make_panel(410, 486, false);
  welcome_with_roster(r, 8);
  CHECK(colour_px(r) > 0);
  CHECK_EQ(thumb_px(r), hg::App::kMinThumbPx);  // 128, 96, 72, 54, 40, then 32: before, 40 was the last try
}

TEST("profile thumbnails: none without room, without colour faces or without a touchscreen") {
  Rig plain(Rig::touch_profile());  // no PSRAM to spare
  welcome_with_roster(plain, 8);
  CHECK_EQ(thumb_px(plain), 0);
  CHECK(plain.fake.last("hello") && !(*plain.fake.last("hello"))["thumbs"].is_object());
  plain.server(thumb_start("p3", "t3", 96));
  send_pixels(plain, 96, kRose, 9);
  plain.server(thumb_end());
  CHECK(plain.app.open_profile_picker());
  CHECK(plain.app.model().thumbs.empty());

  hg::DeviceProfile tight = thumb_profile();  // room for the colour face, then for nothing else
  tight.colour_face_alloc = [](size_t n) -> void* {
    static int calls = 0;
    return calls++ ? nullptr : std::malloc(n);
  };
  Rig small(tight);
  welcome_with_roster(small, 8);
  CHECK(colour_px(small) > 0);
  CHECK_EQ(thumb_px(small), 0);

  Rig buttons(colour_profile());
  buttons.bring_online(true);
  CHECK_EQ(thumb_px(buttons), 0);
}

TEST("screen: an open profile list or dashboard times out, closes in the dark and wakes to Ready") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online_with_profiles();
  CHECK_EQ(r.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  CHECK(r.app.open_profile_picker());
  r.advance(16000);  // half of the timeout: dim, the list still open
  CHECK_EQ(r.fake.brightness, 10);
  CHECK(r.app.profile_picker_open());
  r.server(R"({"type":"ping"})");
  r.advance(15000);  // the timeout: dark, and the list is gone
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.app.profile_picker_open());
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);

  r.server(kDashboard);
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  r.server(R"({"type":"ping"})");
  r.advance(16000);
  r.server(R"({"type":"ping"})");
  r.advance(15000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("screen: the PWR key turns the screen off over a list or a card, and wakes to Ready") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online_with_profiles();
  CHECK(r.app.open_profile_picker());
  r.app.on_power_key();  // PWR is Back on the profile grid: Ready, still lit
  CHECK(!r.app.profile_picker_open());
  CHECK_EQ(r.fake.brightness, 100);
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 0);
  r.advance(1000);  // nothing open lights it again
  CHECK_EQ(r.fake.brightness, 0);
  CHECK(!r.app.profile_picker_open());
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);

  r.server(R"({"type":"display","title":"Note","body":"Stays until closed","ttl_s":0})");
  CHECK(r.app.screen() == hg::Screen::Card);
  r.app.on_power_key();
  r.advance(1000);
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_power_key();
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("screen: in settings the PWR key is Back: the menu closes, the screen stays lit, the next press darkens it") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  r.app.console("cancel");  // a later item: Back leaves from anywhere in the list
  r.app.on_power_key();
  CHECK(!r.app.settings_open());
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);
  r.advance(1000);  // nothing reopens the menu or darkens the screen
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);
  r.app.on_power_key();  // on the home screen, PWR turns the screen off as before
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
}

namespace {
size_t sent_count(const Rig& r, const std::string& type) {
  size_t n = 0;
  for (const Value& v : r.fake.sent) n += v["type"].as_string() == type;
  return n;
}

// One press of the TALK key, held for `held_ms`.
void press_talk(Rig& r, uint32_t held_ms) {
  r.app.on_button(hg::Button::Talk, true);
  r.advance(held_ms);
  r.app.on_button(hg::Button::Talk, false);
}
}  // namespace

TEST("double press: TALK twice goes home, closes the dashboard and asks for the default profile") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  r.server(kDashboard);
  tap_cancel(r);
  CHECK(r.app.screen() == hg::Screen::Dashboard);
  // The first press talks at once: nothing waits to see whether a second follows.
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  CHECK(r.fake.mic_on);
  CHECK_EQ(sent_count(r, "audio.start"), size_t(1));
  r.advance(100);
  r.app.on_button(hg::Button::Talk, false);  // too short: discarded, as always
  r.advance(150);
  r.app.on_button(hg::Button::Talk, true);  // 250 ms after the first press
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.fake.mic_on);
  CHECK_EQ(sent_count(r, "audio.start"), size_t(1));
  const Value* select = r.fake.last("profile.select");
  CHECK(select != nullptr);
  if (select) CHECK_EQ((*select)["profile"].as_string(), std::string("default"));
  CHECK(r.app.model().hint.find("while speaking") == std::string::npos);  // the first press's hint is gone
  r.app.on_button(hg::Button::Talk, false);  // the release does nothing
  CHECK_EQ(sent_count(r, "audio.start"), size_t(1));
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.server(R"({"type":"profile","profile":"default"})");
  CHECK_EQ(r.app.agent_profile(), std::string("default"));
  // A third press is a press of its own.
  r.advance(100);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("double press: on the default profile it only goes home; slow presses each talk") {
  Rig r;
  r.bring_online_with_profiles();
  press_talk(r, 100);
  r.advance(100);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(r.fake.last("profile.select") == nullptr);
  r.app.on_button(hg::Button::Talk, false);

  Rig slow;
  slow.bring_online_with_profiles(true, "ops");
  press_talk(slow, 100);
  slow.advance(hg::App::kDoublePressMs);  // 500 ms between the presses
  slow.app.on_button(hg::Button::Talk, true);
  CHECK(slow.app.screen() == hg::Screen::Listening);
  CHECK_EQ(sent_count(slow, "audio.start"), size_t(2));
  CHECK(slow.fake.last("profile.select") == nullptr);
  slow.app.on_button(hg::Button::Talk, false);
}

TEST("double press: in tap mode the second press throws the recording away instead of sending it") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  CHECK_EQ(r.app.console("set talk_mode tap"), std::string("@ok talk_mode"));
  press_talk(r, 80);
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.advance(150);
  press_talk(r, 80);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.end") == nullptr);
  const Value* cancel = r.fake.last("audio.cancel");
  CHECK(cancel != nullptr);
  CHECK(r.fake.last("profile.select") != nullptr);
}

TEST("double press: it stops a reply the first press talked over") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  r.app.submit_text("hello");
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.server(R"({"type":"reply.delta","turn":"t1","text":"Working on it"})");
  CHECK(r.fake.last("cancel") == nullptr);
  press_talk(r, 60);
  r.advance(100);
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.last("cancel") != nullptr);
  CHECK(r.app.screen() == hg::Screen::Ready);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("double press: touch, the settings menu, a question and double_home off keep TALK as it was") {
  Rig touch(Rig::touch_profile());
  touch.bring_online_with_profiles(true, "ops");
  touch.app.on_button(hg::Button::Talk, true, true);
  touch.advance(60);
  touch.app.on_button(hg::Button::Talk, false, true);
  touch.advance(100);
  touch.app.on_button(hg::Button::Talk, true, true);
  CHECK(touch.app.screen() == hg::Screen::Listening);
  touch.app.on_button(hg::Button::Talk, false, true);
  CHECK(touch.fake.last("profile.select") == nullptr);

  Rig menu;
  menu.bring_online_with_profiles(true, "ops");
  CHECK(menu.app.open_settings());
  press_talk(menu, 60);
  menu.advance(100);
  press_talk(menu, 60);
  CHECK(menu.app.settings_open());

  Rig asked;
  asked.bring_online_with_profiles(true, "ops");
  press_talk(asked, 60);
  asked.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"ls"})");
  asked.advance(100);
  asked.app.on_button(hg::Button::Talk, true);
  CHECK(asked.app.screen() == hg::Screen::Prompt);
  asked.app.on_button(hg::Button::Talk, false);
  CHECK(asked.fake.last("profile.select") == nullptr);

  Rig off;
  off.bring_online_with_profiles(true, "ops");
  CHECK(off.app.double_home());
  CHECK_EQ(off.app.console("set double_home maybe"), std::string("@error double_home must be on or off"));
  CHECK_EQ(off.app.console("set double_home off"), std::string("@ok double_home"));
  CHECK(!off.app.double_home());
  press_talk(off, 60);
  off.advance(100);
  off.app.on_button(hg::Button::Talk, true);
  CHECK(off.app.screen() == hg::Screen::Listening);
  off.app.on_button(hg::Button::Talk, false);
  CHECK(off.fake.last("profile.select") == nullptr);
}

TEST("double press: a press that only wakes the screen starts none") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online_with_profiles(true, "ops");
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 0);
  press_talk(r, 60);  // wakes
  CHECK_EQ(r.fake.brightness, 100);
  r.advance(100);
  r.app.on_button(hg::Button::Talk, true);  // a quick press-and-hold after the wake still talks
  CHECK(r.app.screen() == hg::Screen::Listening);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("profile.select") == nullptr);
}

TEST("wake talk: off by default, a press on a dark screen only wakes; on, it wakes and talks at once") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online_with_profiles();
  CHECK(!r.app.wake_talk());
  CHECK_EQ(r.app.console("set wake_talk maybe"), std::string("@error wake_talk must be on or off"));
  CHECK_EQ(r.app.console("set wake_talk on"), std::string("@ok wake_talk"));
  CHECK(r.app.wake_talk());
  CHECK_EQ(r.fake.kv["wake_talk"], std::string("on"));
  r.app.on_power_key();
  CHECK_EQ(r.fake.brightness, 0);
  r.app.on_button(hg::Button::Talk, true);  // hold to talk: lit and listening in the same press
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Listening);
  CHECK(r.fake.mic_on);
  r.advance(1000);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") != nullptr);
  CHECK(r.app.screen() == hg::Screen::Thinking);

  // Tap mode: the press starts a recording that a second tap (later than a double press) sends.
  Rig tap;
  tap.fake.backlight = true;
  tap.bring_online_with_profiles();
  tap.app.console("set wake_talk on");
  tap.app.console("set talk_mode tap");
  tap.app.on_power_key();
  press_talk(tap, 80);
  CHECK_EQ(tap.fake.brightness, 100);
  CHECK(tap.app.screen() == hg::Screen::Listening);
  tap.advance(1500);
  press_talk(tap, 80);
  CHECK(tap.fake.last("audio.end") != nullptr);
}

TEST("wake talk: a double press on a dark screen lights it and goes home to the default profile") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online_with_profiles(true, "ops");
  r.app.console("set wake_talk on");
  r.app.on_power_key();
  press_talk(r, 80);
  r.advance(100);
  r.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK(!r.fake.mic_on);
  CHECK(r.fake.last("audio.end") == nullptr);
  const Value* select = r.fake.last("profile.select");
  CHECK(select != nullptr);
  if (select) CHECK_EQ((*select)["profile"].as_string(), std::string("default"));
  r.app.on_button(hg::Button::Talk, false);
}

TEST("wake talk: touch, a question, the menu and no connection still only wake") {
  Rig touch(Rig::touch_profile());
  touch.fake.backlight = true;
  touch.bring_online_with_profiles();
  touch.app.console("set wake_talk on");
  touch.app.on_power_key();
  touch.app.on_button(hg::Button::Talk, true, true);
  CHECK_EQ(touch.fake.brightness, 100);
  CHECK(!touch.fake.mic_on);
  touch.app.on_button(hg::Button::Talk, false, true);

  Rig asked;
  asked.fake.backlight = true;
  asked.bring_online_with_profiles();
  asked.app.console("set wake_talk on");
  asked.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"ls"})");
  asked.app.on_power_key();
  CHECK_EQ(asked.fake.brightness, 0);
  press_talk(asked, 60);
  CHECK_EQ(asked.fake.brightness, 100);
  CHECK(asked.app.screen() == hg::Screen::Prompt);  // not answered
  CHECK(asked.fake.last("prompt.reply") == nullptr);

  Rig menu;
  menu.fake.backlight = true;
  menu.bring_online_with_profiles();
  menu.app.console("set wake_talk on");
  CHECK_EQ(menu.app.console("set screen_timeout 30"), std::string("@ok screen_timeout"));
  CHECK(menu.app.open_settings());
  menu.advance(31000);
  CHECK_EQ(menu.fake.brightness, 0);
  press_talk(menu, 60);
  CHECK(menu.app.settings_open());
  CHECK(!menu.fake.mic_on);

  Rig offline;
  offline.fake.backlight = true;
  offline.bring_online_with_profiles(false);  // waiting for pairing
  offline.app.console("set wake_talk on");
  offline.app.on_power_key();
  offline.app.on_button(hg::Button::Talk, true);
  CHECK_EQ(offline.fake.brightness, 100);
  CHECK(!offline.fake.mic_on);
  offline.app.on_button(hg::Button::Talk, false);
}

TEST("settings: Double press home and Talk from dark screen toggle their settings; boards without a TALK key hide them") {
  hg::DeviceProfile p = Rig::touch_profile();
  p.talk_key = true;
  p.talk_label = "BOOT";
  Rig r(p);
  r.bring_online_with_profiles();
  CHECK(r.app.open_settings());
  CHECK(find_setting(r, "Double press home"));
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  CHECK(r.app.model().body.find("BOOT") != std::string::npos);
  r.app.console("talk");
  r.app.console("release");
  CHECK(!r.app.double_home());
  CHECK_EQ(r.fake.kv["double_home"], std::string("off"));
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Talk from dark screen"));
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.app.wake_talk());
  CHECK_EQ(r.fake.kv["wake_talk"], std::string("on"));
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  CHECK(r.app.model().body.find("BOOT") != std::string::npos);

  hg::DeviceProfile touch_only = Rig::touch_profile();
  touch_only.talk_key = false;
  Rig bare(touch_only);
  bare.bring_online_with_profiles();
  CHECK(bare.app.open_settings());
  CHECK(!find_setting(bare, "Double press home"));
  CHECK(!find_setting(bare, "Talk from dark screen"));
}

TEST("double press: a press that answers a question or selects Back in the menu starts none") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  r.server(R"({"type":"turn.start","turn":"t1"})");
  r.server(R"({"type":"prompt","id":"q1","title":"Allow?","text":"ls"})");
  r.advance(1000);  // the question is armed for an answer
  press_talk(r, 60);
  CHECK(r.fake.last("prompt.reply") != nullptr);
  r.advance(100);
  r.app.on_button(hg::Button::Talk, true);  // within 400 ms of the answer
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("cancel") == nullptr);
  CHECK(r.fake.last("profile.select") == nullptr);

  Rig menu;
  menu.bring_online_with_profiles(true, "ops");
  CHECK(menu.app.open_settings());
  CHECK(find_setting(menu, "Back to Hermes"));
  press_talk(menu, 60);
  CHECK(!menu.app.settings_open());
  menu.advance(100);
  menu.app.on_button(hg::Button::Talk, true);
  CHECK(menu.app.screen() == hg::Screen::Listening);
  menu.app.on_button(hg::Button::Talk, false);
  CHECK(menu.fake.last("profile.select") == nullptr);
}

TEST("double press: a first press held long enough to send its recording starts none") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  press_talk(r, 370);  // past the 350 ms cutoff: the recording goes to Hermes
  CHECK(r.fake.last("audio.end") != nullptr);
  r.advance(20);  // the second press comes 390 ms after the first
  r.app.on_button(hg::Button::Talk, true);
  CHECK(r.fake.last("cancel") == nullptr);
  CHECK(r.fake.last("profile.select") == nullptr);
  CHECK(r.app.screen() == hg::Screen::Listening);  // a new question, as before
  r.app.on_button(hg::Button::Talk, false);
}

TEST("double press: an alarm that rings between the presses takes the second one") {
  Rig r;
  r.bring_online_with_profiles(true, "ops");
  r.server(R"({"type":"action","id":"t1","name":"timer.start","args":{"seconds":1}})");
  r.advance(800);
  press_talk(r, 60);  // too short: discarded
  r.advance(200);     // the timer ends
  CHECK(r.app.screen() == hg::Screen::Alarm);
  r.app.on_button(hg::Button::Talk, true);  // within 400 ms of the first press
  CHECK(r.app.screen() != hg::Screen::Alarm);
  CHECK(r.fake.last("profile.select") == nullptr);
  r.app.on_button(hg::Button::Talk, false);
}

TEST("screen: the PWR key leaves a running hardware check straight to the home screen") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 3; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Microphone check"));
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.fake.mic_on);
  r.app.on_power_key();
  CHECK(!r.fake.mic_on);  // the check stops with the menu
  CHECK(!r.app.settings_open());
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);

  // The display check's colour bands go too.
  CHECK(r.app.open_settings());
  for (int i = 0; i < 5; ++i) r.app.console("cancel");
  CHECK_EQ(r.app.model().detail, std::string("Display check"));
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.app.model().color_test);
  r.app.on_power_key();
  CHECK(!r.app.settings_open());
  CHECK(!r.app.model().color_test);
  CHECK(r.app.screen() == hg::Screen::Ready);
  CHECK_EQ(r.fake.brightness, 100);
}

// ---- The settings list: rows, touch and scrolling ----

namespace {
// Taps (x, y): a press and a lift with no movement.
void tap_at(Rig& r, hg::TouchGestures& touch, int x, int y) {
  touch.update(true, x, y, r.fake.clock);
  touch.update(false, 0, 0, r.fake.clock + 50);
}

int settings_row(const Rig& r, const std::string& label) {
  const auto& rows = r.app.model().settings;
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].label == label) return static_cast<int>(i);
  return -1;
}
}  // namespace

TEST("settings list: every offered item is a row with its icon and state, and the selected one is marked") {
  hg::DeviceProfile p = Rig::touch_profile();
  p.motion_sensor = true;
  Rig r(p);
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  const auto& rows = r.app.model().settings;
  CHECK(rows.size() > 10);
  CHECK_EQ(r.app.model().settings_sel, 0);
  CHECK_EQ(rows[0].label, std::string("Speaker volume"));
  CHECK(rows[0].kind == hg::SettingRow::Kind::Slider);
  CHECK(rows[0].icon == hg::SettingRow::Icon::Volume);
  CHECK_EQ(int(rows[0].level), 70);
  CHECK_EQ(rows[0].value, std::string("70%"));
  const int bright = settings_row(r, "Screen brightness");
  CHECK(bright == 1 && rows[1].kind == hg::SettingRow::Kind::Slider && rows[1].level == 100);
  const int talk = settings_row(r, "Talk mode");
  CHECK(talk > 0 && rows[static_cast<size_t>(talk)].value == "Hold");
  const int face = settings_row(r, "Face-down sleep");
  CHECK(face > 0 && rows[static_cast<size_t>(face)].kind == hg::SettingRow::Kind::Toggle);
  CHECK_EQ(int(rows[static_cast<size_t>(face)].level), 0);
  const int mic = settings_row(r, "Microphone check");
  CHECK(mic > 0 && rows[static_cast<size_t>(mic)].kind == hg::SettingRow::Kind::Action);
  CHECK(rows[static_cast<size_t>(mic)].value.empty());
  CHECK_EQ(rows.back().label, std::string("Back to Hermes"));
  CHECK(settings_row(r, "Power off") < 0);  // not offered on this board: no row
  // Buttons move the selection; the model's detail and body follow it.
  r.app.on_button(hg::Button::Down, true);
  CHECK_EQ(r.app.model().settings_sel, 1);
  CHECK_EQ(r.app.model().detail, std::string("Screen brightness"));
  // Spanish: the short talk mode value is translated too.
  r.app.console("set language es");
  CHECK_EQ(r.app.model().settings[static_cast<size_t>(talk)].value, std::string("Mantener"));
}

TEST("settings list: the battery alert and charging items have their icons and switches") {
  KeyPower key;
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.hal.power = &key;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  using Icon = hg::SettingRow::Icon;
  using Kind = hg::SettingRow::Kind;
  const auto row = [&](const char* label) -> const hg::SettingRow& {
    const int i = settings_row(r, label);
    CHECK(i >= 0);
    return r.app.model().settings[static_cast<size_t>(i < 0 ? 0 : i)];
  };
  const auto is = [&](const char* label, Icon icon, Kind kind) {
    const hg::SettingRow& x = row(label);
    CHECK(x.icon == icon);
    CHECK(x.kind == kind);
  };
  is("Low battery alert", Icon::Bell, Kind::Value);
  is("Low battery sound", Icon::Volume, Kind::Toggle);
  is("Charging screen", Icon::Bolt, Kind::Toggle);
  is("Charging sound", Icon::Volume, Kind::Toggle);
  is("Full battery screen", Icon::Check, Kind::Toggle);
  is("Full battery sound", Icon::Volume, Kind::Toggle);
  // A switch follows its setting.
  const int before = row("Charging sound").level;
  r.app.console(before ? "set charging_sound off" : "set charging_sound on");
  CHECK_EQ(int(row("Charging sound").level), before ? 0 : 1);
}

TEST("settings: Deep Wi-Fi sleep is on by default, toggles wifi_deep_sleep, and shows only with a battery and Wi-Fi") {
  KeyPower key;
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.hal.power = &key;
  r.app.current_wifi = [] { return std::string("home"); };
  r.bring_online(true);
  CHECK(r.app.wifi_deep_sleep());
  CHECK_EQ(r.app.console("set wifi_deep_sleep maybe"), std::string("@error wifi_deep_sleep must be on or off"));
  CHECK(r.app.open_settings());
  const int i = settings_row(r, "Deep Wi-Fi sleep");
  CHECK(i >= 0);
  const hg::SettingRow& row = r.app.model().settings[static_cast<size_t>(i < 0 ? 0 : i)];
  CHECK(row.icon == hg::SettingRow::Icon::Wifi);
  CHECK(row.kind == hg::SettingRow::Kind::Toggle);
  CHECK_EQ(int(row.level), 1);
  CHECK(find_setting(r, "Deep Wi-Fi sleep"));
  CHECK(r.app.model().body.rfind("On\n", 0) == 0);
  r.app.on_button(hg::Button::Talk, true);  // select: the switch flips and is kept
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.app.wifi_deep_sleep());
  CHECK_EQ(r.fake.kv["wifi_deep_sleep"], std::string("off"));
  CHECK(r.app.model().body.rfind("Off\n", 0) == 0);
  CHECK_EQ(r.app.console("set wifi_deep_sleep on"), std::string("@ok wifi_deep_sleep"));
  CHECK(r.app.wifi_deep_sleep());

  Rig plugged(Rig::touch_profile());  // no battery reading: no item
  plugged.app.current_wifi = [] { return std::string("home"); };
  plugged.bring_online(true);
  CHECK(plugged.app.open_settings());
  CHECK(settings_row(plugged, "Deep Wi-Fi sleep") < 0);
  KeyPower key2;
  Rig no_wifi(Rig::touch_profile());  // the simulator: no Wi-Fi to tune
  no_wifi.hal.power = &key2;
  no_wifi.bring_online(true);
  CHECK(no_wifi.app.open_settings());
  CHECK(settings_row(no_wifi, "Deep Wi-Fi sleep") < 0);
}

TEST("settings list: buttons scroll the list smoothly to the selection, and back to the top on the next visit") {
  Rig r;
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  CHECK_EQ(r.app.model().settings_scroll, 0);
  r.app.on_button(hg::Button::Up, true);  // wraps to the last row, Back
  CHECK_EQ(r.app.model().detail, std::string("Back to Hermes"));
  CHECK_EQ(r.app.model().settings_scroll, 0);  // nothing jumps: the list eases there
  std::vector<int> path;
  for (int i = 0; i < 30; ++i) {
    r.advance(10);
    if (path.empty() || path.back() != r.app.model().settings_scroll) path.push_back(r.app.model().settings_scroll);
  }
  CHECK(path.size() >= 4);  // several frames in between
  for (size_t i = 1; i < path.size(); ++i) CHECK(path[i] > path[i - 1]);
  const int end = path.back();
  CHECK(end > 0);
  r.advance(500);
  CHECK_EQ(r.app.model().settings_scroll, end);  // and it stays there
  r.app.on_button(hg::Button::Down, true);  // wraps to the top: it eases back to 0
  r.advance(500);
  CHECK_EQ(r.app.model().settings_scroll, 0);
  r.app.on_button(hg::Button::Up, true);
  r.advance(500);
  r.app.close_settings();
  CHECK(r.app.open_settings());
  CHECK_EQ(r.app.model().settings_scroll, 0);
}

TEST("settings list: a drag scrolls with the finger, stops at the ends, and changes nothing") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  hg::TouchGestures touch(r.app);
  const std::string volume = r.fake.kv["volume"];
  touch.update(true, 150, 200, r.fake.clock);
  touch.update(true, 150, 190, r.fake.clock + 10);  // within the slop: still a tap
  CHECK_EQ(r.app.model().settings_scroll, 0);
  touch.update(true, 150, 160, r.fake.clock + 20);
  CHECK_EQ(r.app.model().settings_scroll, 40);  // from the press, not the slop
  touch.update(true, 150, 120, r.fake.clock + 30);
  CHECK_EQ(r.app.model().settings_scroll, 80);
  touch.update(true, 150, 140, r.fake.clock + 40);
  CHECK_EQ(r.app.model().settings_scroll, 60);
  touch.update(false, 0, 0, r.fake.clock + 50);
  r.advance(300);  // a drag leaves the list where the finger put it
  CHECK_EQ(r.app.model().settings_scroll, 60);
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  CHECK_EQ(r.fake.kv["volume"], volume);
  // Past the top it stops at 0; past the end, at the last row.
  touch.update(true, 150, 60, r.fake.clock);
  touch.update(true, 150, 230, r.fake.clock + 10);
  touch.update(false, 0, 0, r.fake.clock + 20);
  CHECK_EQ(r.app.model().settings_scroll, 0);
  int last = -1;
  for (int i = 0; i < 6; ++i) {
    touch.update(true, 150, 230, r.fake.clock);
    touch.update(true, 150, 60, r.fake.clock + 10);
    touch.update(false, 0, 0, r.fake.clock + 20);
    last = r.app.model().settings_scroll;
  }
  CHECK(last > 0);
  touch.update(true, 150, 230, r.fake.clock);
  touch.update(true, 150, 60, r.fake.clock + 10);
  touch.update(false, 0, 0, r.fake.clock + 20);
  CHECK_EQ(r.app.model().settings_scroll, last);
  // Buttons bring the list back to the selection.
  r.app.on_button(hg::Button::Down, true);
  r.advance(500);
  CHECK(r.app.model().settings_scroll < last);
}

TEST("settings list: a tap selects a row and changes it; toggles flip on the first tap") {
  hg::DeviceProfile p = Rig::touch_profile();
  p.motion_sensor = true;
  Rig r(p);
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  hg::TouchGestures touch(r.app);
  const int talk = settings_row(r, "Talk mode");
  auto [tx, ty] = find_point(r, [&](int x, int y) { return r.app.settings_row_hit(x, y) == talk; });
  CHECK(tx >= 0);
  tap_at(r, touch, tx, ty);
  CHECK_EQ(r.app.model().detail, std::string("Talk mode"));
  CHECK_EQ(r.fake.kv["talk_mode"], std::string("tap"));
  CHECK(!r.fake.mic_on);  // a tap in the list never talks
  CHECK(r.fake.last("audio.start") == nullptr);
  tap_at(r, touch, tx, ty);
  CHECK_EQ(r.fake.kv["talk_mode"], std::string("hold"));

  // A toggle further down: buttons bring it into view, then a tap on it flips it.
  const int face = settings_row(r, "Face-down sleep");
  while (r.app.model().settings_sel < face - 1) r.app.on_button(hg::Button::Down, true);
  r.advance(500);
  auto [fx, fy] = find_point(r, [&](int x, int y) { return r.app.settings_row_hit(x, y) == face; });
  CHECK(fx >= 0);
  tap_at(r, touch, fx, fy);
  CHECK_EQ(r.app.model().detail, std::string("Face-down sleep"));
  CHECK_EQ(r.fake.kv["face_down_sleep"], std::string("on"));
  CHECK_EQ(int(r.app.model().settings[static_cast<size_t>(face)].level), 1);
  r.advance(500);
  auto [gx, gy] = find_point(r, [&](int x, int y) { return r.app.settings_row_hit(x, y) == face; });
  tap_at(r, touch, gx, gy);
  CHECK_EQ(r.fake.kv["face_down_sleep"], std::string("off"));

  // Back closes the menu.
  const int back = static_cast<int>(r.app.model().settings.size()) - 1;
  while (r.app.model().settings_sel != back) r.app.on_button(hg::Button::Down, true);
  r.advance(500);
  auto [bx, by] = find_point(r, [&](int x, int y) { return r.app.settings_row_hit(x, y) == back; });
  CHECK(bx >= 0);
  tap_at(r, touch, bx, by);
  CHECK(!r.app.settings_open());
  CHECK(r.app.screen() == hg::Screen::Ready);
}

TEST("settings list: Touch to talk is a switch, and with it off a tap on its row still flips it") {
  Rig r(touch_talk_key_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  const int row = settings_row(r, "Touch to talk");
  CHECK(row >= 0);
  const auto& item = r.app.model().settings[static_cast<size_t>(row)];
  CHECK(item.kind == hg::SettingRow::Kind::Toggle);
  CHECK_EQ(int(item.level), 1);  // on by default
  CHECK_EQ(settings_row(r, "Talk mode") + 1, row);  // right after Talk mode
  while (r.app.model().settings_sel < row - 1) r.app.on_button(hg::Button::Down, true);  // into view
  r.advance(500);
  hg::TouchGestures touch(r.app);
  auto [x, y] = find_point(r, [&](int px, int py) { return r.app.settings_row_hit(px, py) == row; });
  CHECK(x >= 0);
  tap_at(r, touch, x, y);  // a toggle flips on the first tap
  CHECK(!r.app.touch_talk());
  CHECK_EQ(int(r.app.model().settings[static_cast<size_t>(row)].level), 0);
  CHECK(!r.fake.mic_on);
  r.advance(500);
  auto [x2, y2] = find_point(r, [&](int px, int py) { return r.app.settings_row_hit(px, py) == row; });
  tap_at(r, touch, x2, y2);  // off, the touchscreen still works the menu
  CHECK(r.app.touch_talk());
  CHECK_EQ(r.fake.kv["touch_talk"], std::string("on"));
}

TEST("settings list: a slider opens on the first tap, then a tap or a drag on its bar sets the level") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  hg::TouchGestures touch(r.app);
  // Brightness is closed: a tap on its row selects it and changes nothing.
  auto [rx, ry] = find_point(r, [&](int x, int y) { return r.app.settings_row_hit(x, y) == 1; });
  tap_at(r, touch, rx, ry);
  CHECK_EQ(r.app.model().detail, std::string("Screen brightness"));
  CHECK_EQ(r.fake.brightness, 100);
  r.advance(500);
  // Its bar: the left end, the middle and the right end.
  int left = -1, right = -1, bar_y = -1;
  for (int y = 0; y < r.fake.height && bar_y < 0; ++y)
    for (int x = 0; x < r.fake.width; ++x)
      if (r.app.settings_slider_hit(x, y) == 0 && r.app.settings_slider_hit(x + 1, y) > 0) { left = x; bar_y = y; break; }
  CHECK(bar_y >= 0);
  for (int x = r.fake.width - 1; x > left; --x)
    if (r.app.settings_slider_hit(x, bar_y) == 100 && r.app.settings_slider_hit(x - 1, bar_y) < 100) { right = x; break; }
  CHECK(right > left);
  CHECK(r.app.settings_row_hit(rx, ry) == 1);
  tap_at(r, touch, (left + right) / 2, bar_y);
  CHECK_EQ(r.fake.kv["brightness"], std::string("50"));
  CHECK_EQ(r.fake.brightness, 50);
  CHECK_EQ(r.app.model().settings[1].value, std::string("50%"));
  tap_at(r, touch, left, bar_y);  // the far left is the floor, 5%
  CHECK_EQ(r.fake.kv["brightness"], std::string("5"));
  // A drag follows the finger on screen and saves once, on the lift, in steps of 5.
  int saves = 0;
  r.app.on_setting_changed = [&](std::string_view key) { saves += key == "brightness"; };
  touch.update(true, left, bar_y, r.fake.clock);
  touch.update(true, left + (right - left) / 4, bar_y + 30, r.fake.clock + 10);  // height doesn't matter once sliding
  CHECK(r.app.model().settings[1].level >= 24 && r.app.model().settings[1].level <= 26);
  CHECK_EQ(saves, 0);
  CHECK_EQ(r.fake.kv["brightness"], std::string("5"));
  touch.update(true, left + (right - left) * 3 / 4 + 1, bar_y, r.fake.clock + 20);
  touch.update(false, 0, 0, r.fake.clock + 30);
  CHECK_EQ(saves, 1);
  CHECK_EQ(r.fake.kv["brightness"], std::string("75"));
  CHECK_EQ(int(r.app.model().settings[1].level), 75);
  CHECK_EQ(r.app.model().detail, std::string("Screen brightness"));
}

TEST("settings list: a value cut short on its row keeps its full text in the description") {
  FakeHal display;
  display.make_panel(240, 135);
  hg::Ui ui(display);
  const auto selected_h = [&](const std::string& value) {
    hg::UiModel m;
    m.screen = hg::Screen::Settings;
    for (int i = 0; i < 12; ++i) {  // enough rows for a scroll bar
      hg::SettingRow row;
      row.label = i == 0 ? "Language" : "Item";
      row.value = i == 0 ? value : "x";
      m.settings.push_back(row);
    }
    m.settings_sel = 0;
    m.body = value + "\nSelect to change.";
    return ui.settings_row_h(m, 0);
  };
  // "English" fits beside the label, so the description drops the repeat; the long
  // value is cut on the row, so its full line stays in the description.
  CHECK(selected_h("Automatic, as Hermes: English") > selected_h("English"));
}

TEST("settings list: a tap while the list eases changes the row that was under the finger") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  hg::TouchGestures touch(r.app);
  r.app.on_button(hg::Button::Up, true);  // to the last item: the list starts easing to the end
  CHECK_EQ(r.app.model().detail, std::string("Back to Hermes"));
  const int last = r.app.model().settings_sel;
  // A row in the middle of the screen, as it is now.
  const int x = r.fake.width / 2, y = r.fake.height / 2;
  const int under = r.app.settings_row_hit(x, y);
  CHECK(under >= 0 && under != last);
  touch.update(true, x, y, r.fake.clock);
  r.advance(100);  // the list would keep easing under a still finger
  touch.update(false, 0, 0, r.fake.clock);
  CHECK(r.app.settings_open());
  CHECK_EQ(r.app.model().settings_sel, under);
}

TEST("settings list: a slide is not saved when a knob moves the selection before the lift") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  CHECK_EQ(r.app.model().detail, std::string("Speaker volume"));
  hg::TouchGestures touch(r.app);
  int left = -1, bar_y = -1;
  for (int y = 0; y < r.fake.height && bar_y < 0; ++y)
    for (int x = 0; x < r.fake.width; ++x)
      if (r.app.settings_slider_hit(x, y) == 0 && r.app.settings_slider_hit(x + 1, y) > 0) { left = x; bar_y = y; break; }
  CHECK(bar_y >= 0);
  int saves = 0;
  r.app.on_setting_changed = [&](std::string_view) { ++saves; };
  touch.update(true, left, bar_y, r.fake.clock);
  touch.update(true, left + 20, bar_y, r.fake.clock + 10);
  r.app.on_button(hg::Button::Down, true);  // the knob moves on to brightness mid-slide
  CHECK_EQ(r.app.model().detail, std::string("Screen brightness"));
  CHECK_EQ(int(r.app.model().settings[1].level), 100);  // no preview of the volume slide here
  touch.update(true, left + 40, bar_y, r.fake.clock + 20);
  touch.update(false, 0, 0, r.fake.clock + 30);
  CHECK_EQ(saves, 0);
  CHECK_EQ(r.fake.brightness, 100);
  CHECK_EQ(int(r.app.model().settings[1].level), 100);
  CHECK_EQ(int(r.app.model().settings[0].level), 70);
}

TEST("settings list: with a check running, a tap stops it and a swipe down moves on, as before") {
  Rig r(Rig::touch_profile());
  r.fake.backlight = true;
  r.bring_online(true);
  CHECK(r.app.open_settings());
  for (int i = 0; i < 3; ++i) r.app.console("cancel");
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.fake.mic_on);
  CHECK(!r.app.settings_list_live());
  hg::TouchGestures touch(r.app);
  tap_at(r, touch, 150, 150);
  CHECK(!r.fake.mic_on);
  CHECK(r.app.model().body.find("Check stopped") != std::string::npos);
  CHECK(r.app.settings_list_live());
  r.app.console("talk");
  r.app.console("release");
  CHECK(r.fake.mic_on);
  touch.update(true, 150, 80, r.fake.clock);
  touch.update(true, 150, 160, r.fake.clock + 30);
  touch.update(false, 0, 0, r.fake.clock + 40);
  CHECK(!r.fake.mic_on);
  CHECK_EQ(r.app.model().detail, std::string("Speaker check"));
}

TEST("settings list: Double press home and Talk from dark screen are toggle rows with their state") {
  Rig r;
  r.bring_online_with_profiles();
  CHECK(r.app.open_settings());
  const auto row = [&](const std::string& label) -> const hg::SettingRow* {
    for (const auto& s : r.app.model().settings)
      if (s.label == label) return &s;
    return nullptr;
  };
  const hg::SettingRow* home = row("Double press home");
  const hg::SettingRow* wake = row("Talk from dark screen");
  CHECK(home && wake);
  if (!home || !wake) return;
  CHECK(home->kind == hg::SettingRow::Kind::Toggle && home->level == 1);
  CHECK(home->icon == hg::SettingRow::Icon::Back);
  CHECK(wake->kind == hg::SettingRow::Kind::Toggle && wake->level == 0);
  CHECK(wake->icon == hg::SettingRow::Icon::Talk);
  r.app.console("set wake_talk on");
  r.app.console("set double_home off");
  CHECK(row("Talk from dark screen")->level == 1);
  CHECK(row("Double press home")->level == 0);
}

// --- The recording buffer: a lost connection doesn't lose the recording ------

namespace {

hg::DeviceProfile buffered_profile() {
  hg::DeviceProfile p = Rig::profile("ws://hermes.local:8765/gadget");
  p.recording_alloc = std::malloc;
  return p;
}

// Distinct samples, so the order of what reaches Hermes can be checked: sample i is i.
struct Speech {
  int16_t next = 0;
  std::vector<int16_t> all;
  void say(Rig& r, size_t samples) {
    std::vector<int16_t> pcm(samples);
    for (auto& s : pcm) s = next++;
    all.insert(all.end(), pcm.begin(), pcm.end());
    r.app.on_mic_samples(pcm.data(), pcm.size());
  }
  // 100 ms of speech, 100 ms of clock, `n` times.
  void talk(Rig& r, int n) {
    for (int i = 0; i < n; ++i) {
      say(r, 1600);
      r.advance(100);
    }
  }
};

// The audio frames sent for `stream` from sent_binary[from] on, in order: their samples,
// and whether their sequence numbers ran 0, 1, 2... without a gap.
std::vector<int16_t> uploaded(const Rig& r, int stream, size_t from, bool* in_order = nullptr) {
  std::vector<int16_t> pcm;
  uint16_t want = 0;
  bool ok = true;
  for (size_t i = from; i < r.fake.sent_binary.size(); ++i) {
    const auto& f = r.fake.sent_binary[i];
    hg::proto::BinaryFrame b;
    if (!hg::proto::parse_binary(f.data(), f.size(), b) || b.channel != hg::proto::Channel::Audio || b.stream != stream)
      continue;
    ok &= b.seq == want++;
    const size_t n = (f.size() - hg::proto::kBinaryHeader) / 2;
    const auto* s = reinterpret_cast<const int16_t*>(f.data() + hg::proto::kBinaryHeader);
    pcm.insert(pcm.end(), s, s + n);
  }
  if (in_order) *in_order = ok;
  return pcm;
}

// Waits for the next connection attempt (the first retry is 1 s after a loss) and lets it
// through; `paired` as the welcome says.
void reconnect(Rig& r, bool paired = true) {
  const int before = r.fake.connects;
  for (int i = 0; i < 400 && r.fake.connects == before; ++i) r.advance(100);
  CHECK_EQ(r.fake.connects, before + 1);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(std::string(R"({"type":"welcome","session":"s2","heartbeat_s":20,"paired":)") +
           (paired ? "true" : "false") + "}");
}

size_t count_type(const Rig& r, const std::string& type) {
  return static_cast<size_t>(std::count_if(r.fake.sent.begin(), r.fake.sent.end(),
                                           [&](const Value& v) { return v["type"].as_string() == type; }));
}

}  // namespace

TEST("recording buffer: sized for the utterance limit, and keeps what fits") {
  CHECK_EQ(hg::RecordingBuffer::bytes_for(30000, 16000), size_t(31 * 16000 * 2));
  CHECK_EQ(hg::RecordingBuffer::bytes_for(60000, 16000), size_t(61 * 16000 * 2));  // a 60 s limit: a constant
  hg::RecordingBuffer b;
  CHECK(!b.ready());
  CHECK(!b.reserve([](size_t) -> void* { return nullptr; }, 100));  // no room: off
  CHECK(!b.ready());
  CHECK(b.reserve(std::malloc, 10));
  const int16_t s[] = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(b.append(s, 6), size_t(5));  // 10 bytes: five samples
  CHECK_EQ(b.size(), size_t(10));
  b.advance(4);
  CHECK_EQ(b.unsent(), size_t(6));
  CHECK_EQ(reinterpret_cast<const int16_t*>(b.next())[0], int16_t(3));
  b.rewind();
  CHECK_EQ(b.unsent(), size_t(10));
  b.clear();
  CHECK_EQ(b.size(), size_t(0));
}

TEST("recording buffer: online, the recording streams in 40 ms frames as before") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  const Value* start = r.fake.last("audio.start");
  CHECK(start && (*start)["resent"].is_null());
  const int stream = start ? static_cast<int>((*start)["stream"].as_int()) : -1;
  sp.talk(r, 10);
  CHECK_EQ(r.fake.sent_binary.size(), size_t(25));  // 1 s: 25 frames of 640 samples, sent as they fill
  sp.say(r, 100);                                   // a part frame waits for the end
  CHECK_EQ(r.fake.sent_binary.size(), size_t(25));
  r.app.on_button(hg::Button::Talk, false);
  bool in_order = false;
  CHECK(uploaded(r, stream, 0, &in_order) == sp.all);
  CHECK(in_order);
  CHECK(r.fake.last("audio.end") != nullptr);
  CHECK(r.app.screen() == hg::Screen::Thinking);
}

TEST("recording buffer: at 24 kHz the buffered upload goes in 960-sample (40 ms) frames too") {
  hg::DeviceProfile p = buffered_profile();
  p.mic_rate = 24000;
  Rig r(p);
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(320, 7);  // the codec boards' 13 ms deliveries
  for (int i = 0; i < 31; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(10);
  }
  CHECK_EQ(r.fake.sent_binary.size(), size_t(10));
  for (const auto& f : r.fake.sent_binary) CHECK_EQ(f.size(), hg::proto::kBinaryHeader + 960 * 2);
  r.advance(400);
  r.app.on_button(hg::Button::Talk, false);
  CHECK_EQ(r.fake.sent_binary.size(), size_t(11));
  CHECK_EQ(r.fake.sent_binary.back().size(), hg::proto::kBinaryHeader + 320 * 2);
}

TEST("recording buffer: a slow link delays frames instead of dropping them, and audio.end waits") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  const int stream = static_cast<int>((*r.fake.last("audio.start"))["stream"].as_int());
  sp.talk(r, 3);
  r.fake.refuse_binary = true;  // the send queue is full
  sp.talk(r, 5);
  r.fake.logs.clear();
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") == nullptr);  // not before the audio it ends
  CHECK(r.app.screen() == hg::Screen::Thinking);
  r.fake.refuse_binary = false;
  r.advance(20);
  bool in_order = false;
  CHECK(uploaded(r, stream, 0, &in_order) == sp.all);
  CHECK(in_order);
  CHECK(r.fake.last("audio.end") != nullptr);
  bool none_lost = false;
  for (const auto& l : r.fake.logs) none_lost |= l.find(" 0 frames lost") != std::string::npos;
  CHECK(none_lost);
}

TEST("recording buffer: a connection lost mid-recording keeps recording, and all of it goes after reconnecting") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  const Value* first = r.fake.last("audio.start");
  const std::string id = first ? (*first)["id"].as_string() : "";
  const int old_stream = first ? static_cast<int>((*first)["stream"].as_int()) : -1;
  sp.talk(r, 5);
  r.app.on_transport_closed("disconnected");
  CHECK(r.app.in_turn());
  CHECK(r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Listening);  // not the connecting screen
  CHECK_EQ(r.app.model().detail, std::string("Offline, still recording"));
  sp.talk(r, 5);  // the connection is still down
  r.app.on_button(hg::Button::Talk, false);
  CHECK(!r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK_EQ(r.app.model().detail, std::string("Reconnecting to send..."));
  CHECK(r.fake.last("audio.end") == nullptr);

  const size_t sent_before = r.fake.sent_binary.size();
  reconnect(r);
  r.advance(20);
  const Value* again = r.fake.last("audio.start");
  CHECK(again && again != first);
  if (!again) return;
  CHECK_EQ((*again)["id"].as_string(), id);  // the same request
  const int stream = static_cast<int>((*again)["stream"].as_int());
  CHECK(stream != old_stream);  // on a new stream
  CHECK_EQ((*again)["resent"].as_int(), int64_t(1));
  CHECK((*again)["age_ms"].as_int() >= 1000);
  bool in_order = false;
  CHECK(uploaded(r, stream, sent_before, &in_order) == sp.all);  // every sample, from the first
  CHECK(in_order);
  const Value* end = r.fake.last("audio.end");
  CHECK(end && (*end)["stream"].as_int() == stream);
  CHECK(end && (*end)["duration_ms"].as_int() >= 1000);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK_EQ(r.app.model().detail, std::string("Sending..."));
  // And the turn goes on as usual.
  r.server(R"({"type":"turn.start","turn":"a1"})");
  r.server(R"({"type":"reply","turn":"a1","text":"Got it."})");
  r.server(R"({"type":"turn.end","turn":"a1","outcome":"success"})");
  r.advance(200);
  CHECK(!r.app.in_turn());
}

TEST("recording buffer: reconnecting during the recording sends what was held, then streams on") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 3);
  r.app.on_transport_closed("disconnected");
  sp.talk(r, 3);
  reconnect(r);  // still talking
  sp.talk(r, 4);
  CHECK(r.app.screen() == hg::Screen::Listening);
  CHECK(r.app.model().detail != std::string("Offline, still recording"));
  const Value* again = r.fake.last("audio.start");
  const int stream = again ? static_cast<int>((*again)["stream"].as_int()) : -1;
  r.app.on_button(hg::Button::Talk, false);
  bool in_order = false;
  CHECK(uploaded(r, stream, 0, &in_order) == sp.all);
  CHECK(in_order);
  CHECK_EQ(count_type(r, "audio.end"), size_t(1));
}

TEST("recording buffer: a finished recording waits 45 s for the connection, then is dropped") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.app.on_transport_closed("disconnected");
  r.app.on_button(hg::Button::Talk, false);
  // Every attempt fails (no open): each connect times out after 10 s, without extending the wait.
  r.advance(hg::App::kRecordingWaitMs - 1000);
  CHECK(r.fake.connects >= 3);
  CHECK(r.app.in_turn());
  CHECK(r.app.screen() == hg::Screen::Thinking);
  r.advance(1100);
  CHECK(!r.app.in_turn());
  CHECK_EQ(r.app.model().detail, std::string("Couldn't send the recording"));
  // Back online later: nothing is sent.
  r.advance(20000);
  r.app.on_transport_open();
  r.server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
  r.server(R"({"type":"welcome","session":"s3","heartbeat_s":20,"paired":true})");
  r.advance(100);
  CHECK_EQ(count_type(r, "audio.start"), size_t(1));
  CHECK(r.fake.last("audio.end") == nullptr);
}

TEST("recording buffer: cancel discards a held recording, while recording or waiting") {
  {
    Rig r(buffered_profile());
    r.bring_online(true);
    Speech sp;
    r.app.on_button(hg::Button::Talk, true);
    sp.talk(r, 5);
    r.app.on_transport_closed("disconnected");
    r.app.on_button(hg::Button::Cancel, true);
    r.app.on_button(hg::Button::Cancel, false);
    CHECK(!r.app.in_turn());
    CHECK(!r.fake.mic_on);
    reconnect(r);
    r.advance(100);
    CHECK_EQ(count_type(r, "audio.start"), size_t(1));
  }
  {
    Rig r(buffered_profile());
    r.bring_online(true);
    Speech sp;
    r.app.on_button(hg::Button::Talk, true);
    sp.talk(r, 5);
    r.app.on_transport_closed("disconnected");
    r.app.on_button(hg::Button::Talk, false);
    r.app.on_button(hg::Button::Cancel, true);
    r.app.on_button(hg::Button::Cancel, false);
    CHECK(!r.app.in_turn());
    reconnect(r);
    r.advance(100);
    CHECK_EQ(count_type(r, "audio.start"), size_t(1));
    CHECK(r.fake.last("audio.end") == nullptr);
    CHECK(r.fake.last("cancel") == nullptr);  // Hermes had no turn to cancel
  }
}

TEST("recording buffer: tap mode ends a held recording with a tap, and the limit still ends it") {
  hg::DeviceProfile p = buffered_profile();
  Rig r(p);
  r.bring_online(true);
  r.app.console("set talk_mode tap");
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Listening);
  sp.talk(r, 3);
  r.app.on_transport_closed("disconnected");
  sp.talk(r, 3);
  r.app.on_button(hg::Button::Talk, true);  // the tap that ends it, though not connected
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK_EQ(r.app.model().detail, std::string("Reconnecting to send..."));

  Rig limit(buffered_profile());
  limit.bring_online(true);
  Speech all;
  limit.app.on_button(hg::Button::Talk, true);
  limit.app.on_transport_closed("disconnected");
  all.talk(limit, hg::App::kMaxUtteranceMs / 100 + 2);  // 30 s and a bit, offline throughout
  CHECK(!limit.fake.mic_on);
  CHECK(limit.app.screen() == hg::Screen::Thinking);
  reconnect(limit);
  limit.advance(1000);  // the link takes the 30 s over a few ticks
  const Value* again = limit.fake.last("audio.start");
  const int stream = again ? static_cast<int>((*again)["stream"].as_int()) : -1;
  const auto got = uploaded(limit, stream, 0);
  CHECK(got.size() >= size_t(hg::App::kMaxUtteranceMs) * 16);  // all 30 s, nothing dropped
  CHECK(std::equal(got.begin(), got.end(), all.all.begin()));
  CHECK(limit.fake.last("audio.end") != nullptr);
}

TEST("recording buffer: a device unpaired on reconnect drops the held recording") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.app.on_transport_closed("disconnected");
  r.app.on_button(hg::Button::Talk, false);
  reconnect(r, false);
  r.advance(100);
  CHECK(!r.app.in_turn());
  CHECK_EQ(count_type(r, "audio.start"), size_t(1));
  CHECK(r.app.screen() == hg::Screen::Pairing);
}

TEST("recording buffer: a connection lost after audio.end went ends the turn, as before") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.fake.last("audio.end") != nullptr);
  r.app.on_transport_closed("disconnected");
  CHECK(!r.app.in_turn());
  reconnect(r);
  r.advance(100);
  CHECK_EQ(count_type(r, "audio.start"), size_t(1));  // Hermes has it: no second copy
}

TEST("recording buffer: a board without one ends the recording on a lost connection, as before") {
  Rig r;  // no recording_alloc
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.app.on_transport_closed("disconnected");
  CHECK(!r.app.in_turn());
  CHECK(!r.fake.mic_on);
  CHECK(r.app.screen() == hg::Screen::Connecting);
}

// --- Diagnostics for lost connections -----------------------------------------

namespace {
Value diag_of(Rig& r) {
  Value v;
  const std::string out = r.app.console("diag");
  CHECK(out.rfind("@diag {", 0) == 0);
  CHECK(hg::json::parse(out.substr(6), v));
  return v;
}
bool logged(const Rig& r, const std::string& text) {
  for (const auto& l : r.fake.logs)
    if (l.find(text) != std::string::npos) return true;
  return false;
}
}  // namespace

TEST("diag: every lost connection is logged and kept with what the device was doing") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(1600, 1000);
  r.app.on_mic_samples(pcm.data(), pcm.size());
  r.app.on_transport_closed("disconnected: a 1286 B send in flight for 10000 ms; transport errno 11; rssi -74");
  CHECK(logged(r, "connection closed: disconnected: a 1286 B send in flight for 10000 ms; transport errno 11; rssi -74 (recording)"));
  // A reconnect that times out counts too.
  r.advance(12000);
  CHECK(logged(r, "connection closed: connect timeout (idle)"));
  // The Wi-Fi goes while the next attempt is connecting: that attempt ends with the Wi-Fi's reason.
  const int attempts = r.fake.connects;
  for (int i = 0; i < 100 && r.fake.connects == attempts; ++i) r.advance(100);
  r.app.on_network(false, "Wi-Fi lost (reason 200 beacon timeout, rssi -80)");
  CHECK(logged(r, "network down: Wi-Fi lost (reason 200 beacon timeout, rssi -80) (idle)"));

  Value v = diag_of(r);
  CHECK(v["connection"]["closes_total"].as_int() >= 2);
  const auto& closes = v["connection"]["closes"].elements();
  CHECK(closes.size() >= 2);
  if (closes.size() < 2) return;
  CHECK_EQ(closes.front()["during"].as_string(), std::string("recording"));
  CHECK(closes.front()["reason"].as_string().find("rssi -74") != std::string::npos);
  bool timeout = false;
  for (const auto& c : closes) timeout |= c["reason"].as_string() == "connect timeout";
  CHECK(timeout);
  // The Wi-Fi loss ends the attempt, and its reason and signal stay after the network is back.
  r.app.on_network(true, "192.168.1.20");
  v = diag_of(r);
  const auto& after = v["connection"]["closes"].elements();
  CHECK(!after.empty());
  if (after.empty()) return;
  CHECK_EQ(after.back()["reason"].as_string(), std::string("Wi-Fi lost (reason 200 beacon timeout, rssi -80)"));
}

TEST("diag: a Wi-Fi loss after the WebSocket reported its own loss counts once") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(640, 1000);
  r.app.on_mic_samples(pcm.data(), pcm.size());
  r.app.on_transport_closed("disconnected; rssi -82");
  r.app.on_network(false, "Wi-Fi lost (reason 200 beacon timeout, rssi -82)");
  CHECK(logged(r, "network down: Wi-Fi lost (reason 200 beacon timeout, rssi -82) (idle)"));
  const Value v = diag_of(r);
  CHECK_EQ(v["connection"]["closes_total"].as_int(), int64_t(1));
  const auto& closes = v["connection"]["closes"].elements();
  CHECK_EQ(closes.size(), size_t(1));
  if (closes.empty()) return;
  CHECK_EQ(closes[0]["reason"].as_string(), std::string("disconnected; rssi -82"));
  CHECK_EQ(closes[0]["during"].as_string(), std::string("recording"));
  CHECK(r.app.screen() == hg::Screen::Offline);  // the network state still moves on
}

TEST("diag: a recording a lost connection cut short is the last recording") {
  Rig r;
  r.bring_online(true);
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(640, 1000);
  for (int i = 0; i < 15; ++i) {
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(40);
  }
  r.app.on_transport_closed("disconnected");
  Value v = diag_of(r);
  CHECK(v["recordings"]["last"]["ms"].as_int() >= 600);
  CHECK_EQ(v["recordings"]["last"]["frames_lost"].as_int(), int64_t(0));  // every frame went

  // Cut short with part of a frame built: that part never went.
  reconnect(r);
  r.app.on_button(hg::Button::Talk, true);
  r.app.on_mic_samples(pcm.data(), pcm.size());
  r.app.on_mic_samples(pcm.data(), 320);
  r.advance(500);
  r.app.on_transport_closed("disconnected");
  v = diag_of(r);
  CHECK_EQ(v["recordings"]["last"]["frames_lost"].as_int(), int64_t(1));
}

TEST("diag: the close history keeps the last five") {
  Rig r;
  r.bring_online(true);
  for (int i = 0; i < 7; ++i) {
    r.app.on_transport_closed("gone " + std::to_string(i));
    reconnect(r);
  }
  Value v = diag_of(r);
  CHECK_EQ(v["connection"]["closes_total"].as_int(), int64_t(7));
  const auto& closes = v["connection"]["closes"].elements();
  CHECK_EQ(closes.size(), size_t(5));
  if (closes.size() != 5) return;
  CHECK_EQ(closes.front()["reason"].as_string(), std::string("gone 2"));
  CHECK_EQ(closes.back()["reason"].as_string(), std::string("gone 6"));
  CHECK_EQ(closes.back()["during"].as_string(), std::string("idle"));
}

TEST("diag: the last recording's length and lost frames") {
  Rig r;
  r.bring_online(true);
  CHECK(diag_of(r)["recordings"]["last"].is_null());
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(640, 1000);
  for (int i = 0; i < 10; ++i) {
    r.fake.refuse_binary = i >= 4 && i < 7;
    r.app.on_mic_samples(pcm.data(), pcm.size());
    r.advance(40);
  }
  r.app.on_button(hg::Button::Talk, false);
  const Value v = diag_of(r);
  CHECK_EQ(v["recordings"]["last"]["frames_lost"].as_int(), int64_t(3));
  CHECK(v["recordings"]["last"]["ms"].as_int() >= 400);
}

TEST("recording buffer: a turn that ends some other way takes its unsent recording with it") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.fake.refuse_binary = true;  // the link never takes the rest
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.in_turn());
  for (int i = 0; i < 10; ++i) {  // past the 180 s Thinking gives Hermes, the server pinging meanwhile
    r.advance(18100);
    r.server(R"({"type":"ping","ts":1})");
  }
  CHECK(!r.app.in_turn());
  r.fake.refuse_binary = false;
  const size_t before = r.fake.sent_binary.size();
  r.advance(1000);
  CHECK_EQ(r.fake.sent_binary.size(), before);  // nothing more goes
  CHECK(r.fake.last("audio.end") == nullptr);
  const Value* cancel = r.fake.last("audio.cancel");  // and the hub drops what it has
  CHECK(cancel && (*cancel)["reason"].as_string() == "timeout");
}

TEST("diag: a buffered recording logs its length, frames lost and the link's backlog, and diag keeps the last") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 2);
  r.fake.refuse_binary = true;  // the link stalls for 300 ms of audio
  sp.say(r, 1600);
  sp.say(r, 1600);
  sp.say(r, 1600);
  r.fake.refuse_binary = false;
  sp.talk(r, 2);
  r.fake.logs.clear();
  r.app.on_button(hg::Button::Talk, false);
  CHECK(logged(r, " 0 frames lost (past the buffer), link backlog peaked at "));
  Value v = diag_of(r);
  CHECK(v["recordings"]["buffer"].as_int() > 0);
  const Value& last = v["recordings"]["last"];
  CHECK_EQ(last["frames_lost"].as_int(), int64_t(0));
  CHECK(last["backlog_peak_ms"].as_int() >= 300);
  CHECK_EQ(last["resends"].as_int(), int64_t(0));

  // Held through a lost connection and sent again: the log and diag say so.
  r.server(R"({"type":"turn.end","turn":"t","outcome":"success"})");
  r.advance(7000);
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.app.on_transport_closed("disconnected");
  CHECK(logged(r, "connection closed: disconnected (recording)"));
  CHECK(logged(r, "connection lost during a recording; still recording, 500 ms held"));
  r.app.on_button(hg::Button::Talk, false);
  r.advance(12000);  // a reconnect that times out
  CHECK(logged(r, "connection closed: connect timeout (sending a recording)"));
  r.fake.logs.clear();
  reconnect(r);
  r.advance(50);
  CHECK(logged(r, "sent again 1x after a lost connection"));
  v = diag_of(r);
  CHECK_EQ(v["recordings"]["held"].as_int(), int64_t(1));
  CHECK_EQ(v["recordings"]["resent"].as_int(), int64_t(1));
  CHECK_EQ(v["recordings"]["last"]["resends"].as_int(), int64_t(1));
}

TEST("diag: a recording dropped for want of a connection is counted") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 5);
  r.app.on_transport_closed("disconnected");
  r.app.on_button(hg::Button::Talk, false);
  r.advance(hg::App::kRecordingWaitMs + 500);
  CHECK_EQ(diag_of(r)["recordings"]["dropped"].as_int(), int64_t(1));
  CHECK(logged(r, "dropped a held recording: no connection within 45 s"));
}

namespace {
// PSRAM free for psram_alloc, the allocator main.cpp gives a board with PSRAM: it sets the
// buffer aside only when RecordingBuffer::kPsramHeadroom stays free after it.
size_t g_psram_free = 0;
void* psram_alloc(size_t n) {
  return hg::RecordingBuffer::leaves_headroom(g_psram_free, n) ? std::malloc(n) : nullptr;
}
}  // namespace

TEST("recording buffer: set aside only when about 1 MB of PSRAM stays free after it") {
  using hg::RecordingBuffer;
  const size_t n = RecordingBuffer::bytes_for(hg::App::kMaxUtteranceMs, 24000);  // the 2.06's 1.5 MB
  CHECK(RecordingBuffer::leaves_headroom(n + RecordingBuffer::kPsramHeadroom, n));
  CHECK(!RecordingBuffer::leaves_headroom(n + RecordingBuffer::kPsramHeadroom - 1, n));
  CHECK(!RecordingBuffer::leaves_headroom(n - 1, n));  // no wrap-around when PSRAM is short

  // A small PSRAM part (2 MB, 1.5 MB of it free): no buffer, and a lost connection ends the
  // recording as before. Start-up says so.
  hg::DeviceProfile small = Rig::profile("ws://hermes.local:8765/gadget");
  small.mic_rate = 24000;
  small.recording_alloc = psram_alloc;
  g_psram_free = 1536 * 1024;
  Rig r(small);
  r.bring_online(true);
  CHECK(logged(r, "no room for the recording buffer: a lost connection ends a recording"));
  CHECK_EQ(diag_of(r)["recordings"]["buffer"].as_int(), int64_t(0));
  r.app.on_button(hg::Button::Talk, true);
  std::vector<int16_t> pcm(960, 1);
  r.app.on_mic_samples(pcm.data(), pcm.size());
  r.advance(500);
  r.app.on_transport_closed("disconnected");
  CHECK(!r.app.in_turn());
  CHECK(!r.fake.mic_on);

  // An 8 MB part with room to spare: the buffer is on, start-up says so, and a recording
  // outlives a lost connection.
  hg::DeviceProfile big = small;
  g_psram_free = 7 * 1024 * 1024;
  Rig b(big);
  b.bring_online(true);
  CHECK(logged(b, "recording buffer on: " + std::to_string(n / 1024) + " KB, a lost connection keeps the recording"));
  CHECK_EQ(diag_of(b)["recordings"]["buffer"].as_int(), static_cast<int64_t>(n));
  b.app.on_button(hg::Button::Talk, true);
  b.app.on_mic_samples(pcm.data(), pcm.size());
  b.advance(500);
  b.app.on_transport_closed("disconnected");
  CHECK(b.app.in_turn());
  CHECK(b.fake.mic_on);
}

TEST("recording buffer: an earlier turn's reply arriving during the upload leaves the new recording alone") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  const int stream = static_cast<int>((*r.fake.last("audio.start"))["stream"].as_int());
  sp.talk(r, 3);
  r.fake.refuse_binary = true;  // a slow link: the end of the recording waits to go
  sp.talk(r, 5);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  // Hermes, not having the new recording yet, finishes the turn the user talked over.
  r.server(R"({"type":"turn.start","turn":"old"})");
  r.server(R"({"type":"reply.delta","turn":"old","text":"The weather is"})");
  r.server(R"({"type":"reply","turn":"old","text":"The weather is sunny."})");
  r.server(R"({"type":"audio.start","stream":9,"rate":16000,"format":"pcm16"})");
  r.server(R"({"type":"turn.end","turn":"old","outcome":"success"})");
  r.advance(100);
  CHECK(r.app.in_turn());
  CHECK(r.app.screen() == hg::Screen::Thinking);
  CHECK(r.fake.last("audio.end") == nullptr);
  r.fake.refuse_binary = false;
  r.advance(50);
  bool in_order = false;
  CHECK(uploaded(r, stream, 0, &in_order) == sp.all);  // every sample still goes
  CHECK(in_order);
  CHECK(r.fake.last("audio.end") != nullptr);
  // The new turn then goes as usual.
  r.server(R"({"type":"turn.start","turn":"new"})");
  r.server(R"({"type":"reply","turn":"new","text":"Done."})");
  r.server(R"({"type":"turn.end","turn":"new","outcome":"success"})");
  r.advance(200);
  CHECK_EQ(r.app.model().body, std::string("Done."));
}

TEST("recording buffer: a new session drops a recording still uploading") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  const int stream = static_cast<int>((*r.fake.last("audio.start"))["stream"].as_int());
  sp.talk(r, 3);
  r.fake.refuse_binary = true;
  sp.talk(r, 5);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  r.app.on_button(hg::Button::Cancel, true);  // held for a new session
  r.advance(2200);
  r.app.on_button(hg::Button::Cancel, false);
  CHECK(r.fake.last("session.new") != nullptr);
  const Value* cancel = r.fake.last("audio.cancel");
  CHECK(cancel && (*cancel)["stream"].as_int() == stream);
  CHECK(cancel && (*cancel)["reason"].as_string() == "new session");
  const size_t before = r.fake.sent_binary.size();
  r.fake.refuse_binary = false;
  r.advance(500);
  CHECK_EQ(r.fake.sent_binary.size(), before);  // nothing of it follows session.new
  CHECK(r.fake.last("audio.end") == nullptr);
}

TEST("recording buffer: talking over a recording still uploading replaces it, and the hub hears so") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  const int first = static_cast<int>((*r.fake.last("audio.start"))["stream"].as_int());
  sp.talk(r, 3);
  r.fake.refuse_binary = true;
  sp.talk(r, 5);
  r.app.on_button(hg::Button::Talk, false);
  CHECK(r.app.screen() == hg::Screen::Thinking);
  r.app.on_button(hg::Button::Talk, true);  // talks again before the first one has all gone
  const Value* cancel = r.fake.last("audio.cancel");
  CHECK(cancel && (*cancel)["stream"].as_int() == first);
  CHECK(cancel && (*cancel)["reason"].as_string() == "replaced");
  const Value* start = r.fake.last("audio.start");
  CHECK(start && (*start)["stream"].as_int() != first);
  r.fake.refuse_binary = false;
  r.advance(50);
  for (const auto& f : r.fake.sent_binary) {
    hg::proto::BinaryFrame b;
    if (hg::proto::parse_binary(f.data(), f.size(), b)) CHECK(b.stream != first || b.seq < 100);
  }
  CHECK(uploaded(r, first, 0).size() <= size_t(3 * 1600));  // only what went before the stall
}

TEST("recording buffer: once its audio.end goes, the Thinking timeout is the short one again") {
  Rig r(buffered_profile());
  r.bring_online(true);
  Speech sp;
  r.app.on_button(hg::Button::Talk, true);
  sp.talk(r, 3);
  r.fake.refuse_binary = true;
  sp.talk(r, 3);
  r.app.on_button(hg::Button::Talk, false);
  r.server(R"({"type":"turn.start","turn":"old"})");  // the earlier turn's, during the upload
  r.fake.refuse_binary = false;
  r.advance(50);
  CHECK(r.fake.last("audio.end") != nullptr);
  for (int i = 0; i < 10; ++i) {  // Hermes never answers this request; the server pings meanwhile
    r.advance(18100);
    r.server(R"({"type":"ping","ts":1})");
  }
  CHECK(!r.app.in_turn());
  CHECK_EQ(r.app.model().detail, std::string("No reply from Hermes"));
}
