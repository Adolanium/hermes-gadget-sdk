#include "hg/app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "hg/crypto.hpp"
#include "hg/protocol.hpp"

namespace hg {
namespace {

constexpr uint32_t kBootScreenMs = 800;
constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr uint32_t kHandshakeTimeoutMs = 10000;
constexpr uint32_t kStableSessionMs = 30000;
constexpr uint32_t kMinUtteranceMs = 350;
constexpr uint32_t kMaxUtteranceMs = 30000;
constexpr uint32_t kThinkingTimeoutMs = 180000;
constexpr uint32_t kSettleAfterReplyMs = 6000;
constexpr uint32_t kReplyLingerMs = 20000;    // reply text stays up this long before the mascot returns
constexpr uint32_t kNewSessionHoldMs = 2000;  // hold CANCEL this long to start a new session
constexpr uint32_t kNewSessionHintMs = 600;   // ...and show the countdown after this long
constexpr uint32_t kPromptArmMs = 600;        // presses this soon after a question appears don't answer it
constexpr uint32_t kPageLineMs = 1000;        // long text: reading time per line before the next page
constexpr uint32_t kManualScrollPauseMs = 15000;  // UP/DOWN pauses automatic paging this long
constexpr uint8_t kCaptionLines = 3;          // detail lines on pairing, setup and card screens
constexpr uint8_t kPromptLines = 4;           // ...and on a question (the renderer drops lines on small screens)
constexpr uint32_t kFrameMs = 100;
constexpr uint32_t kSensorIntervalMs = 2000;
constexpr uint32_t kDefaultCardMs = 15000;
constexpr size_t kMicChunkSamples = 640;  // 40 ms at 16 kHz per binary frame
constexpr uint32_t kBackoffMs[] = {1000, 2000, 4000, 8000, 15000, 30000};
constexpr size_t kBackoffSteps = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

const char* const kSettingKeys[] = {"name", "server", "token", "talk_mode", "volume", "wifi_ssid", "wifi_pass"};

bool is_secret(std::string_view key) { return key == "token" || key == "wifi_pass"; }

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}

}  // namespace

const App::Route App::kRoutes[] = {
    {"challenge", &App::h_challenge},     {"welcome", &App::h_welcome},
    {"pairing", &App::h_pairing},         {"paired", &App::h_paired},
    {"unpaired", &App::h_unpaired},       {"turn.start", &App::h_turn_start},
    {"turn.end", &App::h_turn_end},       {"status", &App::h_status},
    {"reply.delta", &App::h_reply_delta}, {"reply", &App::h_reply},
    {"audio.start", &App::h_audio_start}, {"audio.end", &App::h_audio_end},
    {"audio.abort", &App::h_audio_abort}, {"display", &App::h_display},
    {"image.start", &App::h_image_start}, {"image.end", &App::h_image_end},
    {"action", &App::h_action},           {"ping", &App::h_ping},
    {"notice", &App::h_notice},           {"error", &App::h_error},
    {"transcript", &App::h_transcript},   {"prompt", &App::h_prompt},
    {"prompt.close", &App::h_prompt_close},
};

App::App(Hal& hal, DeviceProfile profile) : hal_(hal), profile_(std::move(profile)) {}

void App::log(LogLevel level, std::string_view msg) {
  if (hal_.system) hal_.system->log(level, msg);
}

std::string App::setting(std::string_view key, std::string_view fallback) const {
  if (hal_.storage) {
    if (auto v = hal_.storage->get(key)) return *v;
  }
  return std::string(fallback);
}

void App::load_settings() {
  name_ = setting("name", profile_.default_name);
  server_url_ = setting("server", profile_.default_server_url);
  access_token_ = setting("token", profile_.default_access_token);
  talk_mode_ = setting("talk_mode", "hold") == "tap" ? TalkMode::Tap : TalkMode::Hold;
  int vol = std::atoi(setting("volume", "70").c_str());
  volume_ = static_cast<uint8_t>(std::max(0, std::min(100, vol)));
  if (hal_.speaker) hal_.speaker->set_volume(volume_);
}

void App::add_action(Action action) {
  for (auto& a : actions_) {
    if (a.name == action.name) {
      a = std::move(action);
      return;
    }
  }
  actions_.push_back(std::move(action));
}

void App::begin() {
  // Device key: generated once, persisted, never leaves the device after enrollment.
  std::string stored = setting("device_key");
  if (stored.empty() || !crypto::base64_decode(stored, key_) || key_.size() != 32) {
    key_.assign(32, 0);
    hal_.system->random_bytes(key_.data(), key_.size());
    if (hal_.storage) hal_.storage->set("device_key", crypto::base64_encode(key_.data(), key_.size()));
    log(LogLevel::Info, "generated a new device key");
  }
  device_id_ = proto::device_id_for_key(key_.data(), key_.size());
  load_settings();

  if (hal_.speaker) {
    Action vol;
    vol.name = "speaker.volume";
    vol.description = "Set the device speaker volume.";
    vol.params = json::Value::object();
    json::Value props = json::Value::object();
    json::Value pct = json::Value::object();
    pct.set("type", "integer").set("minimum", 0).set("maximum", 100).set("description", "Volume percent");
    props.set("percent", pct);
    json::Value req = json::Value::array();
    req.push("percent");
    vol.params.set("type", "object").set("properties", props).set("required", req);
    vol.handler = [this](const json::Value& args, json::Value& result, std::string& error) {
      if (!args["percent"].is_number()) {
        error = "percent is required";
        return false;
      }
      int p = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(100, args["percent"].as_int())));
      volume_ = static_cast<uint8_t>(p);
      hal_.speaker->set_volume(volume_);
      if (hal_.storage) hal_.storage->set("volume", std::to_string(p));
      result.set("percent", p);
      return true;
    };
    add_action(std::move(vol));
  }
  if (hal_.display && hal_.display->info().has_backlight) {
    Action bl;
    bl.name = "screen.brightness";
    bl.description = "Set the screen backlight brightness.";
    json::Value props = json::Value::object();
    json::Value pct = json::Value::object();
    pct.set("type", "integer").set("minimum", 5).set("maximum", 100).set("description", "Brightness percent");
    props.set("percent", pct);
    json::Value req = json::Value::array();
    req.push("percent");
    bl.params.set("type", "object").set("properties", props).set("required", req);
    bl.handler = [this](const json::Value& args, json::Value& result, std::string& error) {
      if (!args["percent"].is_number()) {
        error = "percent is required";
        return false;
      }
      int p = static_cast<int>(std::max<int64_t>(5, std::min<int64_t>(100, args["percent"].as_int())));
      hal_.display->set_backlight(static_cast<uint8_t>(p));
      result.set("percent", p);
      return true;
    };
    add_action(std::move(bl));
  }

  if (hal_.display) ui_ = std::make_unique<Ui>(*hal_.display);
  phase_ = Phase::Boot;
  phase_since_ = now();
  boot_until_ = now() + kBootScreenMs;
  update_model();
  char buf[96];
  std::snprintf(buf, sizeof(buf), "device %s ready (board %s, fw %s)", device_id_.c_str(), profile_.board.c_str(),
                profile_.firmware.c_str());
  log(LogLevel::Info, buf);
}

std::string App::next_id(char prefix) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%c%lu", prefix, static_cast<unsigned long>(++id_counter_));
  return buf;
}

// --------------------------------------------------------------------------
// Connection lifecycle

void App::on_network(bool up, std::string_view detail) {
  network_up_ = up;
  network_detail_ = std::string(detail);
  if (!up) {
    if (phase_ == Phase::Connecting || phase_ == Phase::Handshake || phase_ == Phase::Online) {
      hal_.transport->close();
      drop_session("network lost");
    }
    if (phase_ != Phase::Boot) {
      phase_ = Phase::NoNetwork;
      phase_since_ = now();
    }
    reconnect_pending_ = false;
  } else if (phase_ == Phase::NoNetwork) {
    reconnect_pending_ = true;
    reconnect_at_ = now();
  }
  update_model();
}

void App::connect_now() {
  reconnect_pending_ = false;
  if (server_url_.empty()) {
    fatal_ = "No Hermes server configured";
    phase_ = Phase::NoNetwork;
    update_model();
    return;
  }
  fatal_.clear();
  ++attempts_;
  transport_active_ = true;
  phase_ = Phase::Connecting;
  phase_since_ = now();
  log(LogLevel::Info, std::string("connecting to ") + server_url_);
  hal_.transport->connect(server_url_, proto::kSubprotocol);
  update_model();
}

void App::schedule_reconnect() {
  if (online_since_ && now() - online_since_ >= kStableSessionMs) backoff_ = 0;
  uint32_t delay = kBackoffMs[std::min<size_t>(static_cast<size_t>(backoff_), kBackoffSteps - 1)];
  if (backoff_ < static_cast<int>(kBackoffSteps) - 1) ++backoff_;
  reconnect_at_ = now() + delay;
  reconnect_pending_ = network_up_;
}

void App::drop_session(std::string_view reason) {
  last_close_ = std::string(reason);
  if (mode_ == Mode::Listening && hal_.mic) hal_.mic->stop();
  stop_playback();
  mode_ = Mode::Idle;
  overlay_ = Overlay::None;
  image_stream_ = -1;
  clear_prompt();
  transport_active_ = false;
  schedule_reconnect();
  online_since_ = 0;
  phase_ = network_up_ ? Phase::Connecting : Phase::NoNetwork;
  phase_since_ = now();
}

void App::on_transport_open() {
  if (!transport_active_ || phase_ != Phase::Connecting) return;
  phase_ = Phase::Handshake;
  phase_since_ = now();
  last_rx_ = now();
  send_hello();
  update_model();
}

void App::on_transport_closed(std::string_view reason) {
  // Closes we initiated (timeouts, reconfiguration) were already handled.
  if (!transport_active_) return;
  log(LogLevel::Warn, std::string("connection closed: ") + std::string(reason));
  drop_session(reason);
  update_model();
}

void App::send(const json::Value& msg) {
  std::string text = msg.dump();
  if (!hal_.transport->send_text(text)) log(LogLevel::Warn, "send failed");
}

void App::send_hello() {
  json::Value caps = json::Value::object();
  if (hal_.display) {
    DisplayInfo di = hal_.display->info();
    json::Value d = json::Value::object();
    d.set("width", di.width).set("height", di.height).set("color", true).set("charset", "ascii");
    if (di.round) d.set("shape", "round");
    if (ui_) {
      const UiLayout& l = ui_->layout();
      d.set("text_cols", l.body_cols).set("text_rows", l.body_rows);
      json::Value img = json::Value::object();
      img.set("width", ui_->area().width).set("height", l.main_h).set("format", "rgb565");
      d.set("image", img);
    }
    caps.set("display", d);
  }
  if (hal_.mic) {
    json::Value m = json::Value::object();
    m.set("rate", profile_.mic_rate).set("format", "pcm16");
    caps.set("mic", m);
  }
  if (hal_.speaker) {
    json::Value s = json::Value::object();
    s.set("rate", profile_.speaker_rate).set("format", "pcm16");
    caps.set("speaker", s);
  }
  json::Value inputs = json::Value::array();
  inputs.push("talk");
  if (profile_.has_cancel_button) inputs.push("cancel");
  if (profile_.has_scroll_buttons) inputs.push("up").push("down");
  caps.set("inputs", inputs);
  caps.set("talk_mode", talk_mode_ == TalkMode::Tap ? "tap" : "hold");

  json::Value acts = json::Value::array();
  for (const auto& a : actions_) {
    json::Value o = json::Value::object();
    o.set("name", a.name).set("description", a.description).set("params", a.params);
    acts.push(o);
  }
  json::Value sensors = json::Value::object();
  for (const auto& s : sensors_) sensors.set(s.first, s.second);

  json::Value hello = proto::message("hello");
  hello.set("proto", proto::kVersion)
      .set("device_id", device_id_)
      .set("name", name_)
      .set("board", profile_.board)
      .set("firmware", profile_.firmware)
      .set("caps", caps)
      .set("actions", acts)
      .set("sensors", sensors);
  if (!access_token_.empty()) hello.set("token", access_token_);
  send(hello);
  sensors_dirty_ = false;
}

void App::on_transport_text(std::string_view text) {
  last_rx_ = now();
  json::Value msg;
  std::string err;
  if (!json::parse(text, msg, &err) || !msg.is_object()) {
    log(LogLevel::Warn, "dropping malformed frame: " + err);
    return;
  }
  const std::string& type = msg["type"].as_string();
  for (const auto& r : kRoutes) {
    if (type == r.type) {
      (this->*r.handler)(msg);
      update_model();
      return;
    }
  }
  log(LogLevel::Debug, "ignoring message type " + type);
}

void App::h_challenge(const json::Value& m) {
  if (phase_ != Phase::Handshake) return;
  const std::string& nonce = m["nonce"].as_string();
  json::Value auth = proto::message("auth");
  if (m["enrolled"].as_bool()) {
    auth.set("mac", proto::auth_mac(key_.data(), key_.size(), device_id_, nonce));
  } else {
    // First contact: enroll the key. The server checks it hashes to device_id.
    auth.set("key", crypto::base64_encode(key_.data(), key_.size()));
  }
  send(auth);
}

void App::h_welcome(const json::Value& m) {
  if (phase_ != Phase::Handshake) return;
  phase_ = Phase::Online;
  phase_since_ = now();
  online_since_ = now();
  attempts_ = 0;
  paired_ = m["paired"].as_bool();
  int hb = static_cast<int>(m["heartbeat_s"].as_int(20));
  heartbeat_ms_ = static_cast<uint32_t>(std::max(5, std::min(300, hb))) * 1000u;
  if (paired_) {
    pairing_code_.clear();
    pairing_command_.clear();
  }
  log(LogLevel::Info, paired_ ? "online (paired)" : "online (waiting for pairing approval)");
}

void App::h_pairing(const json::Value& m) {
  paired_ = false;
  pairing_code_ = m["code"].as_string();
  pairing_command_ = m["command"].as_string();
}

void App::h_paired(const json::Value&) {
  paired_ = true;
  pairing_code_.clear();
  pairing_command_.clear();
  set_hint_flash("Paired with Hermes");
}

void App::h_unpaired(const json::Value&) {
  paired_ = false;
  mode_ = Mode::Idle;
  stop_playback();
}

// --------------------------------------------------------------------------
// Conversation

bool App::can_talk() const { return phase_ == Phase::Online && paired_; }

void App::h_turn_start(const json::Value& m) {
  active_turn_ = m["turn"].as_string();
  turn_done_ = false;
  reply_final_ = false;
  last_turn_rx_ = now();
  if (mode_ != Mode::Listening) {
    if (mode_ != Mode::Thinking) {
      reply_.clear();
      scroll_ = -1;
    }
    mode_ = Mode::Thinking;
    mode_since_ = now();
  }
  status_.clear();
}

void App::h_turn_end(const json::Value& m) {
  const std::string& turn = m["turn"].as_string();
  if (!active_turn_.empty() && !turn.empty() && turn != active_turn_) return;
  turn_done_ = true;
  last_turn_rx_ = now();
  if (m["outcome"].as_string() == "failure" && reply_.empty()) {
    notice_ = "Hermes could not complete that request";
    notice_until_ = now() + 8000;
  }
  if (mode_ == Mode::Thinking) {
    mode_ = reply_.empty() ? Mode::Idle : Mode::Responding;
    mode_since_ = now();
  }
}

void App::h_status(const json::Value& m) {
  status_ = m["text"].as_string();
  last_turn_rx_ = now();
}

void App::h_transcript(const json::Value& m) {
  // What speech-to-text heard; shown as the user's line while Hermes works.
  user_echo_ = m["text"].as_string();
  last_turn_rx_ = now();
  if (status_ == "Sending...") status_.clear();
}

void App::h_reply_delta(const json::Value& m) {
  if (mode_ == Mode::Listening) return;
  reply_ = m["text"].as_string();
  scroll_ = -1;
  last_turn_rx_ = now();
  if (mode_ != Mode::Responding) {
    mode_ = Mode::Responding;
    mode_since_ = now();
  }
}

void App::h_reply(const json::Value& m) {
  last_turn_rx_ = now();
  const std::string& text = m["text"].as_string();
  if (m["interim"].as_bool()) {
    status_ = text;
    return;
  }
  if (mode_ == Mode::Listening) return;
  reply_ = text;
  reply_final_ = true;
  scroll_ = 0;  // a finished reply is read from the top
  page_at_ = now() + page_dwell_ms();
  status_.clear();
  mode_ = Mode::Responding;
  mode_since_ = now();
}

void App::h_audio_start(const json::Value& m) {
  if (!hal_.speaker || mode_ == Mode::Listening) return;
  uint32_t rate = static_cast<uint32_t>(m["rate"].as_int(profile_.speaker_rate));
  if (m["format"].str_or("pcm16") != "pcm16") {
    log(LogLevel::Warn, "unsupported audio format");
    return;
  }
  if (!hal_.speaker->begin(rate)) {
    log(LogLevel::Warn, "speaker refused stream");
    return;
  }
  in_stream_ = static_cast<int>(m["stream"].as_int(0));
  last_turn_rx_ = now();
  status_.clear();  // audible reply: the working-state phrase is over
  if (mode_ != Mode::Responding) {
    mode_ = Mode::Responding;
    mode_since_ = now();
  }
}

void App::h_audio_end(const json::Value& m) {
  if (in_stream_ < 0 || m["stream"].as_int(-1) != in_stream_) return;
  hal_.speaker->end();
  in_stream_ = -1;
}

void App::h_audio_abort(const json::Value& m) {
  if (in_stream_ < 0 || m["stream"].as_int(-1) != in_stream_) return;
  stop_playback();
}

void App::stop_playback() {
  if (hal_.speaker && (in_stream_ >= 0 || hal_.speaker->busy())) hal_.speaker->abort();
  in_stream_ = -1;
}

bool App::speaking() const { return hal_.speaker && (in_stream_ >= 0 || hal_.speaker->busy()); }

void App::on_transport_binary(const uint8_t* data, size_t len) {
  last_rx_ = now();
  proto::BinaryFrame f;
  if (!proto::parse_binary(data, len, f)) return;
  if (f.channel == proto::Channel::Audio) {
    if (in_stream_ < 0 || f.stream != in_stream_ || !hal_.speaker) return;
    size_t n = f.payload_len / 2;
    pcm_buf_.resize(n);
    std::memcpy(pcm_buf_.data(), f.payload, n * 2);  // payload may be unaligned
    hal_.speaker->write(pcm_buf_.data(), n);
    return;
  }
  if (f.channel == proto::Channel::Image) {
    if (image_stream_ < 0 || f.stream != image_stream_ || !hal_.display) return;
    Canvas c = ui_->canvas();
    const UiLayout& l = ui_->layout();
    c.set_clip_rows(l.main_y, l.main_y + l.main_h);
    size_t total = static_cast<size_t>(image_w_) * static_cast<size_t>(image_h_);
    size_t n = std::min(f.payload_len / 2, total - std::min(total, image_px_));
    pcm_buf_.resize(n);  // reuse as a u16 scratch buffer
    std::memcpy(pcm_buf_.data(), f.payload, n * 2);
    const uint16_t* px = reinterpret_cast<const uint16_t*>(pcm_buf_.data());
    int first_row = static_cast<int>(image_px_ / static_cast<size_t>(image_w_));
    size_t i = 0;
    while (i < n) {
      int row = static_cast<int>((image_px_ + i) / static_cast<size_t>(image_w_));
      int col = static_cast<int>((image_px_ + i) % static_cast<size_t>(image_w_));
      int run = std::min<int>(image_w_ - col, static_cast<int>(n - i));
      c.blit(image_x_ + col, image_y_ + row, run, 1, px + i);
      i += static_cast<size_t>(run);
    }
    image_px_ += n;
    int last_row = static_cast<int>((image_px_ + static_cast<size_t>(image_w_) - 1) / static_cast<size_t>(image_w_));
    int y0 = std::max(l.main_y, image_y_ + first_row);
    int y1 = std::min(l.main_y + l.main_h, image_y_ + last_row);
    if (y1 > y0) ui_->flush(y0, y1);
  }
}

void App::h_display(const json::Value& m) {
  card_title_ = m["title"].as_string();
  card_body_ = m["body"].as_string();
  card_scroll_ = 0;
  page_at_ = now() + page_dwell_ms();
  double ttl = m["ttl_s"].as_number(kDefaultCardMs / 1000.0);
  overlay_ = Overlay::Card;
  overlay_until_ = ttl <= 0 ? 0 : now() + static_cast<uint32_t>(ttl * 1000);
}

void App::h_image_start(const json::Value& m) {
  if (!hal_.display || !ui_) return;
  const UiLayout& l = ui_->layout();
  const DisplayInfo& di = ui_->area();
  image_w_ = static_cast<int>(m["width"].as_int(0));
  image_h_ = static_cast<int>(m["height"].as_int(0));
  if (image_w_ <= 0 || image_h_ <= 0 || image_w_ > di.width || image_h_ > l.main_h ||
      m["format"].str_or("rgb565") != "rgb565") {
    log(LogLevel::Warn, "rejecting image with unsupported geometry");
    image_stream_ = -1;
    return;
  }
  image_stream_ = static_cast<int>(m["stream"].as_int(0));
  image_px_ = 0;
  image_x_ = (di.width - image_w_) / 2;
  image_y_ = l.main_y + (l.main_h - image_h_) / 2;
  // Clear the image area once; rows then stream in.
  Canvas c = ui_->canvas();
  c.set_clip_rows(l.main_y, l.main_y + l.main_h);
  c.fill_rect(0, l.main_y, di.width, l.main_h, rgb565(0, 0, 0));
  ui_->flush(l.main_y, l.main_y + l.main_h);
  overlay_ = Overlay::Image;
  double ttl = m["ttl_s"].as_number(30);
  overlay_until_ = ttl <= 0 ? 0 : now() + static_cast<uint32_t>(ttl * 1000);
}

void App::h_image_end(const json::Value& m) {
  if (m["stream"].as_int(-1) == image_stream_) image_stream_ = -1;
}

void App::h_prompt(const json::Value& m) {
  const std::string& id = m["id"].as_string();
  if (id.empty()) return;
  prompt_id_ = id;
  prompt_title_ = m["title"].as_string();
  prompt_text_ = m["text"].as_string();
  prompt_since_ = now();
  double ttl = m["ttl_s"].as_number(0);
  prompt_until_ = ttl <= 0 ? 0 : now() + static_cast<uint32_t>(ttl * 1000);
}

void App::h_prompt_close(const json::Value& m) {
  const std::string& id = m["id"].as_string();
  if (id.empty() || id == prompt_id_) clear_prompt();
}

bool App::prompt_showing() const { return !prompt_id_.empty() && mode_ != Mode::Listening; }

bool App::prompt_armed(uint32_t pressed_at) const {
  // Guards against a press meant for something else landing on a question that just appeared.
  return static_cast<int32_t>(pressed_at - prompt_since_) >= static_cast<int32_t>(kPromptArmMs);
}

void App::answer_prompt(bool yes) {
  if (prompt_id_.empty()) return;
  json::Value reply = proto::message("prompt.reply");
  reply.set("id", prompt_id_).set("answer", yes ? "yes" : "no");
  send(reply);
  clear_prompt();
  last_turn_rx_ = now();  // Hermes resumes the turn from here
  set_hint_flash(yes ? "Answered: yes" : "Answered: no");
}

void App::clear_prompt() {
  prompt_id_.clear();
  prompt_title_.clear();
  prompt_text_.clear();
  prompt_until_ = 0;
}

void App::h_action(const json::Value& m) {
  json::Value res = proto::message("action.result");
  res.set("id", m["id"]);
  const std::string& name = m["name"].as_string();
  const Action* found = nullptr;
  for (const auto& a : actions_) {
    if (a.name == name) found = &a;
  }
  if (!found || !found->handler) {
    res.set("ok", false).set("error", "unknown action: " + name);
  } else {
    json::Value result = json::Value::object();
    std::string error;
    bool ok = found->handler(m["args"], result, error);
    res.set("ok", ok);
    if (ok) res.set("result", result);
    else res.set("error", error.empty() ? "action failed" : error);
  }
  send(res);
}

void App::h_ping(const json::Value& m) {
  json::Value pong = proto::message("pong");
  pong.set("ts", m["ts"]);
  send(pong);
}

void App::h_notice(const json::Value& m) {
  notice_ = m["text"].as_string();
  double ttl = m["ttl_s"].as_number(8);
  notice_until_ = now() + static_cast<uint32_t>(std::max(1.0, ttl) * 1000);
}

void App::h_error(const json::Value& m) {
  const std::string& code = m["code"].as_string();
  std::string message = m["message"].as_string();
  log(LogLevel::Error, "server error " + code + ": " + message);
  if (code == "auth_failed" || code == "bad_token" || code == "protocol") {
    // Retrying immediately cannot help; surface it and back off hard.
    fatal_ = message.empty() ? code : message;
    backoff_ = static_cast<int>(kBackoffSteps) - 1;
  } else {
    notice_ = message.empty() ? code : message;
    notice_until_ = now() + 8000;
  }
}

// --------------------------------------------------------------------------
// Input

void App::on_button(Button button, bool pressed) {
  switch (button) {
    case Button::Talk:
      if (pressed) {
        if (!can_talk()) {
          set_hint_flash(phase_ == Phase::Online ? "Approve pairing first" : "Not connected to Hermes");
          return;
        }
        if (mode_ == Mode::Listening) {
          if (hands_free_) finish_listening();
          return;
        }
        if (prompt_showing()) {
          if (prompt_armed(now())) answer_prompt(true);
          break;
        }
        start_listening(talk_mode_ == TalkMode::Tap);
      } else if (mode_ == Mode::Listening && !hands_free_) {
        if (now() - mode_since_ < kMinUtteranceMs) {
          cancel_listening("too short");
          set_hint_flash("Hold " + profile_.talk_label + " while speaking");
        } else {
          finish_listening();
        }
      }
      break;
    case Button::Cancel:
      // Short press acts on release; holding past kNewSessionHoldMs starts a new session
      // instead (fired from tick(), so the user gets feedback without letting go).
      if (pressed) {
        cancel_held_ = true;
        cancel_down_at_ = now();
        cancel_long_fired_ = false;
        return;
      }
      if (!cancel_held_) return;
      cancel_held_ = false;
      if (cancel_long_fired_) return;
      if (prompt_showing()) {
        if (prompt_armed(cancel_down_at_)) answer_prompt(false);
      } else if (mode_ == Mode::Listening) cancel_listening("cancelled");
      else if (overlay_ != Overlay::None) dismiss_overlay();
      else if (mode_ == Mode::Thinking || mode_ == Mode::Responding) cancel_turn();
      if (hint_flash_.rfind("New session in", 0) == 0) hint_flash_until_ = now();
      break;
    case Button::Up:
    case Button::Down:
      if (!pressed) break;
      if (prompt_showing()) break;
      page_at_ = now() + kManualScrollPauseMs;
      if (mode_ == Mode::Idle && overlay_ == Overlay::None && !reply_.empty() &&
          static_cast<int32_t>(reply_until_ - now()) <= 0) {
        reply_until_ = now() + kReplyLingerMs;  // first press brings the last reply back
        scroll_ = 0;
        page_at_ = now() + page_dwell_ms();
      } else {
        scroll_body(button == Button::Up ? -1 : +1);
        if (mode_ == Mode::Idle) reply_until_ = now() + kReplyLingerMs;
      }
      break;
  }
  update_model();
}

void App::start_listening(bool hands_free) {
  stop_playback();
  dismiss_overlay();
  if (!hal_.mic || !hal_.mic->start(profile_.mic_rate)) {
    set_hint_flash("Microphone unavailable");
    return;
  }
  hands_free_ = hands_free;
  mic_stream_ = static_cast<uint8_t>(mic_stream_ % 250 + 1);
  mic_seq_ = 0;
  request_id_ = next_id('a');
  level_ = 0;
  Vad::Config vc;
  vc.sample_rate = profile_.mic_rate;
  vad_.reset(vc);
  json::Value start = proto::message("audio.start");
  start.set("id", request_id_)
      .set("stream", mic_stream_)
      .set("rate", profile_.mic_rate)
      .set("format", "pcm16")
      .set("mode", hands_free ? "tap" : "hold");
  send(start);
  mode_ = Mode::Listening;
  mode_since_ = now();
  reply_.clear();
  status_.clear();
  user_echo_.clear();
  scroll_ = -1;
}

void App::finish_listening() {
  if (hal_.mic) hal_.mic->stop();
  json::Value end = proto::message("audio.end");
  end.set("id", request_id_).set("stream", mic_stream_).set("duration_ms", now() - mode_since_);
  send(end);
  mode_ = Mode::Thinking;
  mode_since_ = now();
  last_turn_rx_ = now();
  status_ = "Sending...";
}

void App::cancel_listening(std::string_view why) {
  if (hal_.mic) hal_.mic->stop();
  json::Value c = proto::message("audio.cancel");
  c.set("id", request_id_).set("stream", mic_stream_).set("reason", why);
  send(c);
  mode_ = Mode::Idle;
  mode_since_ = now();
}

void App::start_new_session() {
  if (!can_talk()) {
    set_hint_flash("Not connected to Hermes");
    return;
  }
  if (mode_ == Mode::Listening) cancel_listening("new session");
  stop_playback();
  dismiss_overlay();
  clear_prompt();
  send(proto::message("session.new"));
  reply_.clear();
  user_echo_.clear();
  status_ = "Starting a new session";
  scroll_ = -1;
  mode_ = Mode::Thinking;
  mode_since_ = now();
  last_turn_rx_ = now();
  set_hint_flash("New session");
}

void App::cancel_turn() {
  stop_playback();
  send(proto::message("cancel"));
  mode_ = Mode::Idle;
  mode_since_ = now();
  set_hint_flash("Cancelled");
}

void App::on_mic_samples(const int16_t* samples, size_t count) {
  if (mode_ != Mode::Listening || count == 0) return;
  level_ = level_percent(rms(samples, count));
  size_t off = 0;
  while (off < count) {
    size_t n = std::min(kMicChunkSamples, count - off);
    frame_buf_.resize(proto::kBinaryHeader + n * 2);
    proto::write_binary_header(frame_buf_.data(), proto::Channel::Audio, mic_stream_, mic_seq_++);
    std::memcpy(frame_buf_.data() + proto::kBinaryHeader, samples + off, n * 2);
    hal_.transport->send_binary(frame_buf_.data(), frame_buf_.size());
    off += n;
  }
  if (hands_free_) {
    Vad::Result r = vad_.feed(samples, count);
    if (r == Vad::Result::EndOfSpeech) {
      finish_listening();
      update_model();
    } else if (r == Vad::Result::NoSpeech) {
      cancel_listening("no speech");
      set_hint_flash("Didn't hear anything");
      update_model();
    }
  }
}

void App::submit_text(std::string_view text) {
  std::string t = trim(text);
  if (t.empty()) return;
  if (!can_talk()) {
    set_hint_flash("Not connected to Hermes");
    update_model();
    return;
  }
  stop_playback();
  dismiss_overlay();
  request_id_ = next_id('t');
  json::Value msg = proto::message("text");
  msg.set("id", request_id_).set("text", t);
  send(msg);
  user_echo_ = t;
  reply_.clear();
  status_ = "Sending...";
  scroll_ = -1;
  mode_ = Mode::Thinking;
  mode_since_ = now();
  last_turn_rx_ = now();
  update_model();
}

void App::scroll_body(int delta) {
  const bool card = overlay_ == Overlay::Card;
  const std::string& body = card ? card_body_ : reply_;
  if (body.empty() || !ui_) return;
  int& pos = card ? card_scroll_ : scroll_;
  int total = static_cast<int>(wrap_text(body, ui_->layout().body_cols).size());
  int max_first = std::max(0, total - ui_->body_rows(model_));
  int cur = pos < 0 ? max_first : std::min(pos, max_first);
  pos = std::max(0, std::min(max_first, cur + delta));
}

uint32_t App::page_dwell_ms() const {
  int rows = ui_ ? ui_->layout().body_rows : 8;
  return std::max<uint32_t>(3000, static_cast<uint32_t>(rows) * kPageLineMs);
}

bool App::turn_page() {
  // Only text screens that are read from the top page; a streaming reply stays pinned to its end.
  if (!ui_ || model_.hero) return false;
  int* pos = nullptr;
  const std::string* body = nullptr;
  if (model_.screen == Screen::Card) {
    pos = &card_scroll_;
    body = &card_body_;
  } else if ((model_.screen == Screen::Responding || model_.screen == Screen::Ready) && scroll_ >= 0) {
    pos = &scroll_;
    body = &reply_;
  }
  if (!pos) return false;
  const int rows = ui_->body_rows(model_);
  const int total = static_cast<int>(wrap_text(*body, ui_->layout().body_cols).size());
  const int max_first = std::max(0, total - rows);
  if (*pos >= max_first) return false;
  *pos = std::min(max_first, *pos + std::max(1, rows - 1));  // keep one line of context
  const uint32_t t = now();
  if (mode_ == Mode::Idle && model_.screen == Screen::Ready) reply_until_ = t + kReplyLingerMs;
  if (model_.screen == Screen::Card && overlay_until_ && static_cast<int32_t>(overlay_until_ - t) < static_cast<int32_t>(page_dwell_ms())) {
    overlay_until_ = t + page_dwell_ms();  // let the last page be read
  }
  return true;
}

void App::dismiss_overlay() {
  if (overlay_ == Overlay::None) return;
  overlay_ = Overlay::None;
  image_stream_ = -1;
  if (ui_) ui_->invalidate();
}

void App::set_hint_flash(std::string text) {
  hint_flash_ = std::move(text);
  hint_flash_until_ = now() + 2500;
}

// --------------------------------------------------------------------------
// Telemetry & events

void App::set_sensor(std::string_view name, double value) {
  for (auto& s : sensors_) {
    if (s.first == name) {
      if (s.second != value) {
        s.second = value;
        sensors_dirty_ = true;
      }
      return;
    }
  }
  sensors_.emplace_back(std::string(name), value);
  sensors_dirty_ = true;
}

void App::flush_sensors() {
  if (!sensors_dirty_ || phase_ != Phase::Online) return;
  if (now() - sensors_sent_ < kSensorIntervalMs) return;
  json::Value s = json::Value::object();
  for (const auto& kv : sensors_) s.set(kv.first, kv.second);
  json::Value msg = proto::message("state");
  msg.set("sensors", s);
  send(msg);
  sensors_dirty_ = false;
  sensors_sent_ = now();
}

void App::emit_event(std::string_view name, json::Value data, bool notify_agent) {
  if (phase_ != Phase::Online) {
    log(LogLevel::Warn, "event dropped while offline: " + std::string(name));
    return;
  }
  json::Value msg = proto::message("event");
  msg.set("name", name).set("data", std::move(data)).set("notify", notify_agent);
  send(msg);
}

// --------------------------------------------------------------------------
// Main loop

void App::tick() {
  const uint32_t t = now();

  if (phase_ == Phase::Boot && static_cast<int32_t>(t - boot_until_) >= 0) {
    phase_ = network_up_ ? Phase::Connecting : Phase::NoNetwork;
    phase_since_ = t;
    if (network_up_) {
      reconnect_pending_ = true;
      reconnect_at_ = t;
    }
    update_model();
  }

  if (reconnect_pending_ && network_up_ && static_cast<int32_t>(t - reconnect_at_) >= 0) connect_now();

  if (phase_ == Phase::Connecting && !reconnect_pending_ && t - phase_since_ > kConnectTimeoutMs) {
    hal_.transport->close();
    drop_session("connect timeout");
    update_model();
  } else if (phase_ == Phase::Handshake && t - phase_since_ > kHandshakeTimeoutMs) {
    hal_.transport->close();
    drop_session("handshake timeout");
    update_model();
  } else if (phase_ == Phase::Online && t - last_rx_ > 3 * heartbeat_ms_) {
    hal_.transport->close();
    drop_session("server silent");
    update_model();
  }

  if (mode_ == Mode::Listening && t - mode_since_ > kMaxUtteranceMs) {
    finish_listening();
    update_model();
  }
  if (!prompt_id_.empty()) last_turn_rx_ = t;  // Hermes is waiting for the user, not stuck
  if (mode_ == Mode::Thinking && t - last_turn_rx_ > kThinkingTimeoutMs) {
    mode_ = Mode::Idle;
    notice_ = "No reply from Hermes";
    notice_until_ = t + 8000;
    update_model();
  }
  if (mode_ == Mode::Responding && !speaking()) {
    bool settled = turn_done_ || (reply_final_ && t - last_turn_rx_ > kSettleAfterReplyMs);
    if (settled) {
      mode_ = Mode::Idle;
      mode_since_ = t;
      reply_until_ = t + kReplyLingerMs;
      update_model();
    }
  }
  if (overlay_ != Overlay::None && overlay_until_ && static_cast<int32_t>(t - overlay_until_) >= 0) {
    dismiss_overlay();
    update_model();
  }
  if (!prompt_id_.empty() && prompt_until_ && static_cast<int32_t>(t - prompt_until_) >= 0) {
    clear_prompt();
    update_model();
  }
  if (notice_until_ && static_cast<int32_t>(t - notice_until_) >= 0) {
    notice_.clear();
    notice_until_ = 0;
    update_model();
  }
  if (cancel_held_ && !cancel_long_fired_ && !prompt_showing()) {
    uint32_t held = t - cancel_down_at_;
    if (held >= kNewSessionHoldMs) {
      cancel_long_fired_ = true;
      start_new_session();
      update_model();
    } else if (held >= kNewSessionHintMs && can_talk()) {
      uint32_t left = (kNewSessionHoldMs - held + 999) / 1000;
      std::string hint = "New session in " + std::to_string(left) + "s";
      if (hint != hint_flash_) {
        set_hint_flash(std::move(hint));
        update_model();
      }
    }
  }
  if (hint_flash_until_ && static_cast<int32_t>(t - hint_flash_until_) >= 0) {
    hint_flash_.clear();
    hint_flash_until_ = 0;
    update_model();
  }

  if (static_cast<int32_t>(t - page_at_) >= 0) {
    page_at_ = t + page_dwell_ms();
    if (turn_page()) update_model();
  }

  flush_sensors();

  if (t - frame_at_ >= kFrameMs) {
    frame_at_ = t;
    ++model_.frame;
    update_model();
  }
}

void App::update_model() {
  UiModel& m = model_;
  m.title = name_;
  m.code.clear();
  m.detail.clear();
  m.body.clear();
  m.yes.clear();
  m.no.clear();
  m.hero = false;
  m.caption_lines = 1;
  m.scroll = scroll_;
  m.level = level_;
  m.speaking = speaking();

  switch (phase_) {
    case Phase::NoNetwork:
    case Phase::Boot: m.link = Link::Offline; break;
    case Phase::Connecting:
    case Phase::Handshake: m.link = network_up_ ? Link::Connecting : Link::Offline; break;
    case Phase::Online: m.link = Link::Online; break;
  }
  if (m.link == Link::Offline && network_up_) m.link = Link::Network;

  const std::string talk = profile_.talk_label;
  if (phase_ == Phase::Boot) {
    m.screen = Screen::Boot;
    m.hero = true;
    m.headline = "Hermes";
    m.detail = "starting - fw " + profile_.firmware;
    m.hint = device_id_;
  } else if (!fatal_.empty()) {
    m.screen = Screen::Error;
    m.hero = true;
    m.caption_lines = kCaptionLines;
    m.headline = "Setup needed";
    m.detail = fatal_ + "\n" + (server_url_.empty() ? "Serial console: set server ws://<host>:8765/gadget"
                                                     : "Server: " + server_url_);
    m.hint = "see docs/getting-started.md";
  } else if (phase_ == Phase::NoNetwork) {
    m.screen = Screen::Offline;
    m.hero = true;
    m.caption_lines = 2;
    m.headline = "No network";
    m.detail = network_detail_.empty() ? "Waiting for Wi-Fi" : network_detail_;
    m.hint = "configure Wi-Fi via serial";
  } else if (phase_ == Phase::Connecting || phase_ == Phase::Handshake) {
    m.screen = Screen::Connecting;
    m.hero = true;
    m.caption_lines = kCaptionLines;
    m.headline = phase_ == Phase::Handshake ? "Authenticating" : "Connecting to Hermes";
    m.detail = server_url_;
    if (!last_close_.empty() && attempts_ > 1) m.detail += "\nError: " + last_close_;
    char buf[48];
    if (reconnect_pending_) {
      uint32_t wait = static_cast<int32_t>(reconnect_at_ - now()) > 0 ? (reconnect_at_ - now() + 999) / 1000 : 0;
      std::snprintf(buf, sizeof(buf), "retry in %lus", static_cast<unsigned long>(wait));
    } else {
      std::snprintf(buf, sizeof(buf), "attempt %d", attempts_);
    }
    m.hint = buf;
  } else if (!paired_) {
    m.screen = Screen::Pairing;
    m.hero = true;
    m.caption_lines = kCaptionLines;
    if (!pairing_code_.empty()) {
      m.code = pairing_code_;
      m.headline = "Pairing code " + pairing_code_;
      m.detail = "On the Hermes host, run: " + pairing_command_;
    } else {
      m.headline = "Pair this device";
      m.detail = "Asking Hermes for a pairing code...";
    }
    m.hint = "waiting for approval";
  } else if (mode_ == Mode::Listening) {
    m.screen = Screen::Listening;
    m.hero = true;
    m.headline = "Listening";
    m.detail = hands_free_ ? "Speak now - pause to send" : "Release " + talk + " to send";
    m.hint = profile_.cancel_label + " to discard";
  } else if (!prompt_id_.empty()) {
    m.screen = Screen::Prompt;
    m.hero = true;
    m.caption_lines = kPromptLines;
    m.headline = prompt_title_.empty() ? "Hermes asks" : prompt_title_;
    m.detail = prompt_text_;
    m.yes = talk + ": Yes";
    if (profile_.has_cancel_button) m.no = profile_.cancel_label + ": No";
    m.hint = "Hermes is waiting for you";
  } else if (overlay_ == Overlay::Image) {
    m.screen = Screen::Image;
    m.headline = "Image";
    m.hint = profile_.cancel_label + " to close";
  } else if (overlay_ == Overlay::Card) {
    m.screen = Screen::Card;
    m.headline = card_title_.empty() ? "Hermes" : card_title_;
    m.hint = profile_.cancel_label + " to close";
    // A short card is a caption under the mascot; a long one gets the text layout and pages.
    m.hero = !ui_ || static_cast<int>(wrap_text(card_body_, ui_->layout().hero_cols).size()) <= kCaptionLines;
    if (m.hero) {
      m.caption_lines = kCaptionLines;
      m.detail = card_body_;
    } else {
      m.body = card_body_;
      m.scroll = card_scroll_;
    }
  } else if (mode_ == Mode::Thinking) {
    m.screen = Screen::Thinking;
    m.hero = true;
    m.headline = "Thinking";
    // The live status phrase when Hermes sends one, else what speech-to-text heard.
    m.detail = status_;
    if (!user_echo_.empty()) {
      m.body = "\"" + user_echo_ + "\"";
      if (m.detail.empty()) m.detail = m.body;
    }
    m.hint = profile_.cancel_label + " to stop";
  } else if (mode_ == Mode::Responding) {
    m.screen = Screen::Responding;
    m.headline = m.speaking ? "Speaking" : "Hermes";
    m.detail = status_;
    m.body = reply_;
    m.hint = talk + " to reply";
    // Spoken audio can start before the text arrives: let the mascot talk meanwhile.
    m.hero = reply_.empty();
  } else {
    m.screen = Screen::Ready;
    m.headline = "Ready";
    m.body = reply_;
    m.hint = (talk_mode_ == TalkMode::Tap ? "tap " : "hold ") + talk + " to talk";
    bool showing_reply = !reply_.empty() && static_cast<int32_t>(reply_until_ - now()) > 0;
    m.hero = !showing_reply;
    if (m.hero) {
      m.headline = "Hi, I'm Hermes";
      m.detail = reply_.empty() || !profile_.has_scroll_buttons ? "Ask me anything" : "UP shows my last reply";
    }
  }
  bool keeps_detail = m.screen == Screen::Pairing || m.screen == Screen::Boot || m.screen == Screen::Prompt;
  if (!notice_.empty() && !keeps_detail) m.detail = notice_;
  if (!hint_flash_.empty()) m.hint = hint_flash_;
  if (ui_) ui_->render(m);
}

// --------------------------------------------------------------------------
// Console

std::string App::status_json() const {
  json::Value s = json::Value::object();
  const char* phase = "boot";
  switch (phase_) {
    case Phase::Boot: phase = "boot"; break;
    case Phase::NoNetwork: phase = "no_network"; break;
    case Phase::Connecting: phase = "connecting"; break;
    case Phase::Handshake: phase = "handshake"; break;
    case Phase::Online: phase = "online"; break;
  }
  s.set("device_id", device_id_)
      .set("name", name_)
      .set("board", profile_.board)
      .set("firmware", profile_.firmware)
      .set("phase", phase)
      .set("screen", screen_name(model_.screen))
      .set("paired", paired_)
      .set("server", server_url_)
      .set("network", network_up_);
  if (!pairing_code_.empty()) s.set("pairing_code", pairing_code_);
  if (!prompt_id_.empty()) s.set("prompt", prompt_id_);
  if (!fatal_.empty()) s.set("error", fatal_);
  return s.dump();
}

std::string App::console(std::string_view raw) {
  std::string line = trim(raw);
  if (line.empty()) return {};
  size_t sp = line.find(' ');
  std::string cmd = line.substr(0, sp);
  std::string rest = sp == std::string::npos ? std::string() : trim(std::string_view(line).substr(sp + 1));

  if (cmd == "help") {
    return "@help commands: status | get <key> | set <key> <value> | say <text> | talk | release | cancel | "
           "new-session | yes | no | "
           "reconnect | forget-key | factory-reset   keys: name server token talk_mode volume wifi_ssid wifi_pass";
  }
  if (cmd == "status") return "@status " + status_json();
  if (cmd == "get" || cmd == "set") {
    size_t ks = rest.find(' ');
    std::string key = rest.substr(0, ks);
    bool known = false;
    for (const char* k : kSettingKeys) known = known || key == k;
    if (!known) return "@error unknown key";
    if (cmd == "get") {
      std::string v = setting(key);
      json::Value out = json::Value::object();
      out.set("key", key).set("value", is_secret(key) && !v.empty() ? std::string("<set>") : v);
      return "@value " + out.dump();
    }
    std::string value = ks == std::string::npos ? std::string() : trim(std::string_view(rest).substr(ks + 1));
    if (!hal_.storage) return "@error no storage";
    if (value.empty()) hal_.storage->erase(key);
    else hal_.storage->set(key, value);
    load_settings();
    if (key == "server" || key == "token") {
      // Reconnect with the new endpoint or credentials.
      fatal_.clear();
      if (phase_ != Phase::Boot && phase_ != Phase::NoNetwork) {
        hal_.transport->close();
        drop_session("reconfigured");
      }
      backoff_ = 0;
      reconnect_pending_ = network_up_;
      reconnect_at_ = now();
    }
    if (on_setting_changed) on_setting_changed(key);
    update_model();
    return "@ok " + key;
  }
  if (cmd == "say") {
    submit_text(rest);
    return "@ok say";
  }
  if (cmd == "talk") {
    on_button(Button::Talk, true);
    return "@ok talk";
  }
  if (cmd == "release") {
    on_button(Button::Talk, false);
    return "@ok release";
  }
  if (cmd == "cancel") {
    on_button(Button::Cancel, true);
    on_button(Button::Cancel, false);
    return "@ok cancel";
  }
  if (cmd == "yes" || cmd == "no") {
    if (prompt_id_.empty()) return "@error no question to answer";
    answer_prompt(cmd == "yes");
    update_model();
    return "@ok " + cmd;
  }
  if (cmd == "new-session") {
    start_new_session();
    update_model();
    return can_talk() ? "@ok new-session" : "@error not connected";
  }
  if (cmd == "reconnect") {
    if (phase_ != Phase::Boot && phase_ != Phase::NoNetwork) {
      hal_.transport->close();
      drop_session("manual reconnect");
    }
    backoff_ = 0;
    reconnect_pending_ = network_up_;
    reconnect_at_ = now();
    update_model();
    return "@ok reconnect";
  }
  if (cmd == "forget-key" || cmd == "factory-reset") {
    if (!hal_.storage) return "@error no storage";
    hal_.storage->erase("device_key");
    if (cmd == "factory-reset") {
      for (const char* k : kSettingKeys) hal_.storage->erase(k);
    }
    return "@ok " + cmd + " (restart the device to apply)";
  }
  return "@error unknown command (try: help)";
}

}  // namespace hg
