#include "hg/app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "hg/crypto.hpp"
#include "hg/protocol.hpp"
#include "hg/wifi_networks.hpp"

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
constexpr int kDashboardVersion = 1;    // the dashboard message this firmware draws
constexpr size_t kDashMaxEach = 12;     // sessions, and tiles, kept from one dashboard message
constexpr size_t kMicChunkSamples = 640;  // 40 ms at 16 kHz per binary frame
constexpr size_t kOtaAckBytes = 16 * 1024;  // a firmware update acknowledges every 16 KB written
constexpr uint32_t kOtaIdleMs = 30000;      // ...and is abandoned when no data comes for this long
constexpr uint32_t kOtaRestartMs = 1000;    // time for ota.done to leave before the restart
constexpr uint32_t kBackoffMs[] = {1000, 2000, 4000, 8000, 15000, 30000};
constexpr size_t kBackoffSteps = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

const char* const kSettingKeys[] = {"name", "server", "token", "talk_mode", "volume", "brightness", "screen_timeout", "battery_display", "battery_estimate", "language", "wifi_ssid", "wifi_pass",
                                    "face_down_sleep", "clock", "time_format"};

// NVS limits storage keys to 15 characters; keep the public setting name unchanged.
std::string_view setting_storage_key(std::string_view key) {
  return key == "battery_estimate" ? std::string_view("batt_estimate") : key;
}

bool is_secret(std::string_view key) { return key == "token" || key == "wifi_pass"; }

// " en es ... auto": the `language` setting's values.
std::string language_codes() {
  std::string out;
  for (size_t i = 0; i < i18n::kLangCount; ++i) out += std::string(" ") + i18n::info(static_cast<i18n::Lang>(i)).code;
  return out + " auto";
}

// The language Hermes names in `welcome` (its own ids: "es", "pt-br", "zh-hant"), as one this firmware
// has: the id itself, else its primary subtag ("es-mx" -> "es"). None when it has neither.
std::optional<i18n::Lang> hermes_language(std::string_view id) {
  std::string lowered(id);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '_') c = '-';
  }
  if (auto exact = i18n::parse(lowered)) return exact;
  const size_t dash = lowered.find('-');
  return dash == std::string::npos ? std::nullopt : i18n::parse(std::string_view(lowered).substr(0, dash));
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return std::string(s.substr(a, b - a));
}

// Compares MACs without leaking where they first differ.
bool same_secret(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

// The clock face's time: 24-hour HH:MM, or 12-hour H:MM (midnight is 12), with no AM/PM.
std::string clock_time_text(const WallTime& w, bool h24) {
  char buf[8];  // room for any two uint8_t fields: the ESP-IDF compiler checks the worst case
  if (h24) std::snprintf(buf, sizeof buf, "%02u:%02u", unsigned(w.hour), unsigned(w.minute));
  else std::snprintf(buf, sizeof buf, "%u:%02u", unsigned(w.hour % 12 ? w.hour % 12 : 12), unsigned(w.minute));
  return buf;
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
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
    {"prompt.close", &App::h_prompt_close}, {"ota.offer", &App::h_ota_offer},
    {"ota.begin", &App::h_ota_begin},     {"ota.end", &App::h_ota_end},
    {"ota.abort", &App::h_ota_abort},     {"dashboard", &App::h_dashboard},
    {"profile", &App::h_profile},         {"face", &App::h_face},
};

App::App(Hal& hal, DeviceProfile profile) : hal_(hal), profile_(std::move(profile)) {}

void App::log(LogLevel level, std::string_view msg) {
  if (hal_.system) hal_.system->log(level, msg);
}

std::string App::setting(std::string_view key, std::string_view fallback) const {
  if (hal_.storage) {
    if (auto v = hal_.storage->get(setting_storage_key(key))) return *v;
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
  brightness_ = static_cast<uint8_t>(std::max(5, std::min(100, std::atoi(setting("brightness", "100").c_str()))));
  // The panel level itself is applied once, below, after the sleep flags clear.
  screen_timeout_ms_ = static_cast<uint32_t>(std::max(0, std::min(3600, std::atoi(setting("screen_timeout", "0").c_str())))) * 1000;
  const std::string indicator = setting("battery_display", "off");
  battery_display_ = indicator == "icon" ? BatteryIndicator::Icon
                       : indicator == "percentage" ? BatteryIndicator::Percentage
                       : indicator == "both" ? BatteryIndicator::Both : BatteryIndicator::Off;
  battery_estimate_enabled_ = setting("battery_estimate", "off") == "liion";
  const std::string language = setting("language", "en");
  lang_auto_ = language == "auto";
  lang_ = lang_auto_ ? hermes_lang_.value_or(i18n::Lang::En) : i18n::parse(language).value_or(i18n::Lang::En);
  face_down_sleep_ = setting("face_down_sleep", "off") == "on";
  clock_after_ms_ = static_cast<uint32_t>(std::max(0, std::min(3600, std::atoi(setting("clock", "0").c_str())))) * 1000;
  clock_24h_ = setting("time_format", "24h") != "12h";
  if (face_down_) return;  // a face-down screen stays dark until the gadget is turned over
  activity_at_ = now();
  display_dimmed_ = display_sleeping_ = false;
  clock_showing_ = false;  // a setting change is activity: the clock starts its rest again
  apply_backlight();
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
    if (hal_.storage && !hal_.storage->set("device_key", crypto::base64_encode(key_.data(), key_.size())))
      log(LogLevel::Error, "could not save the device key: this identity lasts until the next restart");
    log(LogLevel::Info, "generated a new device key");
  }
  device_id_ = proto::device_id_for_key(key_.data(), key_.size());
  load_settings();
  agent_profile_ = setting("profile", "default");  // shown until Hermes confirms it; not a console setting

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
      if (hal_.storage && !hal_.storage->set("volume", std::to_string(p))) log(LogLevel::Warn, "could not save volume");
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
      brightness_ = static_cast<uint8_t>(p);
      // Face down, the level is only stored: the turn-over (wake_display) applies it.
      // A dimmed or timed-out screen keeps its level too; the wake applies the new one.
      if (!face_down_) apply_backlight();
      if (hal_.storage && !hal_.storage->set("brightness", std::to_string(p))) log(LogLevel::Warn, "could not save brightness");
      result.set("percent", p);
      return true;
    };
    add_action(std::move(bl));
  }
  if (hal_.display) {
    Action lang;
    lang.name = "screen.language";
    lang.description = "Set the language of the device's own screen text (menus, hints, status), "
                       "or auto to follow Hermes's language. Replies are not affected.";
    json::Value props = json::Value::object();
    json::Value code = json::Value::object();
    json::Value codes = json::Value::array();
    for (size_t i = 0; i < i18n::kLangCount; ++i) codes.push(i18n::info(static_cast<i18n::Lang>(i)).code);
    codes.push("auto");
    code.set("type", "string").set("enum", codes).set("description", "Language code");
    props.set("language", code);
    json::Value req = json::Value::array();
    req.push("language");
    lang.params.set("type", "object").set("properties", props).set("required", req);
    lang.handler = [this](const json::Value& args, json::Value& result, std::string& error) {
      const std::string code = args["language"].is_string() ? args["language"].as_string() : std::string();
      if (code != "auto" && !i18n::parse(code)) {
        error = "language must be one of:" + language_codes();
        return false;
      }
      const std::string reply = console("set language " + code);
      if (reply.rfind("@ok", 0) != 0) {
        error = reply.size() > 7 ? reply.substr(7) : "could not save the language";
        return false;
      }
      result.set("language", code);
      return true;
    };
    add_action(std::move(lang));
  }

  add_timer_actions();  // timer.start/status/cancel, beside the built-in actions
  if (hal_.display) ui_ = std::make_unique<Ui>(*hal_.display);
  if (ui_ && profile_.touch_screen)  // about 7 mm: 7 * ppi / 25.4, rounded up
    ui_->set_touch_px((70 * (profile_.touch_ppi ? profile_.touch_ppi : DeviceProfile::kDefaultTouchPpi) + 253) / 254);
  if (profile_.frame_stats) set_frame_stats(true);
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
  if (ota_busy()) {
    if (ota_ == Ota::Receiving) hal_.updater->abort();
    ota_reset();
    log(LogLevel::Warn, "firmware update abandoned: the connection dropped");
  }
  if (mode_ == Mode::Listening && hal_.mic) hal_.mic->stop();
  stop_playback();
  mode_ = Mode::Idle;
  overlay_ = Overlay::None;
  image_stream_ = -1;
  // What the dashboard showed is stale now; the next connection sends it afresh.
  dashboard_ready_ = false;
  dash_rows_.clear();
  profile_pending_.clear();
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

bool App::send(const json::Value& msg) {
  std::string text = msg.dump();
  if (hal_.transport->send_text(text)) return true;
  log(LogLevel::Warn, "send failed");
  return false;
}

void App::send_hello() {
  json::Value caps = json::Value::object();
  if (hal_.display) {
    DisplayInfo di = hal_.display->info();
    json::Value d = json::Value::object();
    d.set("width", di.width).set("height", di.height).set("color", true).set("charset", "latin1");
    if (di.round) d.set("shape", "round");
    if (ui_) {
      const UiLayout& l = ui_->layout();
      d.set("text_cols", l.body_cols).set("text_rows", l.body_rows);
      // A display that holds only a band of rows refuses images (h_image_start),
      // so it doesn't offer them: the host then shows its unsupported-image notice.
      if (hal_.display->framebuffer_rows() == 0) {
        json::Value img = json::Value::object();
        img.set("width", ui_->area().width).set("height", l.main_h).set("format", "rgb565");
        d.set("image", img);
      }
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
    if (hal_.speaker_decoder) {
      // "format" stays PCM16 for servers that predate "formats".
      json::Value formats = json::Value::array();
      formats.push("pcm16");
      formats.push(hal_.speaker_decoder->format());
      s.set("formats", formats);
    }
    caps.set("speaker", s);
  }
  json::Value inputs = json::Value::array();
  inputs.push("talk");
  if (profile_.touch_screen) inputs.push("touch");
  if (profile_.has_cancel_button) inputs.push("cancel");
  if (profile_.has_scroll_buttons) inputs.push("up").push("down");
  caps.set("inputs", inputs);
  caps.set("talk_mode", talk_mode_ == TalkMode::Tap ? "tap" : "hold");
  if (ui_ && can_open_dashboard()) {
    json::Value dash = json::Value::object();
    dash.set("v", kDashboardVersion).set("rows", ui_->dash_rows());
    caps.set("dashboard", dash);
  }
  caps.set("profiles", true);
  if (ui_ && profile_.faces) caps.set("faces", 1);
  if (hal_.updater) {
    json::Value ota = json::Value::object();
    ota.set("max_size", hal_.updater->capacity());
    caps.set("ota", ota);
  }

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
  const std::string stored = setting("profile", "");  // only what Hermes confirmed, never what it showed
  if (!stored.empty()) hello.set("profile", stored);
  if (face_) hello.set("face", face_->hash);
  if (!access_token_.empty()) hello.set("token", access_token_);
  if (display_sleeping_) hello.set("display_sleeping", true);
  send(hello);
  sensors_dirty_ = false;
  display_sleep_sent_ = display_sleeping_;
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
  // Hermes's language, for a device whose `language` is auto (docs/protocol.md#welcome-server--device).
  hermes_lang_ = hermes_language(m["language"].str_or(""));
  if (lang_auto_) lang_ = hermes_lang_.value_or(i18n::Lang::En);
  int hb = static_cast<int>(m["heartbeat_s"].as_int(20));
  heartbeat_ms_ = static_cast<uint32_t>(std::max(5, std::min(300, hb))) * 1000u;
  if (paired_) {
    pairing_code_.clear();
    pairing_command_.clear();
  }
  // The held face stays only if the welcome names it, paired (else revoked, or a new route: the mascot).
  if (face_ && (!paired_ || m["face"].as_string() != face_->hash)) face_.reset();
  log(LogLevel::Info, paired_ ? "online (paired)" : "online (waiting for pairing approval)");
  agent_profiles_.clear();
  for (const auto& p : m["profiles"].elements()) {
    if (agent_profiles_.size() >= 8) break;
    const std::string& id = p["id"].as_string();
    if (id.empty()) continue;
    const std::string& name = p["name"].as_string();
    agent_profiles_.push_back({id, (name.empty() ? id : name).substr(0, 24)});
  }
  if (m["profile"].is_string()) set_agent_profile(m["profile"].as_string());
  profile_pending_.clear();  // the welcome says which profile answers
  // A freshly installed firmware has reached Hermes, so it can take the next update: keep it.
  if (hal_.updater && hal_.updater->pending_verify()) {
    hal_.updater->confirm();
    log(LogLevel::Info, "new firmware " + profile_.firmware + " reached Hermes; keeping it");
  }
}

void App::h_profile(const json::Value& m) {
  const std::string& error = m["error"].as_string();
  const std::string& profile = m["profile"].as_string();
  if (error.empty()) set_agent_profile(profile);  // a refusal changes nothing
  // Only the answer to a pending switch (its profile, or a refusal) ends the wait; a profile Hermes
  // sends on its own, such as a reset to default, does not.
  if (!profile_pending_.empty() && error.empty() && profile != profile_pending_) return;
  profile_pending_.clear();
  profile_notice(error.empty() ? tr(i18n::Msg::ProfileSwitched, {agent_profile_name()})
                 : tr(error == "busy"      ? i18n::Msg::ProfileRefusedBusy
                      : error == "unknown" ? i18n::Msg::ProfileRefusedUnknown
                                           : i18n::Msg::ProfileRefused));
}

void App::h_face(const json::Value& m) {
  // A face or a clear for another profile is late: it must not touch this profile's face.
  if (!profile_.faces || !paired_ || m["profile"].as_string() != agent_profile_) return;
  face_.reset();
  const std::string& bits = m["bits"].as_string();
  if (bits.empty()) return;
  const std::string& hash = m["hash"].as_string();
  mascot::Face face;
  if (hash.empty() || hash.size() > 64 || !mascot::parse(bits, face)) {
    log(LogLevel::Warn, "ignored a malformed face");
    return;
  }
  face.hash = hash;
  face_ = std::move(face);
}

// The notice line and the open settings item; not the hint bar, which settings hides.
void App::profile_notice(std::string text) {
  if (menu_ == Menu::Profile) check_result_ = text;
  notice_ = std::move(text);
  notice_until_ = now() + 4000;
}

void App::set_agent_profile(const std::string& id) {
  if (id.empty()) return;
  if (id != agent_profile_) face_.reset();  // the mascot until the new profile's face arrives
  agent_profile_ = id;
  if (!hal_.storage || setting("profile", "default") == id) return;
  if (id == "default") hal_.storage->erase("profile");
  else if (!hal_.storage->set("profile", id)) log(LogLevel::Warn, "could not save profile");
}

std::string App::agent_profile_name() const {
  for (const auto& p : agent_profiles_)
    if (p.id == agent_profile_) return p.name;
  return agent_profile_;
}

bool App::can_switch_profile() const {
  return phase_ == Phase::Online && paired_ && agent_profiles_.size() > 1 && mode_ == Mode::Idle && !prompt_showing() &&
         !ota_busy() && !wifi_setup_open() && profile_pending_.empty();
}

bool App::next_agent_profile() {
  size_t i = 0;
  while (i < agent_profiles_.size() && agent_profiles_[i].id != agent_profile_) ++i;
  return select_agent_profile(i < agent_profiles_.size() ? (i + 1) % agent_profiles_.size() : 0);
}

bool App::select_agent_profile(size_t index) {
  if (!can_switch_profile() || index >= agent_profiles_.size()) return false;
  if (profile_picker_open()) dismiss_overlay();
  const AgentProfile& next = agent_profiles_[index];
  if (next.id != agent_profile_) {
    json::Value msg = proto::message("profile.select");
    msg.set("profile", next.id);
    if (send(msg)) {
      profile_pending_ = next.id;
      profile_pending_at_ = now();
      profile_notice(tr(i18n::Msg::ProfileSwitching, {next.name}));
      notice_until_ = now() + kProfileReplyMs;  // shown until the answer replaces it
    } else {
      profile_notice(tr(i18n::Msg::ProfileCantSwitch));
    }
  }
  update_model();
  return true;
}

bool App::profile_name_hit(int x, int y) const { return ui_ && !display_sleeping_ && ui_->name_hit(model_, x, y); }

bool App::open_profile_picker() {
  if (!profile_.touch_screen || !can_switch_profile()) return false;
  if (settings_open()) close_settings();
  overlay_ = Overlay::Profiles;
  overlay_until_ = 0;
  profile_page_ = 0;
  notice_.clear();
  update_model();
  return true;
}

int App::profile_row_hit(int x, int y) const { return ui_ ? ui_->profile_row_hit(model_, x, y) : -1; }

void App::profile_tap(int x, int y) {
  const int row = profile_row_hit(x, y);
  if (row >= 0) {
    select_agent_profile(static_cast<size_t>(row));
  } else if (row == Ui::kMoreRow) {
    profile_page_ = ui_->profile_next_page(model_);
    update_model();
  }
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
  set_hint_flash(tr(i18n::Msg::PairedWithHermes));
}

void App::h_unpaired(const json::Value&) {
  paired_ = false;
  face_.reset();
  mode_ = Mode::Idle;
  stop_playback();
}

// --------------------------------------------------------------------------
// Conversation

bool App::can_talk() const { return phase_ == Phase::Online && paired_ && ota_ != Ota::Receiving; }

void App::h_turn_start(const json::Value& m) {
  wake_display();
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
    notice_ = tr(i18n::Msg::RequestFailed);
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
  wake_display();
  if (face_down_) reply_while_face_down_ = true;  // shown from the top on the turn-over
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
  wake_display();
  if (face_down_ && !m["interim"].as_bool()) reply_while_face_down_ = true;
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
  if (face_down_) return;  // face down mutes Hermes; the stream is never begun, its frames are dropped
  alarm_ringing_ = false;  // Hermes speaks now: the alarm stays on screen, quiet
  close_setup_screens();
  wake_display();
  if (settings_open()) close_settings();
  if (!hal_.speaker || mode_ == Mode::Listening) return;
  uint32_t rate = static_cast<uint32_t>(m["rate"].as_int(profile_.speaker_rate));
  const std::string_view format = m["format"].str_or("pcm16");
  const bool decoded = hal_.speaker_decoder && format == hal_.speaker_decoder->format();
  if (format != "pcm16" && !decoded) {
    log(LogLevel::Warn, "unsupported audio format");
    return;
  }
  if (decoded && !hal_.speaker_decoder->begin(rate)) {
    log(LogLevel::Warn, "decoder refused stream");
    return;
  }
  if (!hal_.speaker->begin(rate)) {
    log(LogLevel::Warn, "speaker refused stream");
    return;
  }
  in_stream_ = static_cast<int>(m["stream"].as_int(0));
  in_decoded_ = decoded;
  in_rate_ = rate;
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
  if (f.channel == proto::Channel::Firmware) {
    const Ota before = ota_;
    ota_chunk(f.stream, f.seq, f.payload, f.payload_len);
    if (ota_ != before) update_model();  // a failed chunk ends the update screen
    return;
  }
  if (f.channel == proto::Channel::Audio) {
    if (in_stream_ < 0 || f.stream != in_stream_ || !hal_.speaker) return;
    if (in_decoded_) {
      constexpr size_t kMaxPacketMs = 120;  // the longest Opus packet
      pcm_buf_.resize(static_cast<size_t>(in_rate_) * kMaxPacketMs / 1000);  // the stream's rate, not the profile's
      const int n = hal_.speaker_decoder->decode(f.payload, f.payload_len, pcm_buf_.data(), pcm_buf_.size());
      if (n > 0) hal_.speaker->write(pcm_buf_.data(), static_cast<size_t>(n));
      return;
    }
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
  if (alarm_) dismiss_alarm();  // a card replaces the alarm
  wake_display();
  card_title_ = m["title"].as_string();
  card_body_ = m["body"].as_string();
  card_scroll_ = 0;
  page_at_ = now() + page_dwell_ms();
  double ttl = m["ttl_s"].as_number(kDefaultCardMs / 1000.0);
  overlay_ = Overlay::Card;
  overlay_until_ = ttl <= 0 ? 0 : now() + static_cast<uint32_t>(ttl * 1000);
}

// A full snapshot: it replaces what the dashboard showed. Unlike a card it doesn't
// light the screen; the user opens the dashboard to look.
void App::h_dashboard(const json::Value& m) {
  if (m["v"].as_int(kDashboardVersion) != kDashboardVersion) return;  // a layout this firmware can't draw
  auto tone = [](std::string_view state) {
    if (state == "busy") return DashRow::Tone::Busy;
    if (state == "waiting") return DashRow::Tone::Waiting;
    if (state == "ok") return DashRow::Tone::Good;
    if (state == "warn") return DashRow::Tone::Warn;
    if (state == "bad") return DashRow::Tone::Bad;
    return DashRow::Tone::Dim;
  };
  std::vector<DashRow> rows;
  int busy = 0, waiting = 0;
  const json::Value& sessions = m["sessions"];
  rows.push_back({tr(i18n::Msg::HermesSessions), "", DashRow::Tone::Dim, true});
  for (size_t i = 0; i < sessions.size() && i < kDashMaxEach; ++i) {
    const json::Value& s = sessions[i];
    const std::string state(s["state"].str_or("idle"));
    busy += state == "busy";
    waiting += state == "waiting";
    const std::string shown = state == "waiting" ? tr(i18n::Msg::DashApproval)
                              : state == "busy"  ? tr(i18n::Msg::DashBusy)
                              : state == "idle"  ? tr(i18n::Msg::DashIdle)
                                                 : state;
    rows.push_back({s["label"].as_string(), shown, tone(state), false});
  }
  if (rows.size() == 1) rows.push_back({tr(i18n::Msg::NoSessions), "", DashRow::Tone::Dim, false});
  const json::Value& tiles = m["tiles"];
  if (tiles.size()) rows.push_back({tr(i18n::Msg::DashStatus), "", DashRow::Tone::Dim, true});
  for (size_t i = 0; i < tiles.size() && i < kDashMaxEach; ++i) {
    const json::Value& t = tiles[i];
    rows.push_back({t["label"].as_string(), t["value"].as_string(), tone(t["state"].str_or("")), false});
  }
  dash_rows_ = std::move(rows);
  dash_headline_ = waiting ? tr(i18n::Msg::NWaiting, {std::to_string(waiting)})
                   : busy  ? tr(i18n::Msg::NBusy, {std::to_string(busy)})
                           : std::string(tr(i18n::Msg::AllIdle));
  dashboard_ready_ = true;
  if (ui_) dash_scroll_ = std::min(dash_scroll_, std::max(0, static_cast<int>(dash_rows_.size()) - ui_->dash_rows()));
}

void App::h_image_start(const json::Value& m) {
  if (alarm_) dismiss_alarm();  // an image draws straight into the screen as it arrives
  close_setup_screens();
  wake_display();
  if (settings_open()) close_settings();
  if (!hal_.display || !ui_) return;
  const UiLayout& l = ui_->layout();
  const DisplayInfo& di = ui_->area();
  image_w_ = static_cast<int>(m["width"].as_int(0));
  image_h_ = static_cast<int>(m["height"].as_int(0));
  // An image is composed in the framebuffer over several packets, so a panel
  // that only holds a band of rows (no PSRAM) has nowhere to keep it.
  if (hal_.display->framebuffer_rows() > 0) {
    log(LogLevel::Warn, "rejecting image: this display has no full framebuffer");
    image_stream_ = -1;
    return;
  }
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
  close_setup_screens();
  wake_display();
  if (settings_open()) close_settings();
  const std::string& id = m["id"].as_string();
  if (id.empty()) return;
  prompt_id_ = id;
  prompt_title_ = m["title"].as_string();
  prompt_text_ = m["text"].as_string();
  prompt_since_ = now();
  double ttl = m["ttl_s"].as_number(0);
  prompt_until_ = ttl <= 0 ? 0 : now() + static_cast<uint32_t>(ttl * 1000);
  // The question goes first: a showing alarm steps back and rings again once the question is gone.
  if (alarm_) {
    dismiss_alarm();
    timer_due_ = true;
  }
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
  set_hint_flash(tr(yes ? i18n::Msg::AnsweredYes : i18n::Msg::AnsweredNo));
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
  if (ui_ && bench_ == Bench::None) ui_->mark_input();
  if (!wifi_setup_text_.empty()) {
    if (button == Button::Cancel && !pressed) close_wifi_setup();
    return;
  }
  if (phone_pairing_) {
    if (button == Button::Cancel && !pressed) close_phone_pairing();
    return;
  }
  const auto bit = static_cast<uint8_t>(1u << static_cast<unsigned>(button));
  if (pressed && wake_display()) { wake_buttons_ |= bit; return; }
  if (wake_buttons_ & bit) {
    if (!pressed) wake_buttons_ &= static_cast<uint8_t>(~bit);
    return;
  }
  // The alarm: a press dismisses it, and its release is swallowed above, so nothing records.
  if (alarm_ && model_.screen == Screen::Alarm && pressed) {
    dismiss_alarm();
    wake_buttons_ |= bit;
    return;
  }
  if (button == Button::Talk) {
    talk_held_ = pressed;
    if (pressed) talk_down_at_ = now();
  }
  if (settings_chord_fired_) {
    if (button == Button::Cancel) cancel_held_ = pressed;
    if (!talk_held_ && !cancel_held_) settings_chord_fired_ = false;
    return;
  }
  if (settings_open()) {
    if (button == Button::Cancel) {
      cancel_held_ = pressed;
      if (pressed) cancel_down_at_ = now();
    }
    if (!(talk_held_ && cancel_held_)) settings_input(button, pressed);
    update_model();
    return;
  }
  switch (button) {
    case Button::Talk:
      if (pressed) {
        if (cancel_held_) return;  // wait for the local settings chord
        if (!can_talk()) {
          set_hint_flash(tr(phase_ == Phase::Online ? i18n::Msg::ApprovePairingFirst : i18n::Msg::NotConnected));
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
          set_hint_flash(profile_.touch_screen ? std::string(tr(i18n::Msg::HoldScreenWhileSpeaking))
                                               : tr(i18n::Msg::HoldButtonWhileSpeaking, {control(profile_.talk_label)}));
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
        if (talk_held_ && mode_ == Mode::Listening) cancel_listening("cancelled");
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
      else if (dashboard_ready_ && paired_) {
        overlay_ = Overlay::Dashboard;
        overlay_until_ = 0;
        dash_scroll_ = 0;
        page_at_ = now() + page_dwell_ms();
      }
      if (hint_countdown_) hint_flash_until_ = now();
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
  if (alarm_) dismiss_alarm();
  if (!hal_.mic || !hal_.mic->start(profile_.mic_rate)) {
    set_hint_flash(tr(i18n::Msg::MicUnavailable));
    return;
  }
  hands_free_ = hands_free;
  mic_stream_ = static_cast<uint8_t>(mic_stream_ % 250 + 1);
  mic_seq_ = 0;
  mic_unsent_ = 0;
  request_id_ = next_id('a');
  level_ = 0;
  Vad::Config vc;
  vc.sample_rate = profile_.mic_rate;
  vc.min_threshold = profile_.vad_min_rms;  // the board's microphone level
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
  if (mic_unsent_)
    log(LogLevel::Warn, "recording: " + std::to_string(mic_unsent_) + " of " + std::to_string(mic_seq_) +
                            " audio frames not sent, the link couldn't keep up");
  if (!send(end)) {
    // Hermes would wait for the rest of the recording and never answer.
    mode_ = Mode::Idle;
    mode_since_ = now();
    notice_ = tr(i18n::Msg::CouldntSendRecording);
    notice_until_ = now() + 8000;
    return;
  }
  mode_ = Mode::Thinking;
  mode_since_ = now();
  last_turn_rx_ = now();
  status_ = tr(i18n::Msg::Sending);
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
    set_hint_flash(tr(i18n::Msg::NotConnected));
    return;
  }
  if (mode_ == Mode::Listening) cancel_listening("new session");
  stop_playback();
  dismiss_overlay();
  clear_prompt();
  send(proto::message("session.new"));
  reply_.clear();
  user_echo_.clear();
  status_ = tr(i18n::Msg::StartingNewSession);
  scroll_ = -1;
  mode_ = Mode::Thinking;
  mode_since_ = now();
  last_turn_rx_ = now();
  set_hint_flash(tr(i18n::Msg::NewSession));
}

void App::cancel_turn() {
  stop_playback();
  send(proto::message("cancel"));
  mode_ = Mode::Idle;
  mode_since_ = now();
  set_hint_flash(tr(i18n::Msg::Cancelled));
}

void App::on_mic_samples(const int16_t* samples, size_t count) {
  if (hardware_check_ == HardwareCheck::Microphone) {
    level_ = level_percent(rms(samples, count));
    update_model();
    return;
  }
  if (mode_ != Mode::Listening || count == 0) return;
  level_ = level_percent(rms(samples, count));
  size_t off = 0;
  while (off < count) {
    size_t n = std::min(kMicChunkSamples, count - off);
    frame_buf_.resize(proto::kBinaryHeader + n * 2);
    proto::write_binary_header(frame_buf_.data(), proto::Channel::Audio, mic_stream_, mic_seq_++);
    std::memcpy(frame_buf_.data() + proto::kBinaryHeader, samples + off, n * 2);
    if (!hal_.transport->send_binary(frame_buf_.data(), frame_buf_.size())) ++mic_unsent_;
    off += n;
  }
  if (hands_free_) {
    Vad::Result r = vad_.feed(samples, count);
    if (r == Vad::Result::EndOfSpeech) {
      finish_listening();
      update_model();
    } else if (r == Vad::Result::NoSpeech) {
      cancel_listening("no speech");
      set_hint_flash(tr(i18n::Msg::DidntHearAnything));
      update_model();
    }
  }
}

void App::submit_text(std::string_view text) {
  close_setup_screens();
  wake_display();
  if (settings_open()) close_settings();
  std::string t = trim(text);
  if (t.empty()) return;
  if (!can_talk()) {
    set_hint_flash(tr(i18n::Msg::NotConnected));
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
  status_ = tr(i18n::Msg::Sending);
  scroll_ = -1;
  mode_ = Mode::Thinking;
  mode_since_ = now();
  last_turn_rx_ = now();
  update_model();
}

void App::scroll_body(int delta) {
  if (overlay_ == Overlay::Dashboard) {
    if (!ui_) return;
    const int max_first = std::max(0, static_cast<int>(dash_rows_.size()) - ui_->dash_rows());
    dash_scroll_ = std::max(0, std::min(max_first, dash_scroll_ + delta));
    return;
  }
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
  if (model_.screen == Screen::Dashboard) {
    // A dashboard longer than the screen cycles through its pages, back to the top.
    const int rows = ui_->dash_rows(), total = static_cast<int>(dash_rows_.size());
    if (total <= rows) return false;
    dash_scroll_ = dash_scroll_ >= total - rows ? 0 : std::min(total - rows, dash_scroll_ + std::max(1, rows - 1));
    return true;
  }
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
  hint_countdown_ = false;
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
  if (hal_.power) {
    s.set("power_available", power_status_ ? 1 : 0);
    for (const char* key : {"battery_present", "battery_mv", "battery_percent", "charging", "external_power"})
      s.set(key, json::Value());
    if (power_status_) {
      const auto& p = *power_status_;
      if (p.battery_present) s.set("battery_present", *p.battery_present ? 1 : 0);
      if (p.battery_mv) s.set("battery_mv", *p.battery_mv);
      if (p.battery_percent) s.set("battery_percent", *p.battery_percent);
      if (p.charging) s.set("charging", *p.charging ? 1 : 0);
      if (p.external_power) s.set("external_power", *p.external_power ? 1 : 0);
    }
  }
  json::Value msg = proto::message("state");
  msg.set("sensors", s);
  send(msg);
  sensors_dirty_ = false;
  sensors_sent_ = now();
}

void App::flush_display_state() {
  // Not rate limited: Hermes holds back screen updates while it is dark and sends
  // the latest as soon as it lights up.
  if (phase_ != Phase::Online || display_sleeping_ == display_sleep_sent_) return;
  json::Value msg = proto::message("state");
  msg.set("display_sleeping", display_sleeping_);
  send(msg);
  display_sleep_sent_ = display_sleeping_;
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
// Firmware updates
//
// offer (size, SHA-256) -> ready (a fresh nonce) -> begin (the server's MAC over
// nonce, SHA-256 and size, keyed with this device's key) -> binary chunks, each
// 16 KB acknowledged -> end. The device checks the size, the SHA-256 and (through
// the port) the image itself before it switches, and only the holder of its key
// can authorize an image.

bool App::ota_busy() const { return ota_ == Ota::Offered || ota_ == Ota::Receiving; }

void App::ota_reset() {
  ota_ = Ota::Idle;
  ota_nonce_.clear();
  ota_sha_.clear();
  ota_version_.clear();
  ota_size_ = ota_received_ = ota_acked_ = 0;
  ota_hash_ = crypto::Sha256();
}

void App::ota_fail(std::string_view code, std::string_view message) {
  if (ota_ == Ota::Receiving) hal_.updater->abort();
  ota_reset();
  json::Value err = proto::message("ota.error");
  err.set("code", code).set("message", message);
  send(err);
  log(LogLevel::Warn, std::string("firmware update failed: ") + std::string(message));
  notice_ = tr(i18n::Msg::UpdateFailed, {message});
  notice_until_ = now() + 10000;
}

void App::h_ota_offer(const json::Value& m) {
  if (!hal_.updater) {
    ota_fail("unsupported", "this device can't install updates over the air");
    return;
  }
  if (ota_busy()) {  // a new offer replaces one in progress
    if (ota_ == Ota::Receiving) hal_.updater->abort();
    ota_reset();
  }
  const int64_t size = m["size"].as_int(0);
  const std::string sha = lower(m["sha256"].as_string());
  const int64_t stream = m["stream"].as_int(0);
  if (size <= 0 || sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos ||
      stream < 1 || stream > 255) {
    ota_fail("bad_offer", "an offer needs size, sha256 and stream");
    return;
  }
  if (static_cast<uint64_t>(size) > hal_.updater->capacity()) {
    ota_fail("too_large", "the image is " + std::to_string(size) + " bytes; the update slot holds " +
                              std::to_string(hal_.updater->capacity()));
    return;
  }
  uint8_t nonce[16];
  hal_.system->random_bytes(nonce, sizeof(nonce));
  ota_ = Ota::Offered;
  ota_nonce_ = crypto::base64_encode(nonce, sizeof(nonce));
  ota_size_ = static_cast<size_t>(size);
  ota_sha_ = sha;
  ota_version_ = m["version"].as_string();
  ota_stream_ = static_cast<uint8_t>(stream);
  ota_last_rx_ = now();
  json::Value ready = proto::message("ota.ready");
  ready.set("nonce", ota_nonce_);
  send(ready);
}

void App::h_ota_begin(const json::Value& m) {
  if (ota_ != Ota::Offered) {
    ota_fail("no_offer", "ota.begin came without an offer");
    return;
  }
  const std::string want = proto::ota_mac(key_.data(), key_.size(), device_id_, ota_nonce_, ota_sha_, ota_size_);
  if (!same_secret(want, m["mac"].as_string())) {
    ota_fail("unauthorized", "the update isn't authorized with this device's key");
    return;
  }
  std::string error;
  if (!hal_.updater->begin(ota_size_, error)) {
    ota_fail("flash", error.empty() ? "can't prepare the update slot" : error);
    return;
  }
  ota_ = Ota::Receiving;
  if (alarm_) dismiss_alarm();  // the Updating screen and its hint must show; no beep over the install
  close_setup_screens();
  close_settings();
  ota_received_ = ota_acked_ = 0;
  ota_seq_ = 0;
  ota_hash_ = crypto::Sha256();
  ota_last_rx_ = now();
  stop_playback();
  json::Value ack = proto::message("ota.ack");
  ack.set("offset", 0);
  send(ack);
  log(LogLevel::Info, "installing firmware " + ota_version_ + " (" + std::to_string(ota_size_) + " bytes)");
}

void App::ota_chunk(uint8_t stream, uint16_t seq, const uint8_t* data, size_t len) {
  if (ota_ != Ota::Receiving || stream != ota_stream_) return;
  if (seq != ota_seq_) {
    ota_fail("sequence", "a chunk of the image went missing");
    return;
  }
  ++ota_seq_;
  if (len > ota_size_ - ota_received_) {
    ota_fail("size", "more data than the offer said");
    return;
  }
  std::string error;
  if (!hal_.updater->write(data, len, error)) {
    ota_fail("flash", error.empty() ? "writing the update failed" : error);
    return;
  }
  ota_hash_.update(data, len);
  ota_received_ += len;
  ota_last_rx_ = now();
  if (ota_received_ - ota_acked_ >= kOtaAckBytes || ota_received_ == ota_size_) {
    ota_acked_ = ota_received_;
    json::Value ack = proto::message("ota.ack");
    ack.set("offset", ota_received_);
    send(ack);
    update_model();
  }
}

void App::h_ota_end(const json::Value&) {
  if (ota_ != Ota::Receiving) {
    ota_fail("no_update", "ota.end came without an update in progress");
    return;
  }
  if (ota_received_ != ota_size_) {
    ota_fail("size", "received " + std::to_string(ota_received_) + " of " + std::to_string(ota_size_) + " bytes");
    return;
  }
  crypto::Digest digest = ota_hash_.finish();
  if (crypto::hex(digest.data(), digest.size()) != ota_sha_) {
    ota_fail("checksum", "the image doesn't match its SHA-256");
    return;
  }
  std::string error;
  if (!hal_.updater->finish(error)) {
    ota_fail("invalid", error.empty() ? "the image was rejected" : error);
    return;
  }
  json::Value done = proto::message("ota.done");
  done.set("version", ota_version_);
  send(done);
  log(LogLevel::Info, "firmware " + ota_version_ + " installed; restarting into it");
  ota_ = Ota::Restarting;
  ota_restart_at_ = now() + kOtaRestartMs;
}

void App::h_ota_abort(const json::Value&) {
  if (!ota_busy()) return;
  if (ota_ == Ota::Receiving) hal_.updater->abort();
  ota_reset();
  log(LogLevel::Info, "firmware update cancelled by the server");
}

// --------------------------------------------------------------------------
// Main loop

void App::tick() {
  const uint32_t t = now();
  power_tick();
  motion_tick();
  clock_tick();
  settings_tick();
  phone_tick();

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
  } else if (!profile_pending_.empty() && t - profile_pending_at_ > kProfileReplyMs) {
    // The switch may still have happened: reconnect, and the welcome says which profile answers.
    hal_.transport->close();
    drop_session("no answer to the profile switch");
    update_model();
  }

  if (ota_busy() && t - ota_last_rx_ > kOtaIdleMs) {
    ota_fail("timeout", "the update stalled");
    update_model();
  }
  if (ota_ == Ota::Restarting && static_cast<int32_t>(t - ota_restart_at_) >= 0) {
    ota_ = Ota::Idle;  // hardware never returns from restart()
    hal_.updater->restart();
    update_model();
  }

  if (mode_ == Mode::Listening && t - mode_since_ > kMaxUtteranceMs) {
    finish_listening();
    update_model();
  }
  if (!prompt_id_.empty()) last_turn_rx_ = t;  // Hermes is waiting for the user, not stuck
  if (mode_ == Mode::Thinking && t - last_turn_rx_ > kThinkingTimeoutMs) {
    mode_ = Mode::Idle;
    notice_ = tr(i18n::Msg::NoReplyFromHermes);
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
  if (cancel_held_ && !talk_held_ && !settings_open() && !cancel_long_fired_ && !prompt_showing()) {
    uint32_t held = t - cancel_down_at_;
    if (held >= kNewSessionHoldMs) {
      cancel_long_fired_ = true;
      start_new_session();
      update_model();
    } else if (held >= kNewSessionHintMs && can_talk()) {
      uint32_t left = (kNewSessionHoldMs - held + 999) / 1000;
      std::string hint = tr(i18n::Msg::NewSessionIn, {std::to_string(left)});
      if (hint != hint_flash_) {
        set_hint_flash(std::move(hint));
        hint_countdown_ = true;
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
  flush_display_state();
  timer_tick();
  perf_tick();

  if (t - frame_at_ >= kFrameMs) {
    frame_at_ = t;
    // The port's time of day feeds the clock face; read once per frame.
    wall_valid_ = hal_.system && hal_.system->wall_time(wall_);
    ++model_.frame;
    update_model();
  }
}

void App::update_model() {
  UiModel& m = model_;
  m.title = name_;
  m.lang = lang_;
  m.code.clear();
  m.clock_time.clear();
  m.clock_date.clear();
  m.detail.clear();
  m.body.clear();
  m.yes.clear();
  m.no.clear();
  m.qr = nullptr;
  m.face = face_ ? &*face_ : nullptr;
  m.dash.clear();
  m.hero = false;
  m.name_target = false;
  m.caption_lines = 1;
  m.scroll = scroll_;
  m.level = level_;
  m.speaking = speaking();
  m.color_test = false;
  m.settings_hold = settings_hold_live();
  m.timer_permille = timer_permille();
  m.flash = false;
  m.battery = -1;
  m.battery_charging = m.battery_low = false;
  m.battery_icon = battery_display_ != BatteryIndicator::Percentage;
  m.battery_text = battery_display_ != BatteryIndicator::Icon;
  if (battery_display_ != BatteryIndicator::Off) {
    if (const auto percent = display_battery_percent()) {
      const auto& p = *power_status_;
      m.battery = static_cast<int8_t>(std::min<int>(*percent, 100));
      m.battery_charging = p.charging == true;
      m.battery_low = *percent <= 10 && p.external_power == false;
      // A voltage estimate reads high on a charger, so show the icon without a number there,
      // with the bolt even once the cell is full and the charger has stopped.
      if (battery_is_estimated() && (p.charging == true || p.external_power == true)) {
        m.battery_icon = true;
        m.battery_text = false;
        m.battery_charging = true;
      }
    }
  }

  switch (phase_) {
    case Phase::NoNetwork:
    case Phase::Boot: m.link = Link::Offline; break;
    case Phase::Connecting:
    case Phase::Handshake: m.link = network_up_ ? Link::Connecting : Link::Offline; break;
    case Phase::Online: m.link = Link::Online; break;
  }
  if (m.link == Link::Offline && network_up_) m.link = Link::Network;
  using i18n::Msg;
  const std::string talk = control(profile_.talk_label);
  const std::string cancel = control(profile_.cancel_label);
  if (!wifi_setup_text_.empty()) {
    m.screen = Screen::Setup;
    m.headline = tr(Msg::WifiSetup);
    m.detail = tr(Msg::ConnectYourPhone);
    m.body = wifi_setup_text_;
    m.qr = wifi_setup_qr_.ok() ? &wifi_setup_qr_ : nullptr;
    m.scroll = 0;
    m.hint = profile_.touch_screen ? std::string(tr(Msg::SwipeDownToClose)) : tr(Msg::ButtonToClose, {cancel});
    render(m);
    return;
  }
  if (phone_pairing_) {
    m.screen = Screen::Setup;
    m.scroll = 0;
    if (phone_passkey_.empty()) {
      m.headline = tr(Msg::PairAPhone);
      m.detail = tr(Msg::BluetoothName, {name_});
      m.body = tr(Msg::PhonePairIntro, {name_});
    } else {
      m.headline = tr(Msg::PhoneCode, {phone_passkey_});
      m.detail = tr(Msg::TypeCodeOnPhone);
      m.body = tr(Msg::OnlyPhoneInFront);
    }
    m.hint = profile_.touch_screen ? std::string(tr(Msg::SwipeDownToCancel)) : tr(Msg::ButtonToCancel, {cancel});
    render(m);
    return;
  }
  if (settings_open()) {
    settings_model();
    if (!display_sleeping_) render(m);
    return;
  }

  if (phase_ == Phase::Boot) {
    m.screen = Screen::Boot;
    m.hero = true;
    m.headline = "Hermes";
    m.detail = tr(Msg::StartingFirmware, {profile_.firmware});
    m.hint = device_id_;
  } else if (ota_ == Ota::Receiving || ota_ == Ota::Restarting) {
    m.screen = Screen::Updating;
    m.hero = true;
    m.caption_lines = 2;
    const std::string version =
        ota_version_.empty() ? std::string(tr(Msg::NewFirmware)) : tr(Msg::FirmwareVersion, {ota_version_});
    if (ota_ == Ota::Restarting) {
      m.headline = tr(Msg::Restarting);
      m.detail = tr(Msg::Installed, {version});
    } else {
      m.headline = tr(Msg::Updating);
      unsigned pct = ota_size_ ? static_cast<unsigned>(static_cast<uint64_t>(ota_received_) * 100 / ota_size_) : 0;
      m.detail = tr(Msg::UpdateProgress, {version, std::to_string(pct), std::to_string((ota_size_ + 1023) / 1024)});
    }
    m.hint = tr(Msg::KeepPowerOn);
  } else if (alarm_) {
    m.screen = Screen::Alarm;
    m.headline = tr(Msg::TimerDone);
    m.detail = timer_label_;
    m.hint = profile_.touch_screen ? std::string(tr(Msg::TapScreenToStop)) : tr(Msg::ButtonToStop, {talk});
    m.flash = alarm_flash();
    // Its own return, as the settings branch has: the tail below must not overwrite the label or hint.
    if (!display_sleeping_) render(m);
    return;
  } else if (!fatal_.empty()) {
    m.screen = Screen::Error;
    m.hero = true;
    m.caption_lines = kCaptionLines;
    m.headline = tr(Msg::SetupNeeded);
    // The status report keeps the English reason; the screen shows it translated.
    const std::string reason = fatal_ == i18n::english(Msg::NoServerConfigured) ? tr(Msg::NoServerConfigured) : fatal_;
    m.detail = reason + "\n" + (server_url_.empty() ? std::string(tr(Msg::SerialSetServer)) : tr(Msg::ServerUrl, {server_url_}));
    m.hint = tr(Msg::SeeDocs);
  } else if (phase_ == Phase::NoNetwork) {
    m.screen = Screen::Offline;
    m.hero = true;
    m.caption_lines = 2;
    m.headline = tr(Msg::NoNetwork);
    m.detail = network_detail_.empty() ? std::string(tr(Msg::WaitingForWifi)) : network_detail_;
    m.hint = tr(Msg::ConfigureWifiSerial);
  } else if (phase_ == Phase::Connecting || phase_ == Phase::Handshake) {
    m.screen = Screen::Connecting;
    m.hero = true;
    m.caption_lines = kCaptionLines;
    m.headline = tr(phase_ == Phase::Handshake ? Msg::Authenticating : Msg::ConnectingToHermes);
    m.detail = server_url_;
    if (!last_close_.empty() && attempts_ > 1) m.detail += "\n" + tr(Msg::ConnectError, {last_close_});
    if (reconnect_pending_) {
      uint32_t wait = static_cast<int32_t>(reconnect_at_ - now()) > 0 ? (reconnect_at_ - now() + 999) / 1000 : 0;
      m.hint = tr(Msg::RetryIn, {std::to_string(wait)});
    } else {
      m.hint = tr(Msg::Attempt, {std::to_string(attempts_)});
    }
  } else if (!paired_) {
    m.screen = Screen::Pairing;
    m.hero = true;
    m.caption_lines = kCaptionLines;
    if (!pairing_code_.empty()) {
      m.code = pairing_code_;
      m.headline = tr(Msg::PairingCode, {pairing_code_});
      m.detail = tr(Msg::RunOnHost, {pairing_command_});
    } else {
      m.headline = tr(Msg::PairThisDevice);
      m.detail = tr(Msg::AskingForCode);
    }
    m.hint = tr(Msg::WaitingForApproval);
  } else if (mode_ == Mode::Listening) {
    m.screen = Screen::Listening;
    m.hero = true;
    m.headline = tr(Msg::Listening);
    m.detail = hands_free_              ? std::string(tr(Msg::SpeakNowPause))
               : profile_.touch_screen ? std::string(tr(Msg::LiftFingerToSend))
                                       : tr(Msg::ReleaseToSend, {talk});
    m.hint = tr(Msg::ButtonToDiscard, {cancel});
  } else if (!prompt_id_.empty()) {
    m.screen = Screen::Prompt;
    m.hero = true;
    m.caption_lines = kPromptLines;
    m.headline = prompt_title_.empty() ? std::string(tr(Msg::HermesAsks)) : prompt_title_;
    m.detail = prompt_text_;
    m.yes = profile_.touch_screen ? std::string(tr(Msg::TapYes)) : tr(Msg::ButtonYes, {talk});
    if (profile_.has_cancel_button)
      m.no = profile_.touch_screen ? std::string(tr(Msg::SwipeNo)) : tr(Msg::ButtonNo, {cancel});
    m.hint = tr(Msg::HermesWaiting);
  } else if (overlay_ == Overlay::Image) {
    m.screen = Screen::Image;
    m.headline = tr(Msg::Image);
    m.hint = tr(Msg::ButtonToClose, {cancel});
  } else if (overlay_ == Overlay::Profiles) {
    m.screen = Screen::Profiles;
    m.headline = tr(Msg::ProfileListTitle);
    for (const auto& p : agent_profiles_)
      m.dash.push_back({p.name, p.id == agent_profile_ ? tr(Msg::ProfileListCurrent) : ">",
                        p.id == agent_profile_ ? DashRow::Tone::Waiting : DashRow::Tone::Dim});
    m.hint = tr(Msg::ProfileListHint);
    m.detail = tr(Msg::ProfileListMore);
    m.scroll = profile_page_;
  } else if (overlay_ == Overlay::Dashboard) {
    m.screen = Screen::Dashboard;
    m.headline = dash_headline_;
    m.dash = dash_rows_;
    m.scroll = dash_scroll_;
    m.hint = profile_.touch_screen ? std::string(tr(Msg::SwipeDownToClose)) : tr(Msg::ButtonToClose, {cancel});
  } else if (overlay_ == Overlay::Card) {
    m.screen = Screen::Card;
    m.headline = card_title_.empty() ? "Hermes" : card_title_;
    m.hint = tr(Msg::ButtonToClose, {cancel});
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
    m.headline = tr(Msg::Thinking);
    // The live status phrase when Hermes sends one, else what speech-to-text heard.
    m.detail = status_;
    if (!user_echo_.empty()) {
      m.body = "\"" + user_echo_ + "\"";
      if (m.detail.empty()) m.detail = m.body;
    }
    m.hint = tr(Msg::ButtonToStop, {cancel});
  } else if (mode_ == Mode::Responding) {
    m.screen = Screen::Responding;
    m.headline = m.speaking ? tr(Msg::Speaking) : "Hermes";
    m.detail = status_;
    m.body = reply_;
    m.hint = profile_.touch_screen ? std::string(tr(Msg::HoldToReply)) : tr(Msg::ButtonToReply, {talk});
    // Spoken audio can start before the text arrives: let the mascot talk meanwhile.
    m.hero = reply_.empty();
  } else {
    m.screen = Screen::Ready;
    m.headline = tr(Msg::Ready);
    m.body = reply_;
    const bool tap = talk_mode_ == TalkMode::Tap;
    m.hint = profile_.touch_screen ? std::string(tr(tap ? Msg::TapScreenToTalk : Msg::HoldScreenToTalk))
                                   : tr(tap ? Msg::TapButtonToTalk : Msg::HoldButtonToTalk, {talk});
    bool showing_reply = !reply_.empty() && static_cast<int32_t>(reply_until_ - now()) > 0;
    m.hero = !showing_reply;
    if (m.hero) {
      const bool switching = agent_profiles_.size() > 1;
      m.headline = switching && agent_profile_ != "default" ? tr(Msg::ProfileGreeting, {agent_profile_name()})
                                                            : std::string(tr(Msg::HiImHermes));
      m.detail = tr(reply_.empty() || !profile_.has_scroll_buttons ? Msg::AskMeAnything : Msg::UpShowsLastReply);
      const bool sessions = dashboard_ready_ && can_open_dashboard() && reply_.empty();
      if (sessions)
        m.detail = profile_.touch_screen ? std::string(tr(Msg::SwipeDownForSessions)) : tr(Msg::ButtonShowsSessions, {cancel});
      // A running timer's time left is the caption's line (after the profile hint, when there is one).
      const bool timing = timer_running_;
      if (timing) m.detail = tr(Msg::TimerLeft, {timer_left_text()});
      if (switching) {
        // In tap-to-talk mode a tap records, so the name is no target there.
        m.name_target = profile_.touch_screen && talk_mode_ == TalkMode::Hold;
        m.detail = tr(m.name_target ? Msg::ProfileTapName : Msg::ProfileInSettings) +
                   ((sessions || timing) ? "\n" + m.detail : "");
        m.caption_lines = 2;
      }
    }
    if (clock_showing_) {
      // The resting clock owns the whole panel: the time, the date and the mascot.
      m.screen = Screen::Clock;
      m.clock_time = clock_time_text(wall_, clock_24h_);
      m.clock_date = tr(Msg::ClockDate,
                        {tr(static_cast<Msg>(static_cast<int>(Msg::WeekdaySun) + std::min<unsigned>(wall_.weekday, 6))),
                         tr(static_cast<Msg>(static_cast<int>(Msg::MonthJan) + std::clamp<int>(wall_.month, 1, 12) - 1)),
                         std::to_string(wall_.day)});
      m.clock_minute = wall_.minute;
      m.hero = false;
      m.name_target = false;
      m.hint.clear();
      m.detail.clear();
      m.body.clear();
    }
  }
  bool keeps_detail = m.screen == Screen::Pairing || m.screen == Screen::Boot || m.screen == Screen::Prompt ||
                      m.screen == Screen::Profiles;
  if (!notice_.empty() && !keeps_detail) m.detail = notice_;
  if (m.screen == Screen::Ready && m.hero && power_status_ && power_status_->battery_percent &&
      *power_status_->battery_percent <= 10 && power_status_->external_power == false)
    m.detail = tr(Msg::LowBattery);
  if (!hint_flash_.empty()) m.hint = hint_flash_;
  if (!display_sleeping_) render(m);
}

// --------------------------------------------------------------------------
// Console

std::string App::status_json() const { return status_value().dump(); }

json::Value App::status_value() const {
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
      .set("profile", agent_profile_)
      .set("server", server_url_)
      .set("network", network_up_);
  s.set("display_sleeping", display_sleeping_);
  if (can_sleep_face_down()) s.set("face_down", face_down_);
  // The language the screen is in, and whether it follows Hermes's (`language` auto).
  s.set("language", i18n::info(lang_).code).set("language_auto", lang_auto_);
  if (hal_.power) s.set("power", power_value());
  if (!pairing_code_.empty()) s.set("pairing_code", pairing_code_);
  if (!prompt_id_.empty()) s.set("prompt", prompt_id_);
  if (timer_active()) s.set("timer", timer_value());
  if (on_phone_pair) s.set("phone", phone_value());
  if (!fatal_.empty()) s.set("error", fatal_);
  if (ota_ != Ota::Idle) {
    json::Value u = json::Value::object();
    u.set("state", ota_ == Ota::Offered ? "offered" : ota_ == Ota::Receiving ? "receiving" : "restarting")
        .set("version", ota_version_)
        .set("received", ota_received_)
        .set("size", ota_size_);
    s.set("update", u);
  }
  return s;
}

std::string App::diag_report() {
  json::Value r = json::Value::object();
  r.set("device_id", device_id_).set("board", profile_.board).set("firmware", profile_.firmware);
  json::Value conn = json::Value::object();
  conn.set("attempts", attempts_).set("network", network_detail_);
  if (!last_close_.empty()) conn.set("last_close", last_close_);
  if (online_since_) conn.set("online_s", (now() - online_since_) / 1000);
  r.set("connection", conn);
  r.set("app", status_value());
  if (frame_stats_) r.set("frames", frame_stats_->to_json());
  if (on_diag) on_diag(r);
  return "@diag " + r.dump();
}

bool App::known_setting(std::string_view key) const {
  for (const char* k : kSettingKeys) {
    if (key == k) return true;
  }
  for (const auto& k : profile_.extra_settings) {
    if (key == k) return true;
  }
  return false;
}

std::string App::console(std::string_view raw) {
  std::string line = trim(raw);
  if (line.empty()) return {};
  size_t sp = line.find(' ');
  std::string cmd = line.substr(0, sp);
  std::string rest = sp == std::string::npos ? std::string() : trim(std::string_view(line).substr(sp + 1));

  if (cmd == "help") {
    std::string keys;
    for (const char* k : kSettingKeys) keys += std::string(" ") + k;
    for (const auto& k : profile_.extra_settings) keys += " " + k;
    return "@help commands: status | diag [log] | get <key> | set <key> <value> | say <text> | talk | release | "
           "cancel | new-session | settings [close] | wifi-setup [close] | wifi [list | add <ssid> [password] | join <ssid> | "
           "forget <ssid>] | phone [pair [close] | forget] | perf [on | off | reset | budget <us> | bench [scroll | mascot | redraw | all]] | yes | no | reconnect | forget-key | factory-reset   keys:" + keys;
  }
  if (cmd == "status") return "@status " + status_json();
  if (cmd == "phone") {
    if (!on_phone_pair) return "@error Bluetooth is not enabled on this board";
    if (rest.empty()) return "@phone " + phone_value().dump();
    if (rest == "pair") return start_phone_pairing() ? "@ok phone pairing open" : "@error phone pairing unavailable";
    if (rest == "pair close") { close_phone_pairing(); return "@ok phone pairing closed"; }
    if (rest == "forget") {
      close_phone_pairing();
      if (on_phone_forget) on_phone_forget();
      on_phone_status(0, false);
      return "@ok phone forget";
    }
    return "@error usage: phone [pair [close] | forget]";
  }
  if (cmd == "wifi-setup") {
    if (rest == "close") { close_wifi_setup(); return "@ok Wi-Fi setup closed"; }
    return start_wifi_setup() ? "@ok " + wifi_setup_text_ : "@error Wi-Fi setup unavailable";
  }
  if (cmd == "wifi") return wifi_command(rest);
  if (cmd == "settings") {
    if (rest == "close") { close_settings(); return "@ok settings closed"; }
    return open_settings() ? "@ok settings" : "@error settings unavailable during a prompt or update";
  }
  if (cmd == "perf") return perf_command(rest);
  if (cmd == "diag" && rest.empty()) return diag_report();
  if (cmd == "diag" && rest == "log") {
    if (!recent_log) return "@error this device keeps no log";
    std::string out = recent_log();
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out + "@log end";
  }
  if (cmd == "get" || cmd == "set") {
    size_t ks = rest.find(' ');
    std::string key = rest.substr(0, ks);
    if (!known_setting(key)) return "@error unknown key";
    if (cmd == "get") {
      std::string v = setting(key);
      json::Value out = json::Value::object();
      out.set("key", key).set("value", is_secret(key) && !v.empty() ? std::string("<set>") : v);
      return "@value " + out.dump();
    }
    std::string value = ks == std::string::npos ? std::string() : trim(std::string_view(rest).substr(ks + 1));
    if (key == "screen_timeout" && !value.empty() &&
        (value.size() > 4 || value.find_first_not_of("0123456789") != std::string::npos || std::atoi(value.c_str()) > 3600))
      return "@error screen_timeout must be 0..3600 seconds";
    if (key == "battery_display" && !value.empty() && value != "icon" && value != "percentage" && value != "both" &&
        value != "off")
      return "@error battery_display must be icon, percentage, both or off";
    if (key == "battery_estimate" && !value.empty() && value != "off" && value != "liion")
      return "@error battery_estimate must be off or liion";
    if (key == "face_down_sleep" && !value.empty() && value != "on" && value != "off")
      return "@error face_down_sleep must be on or off";
    if (key == "clock" && !value.empty() &&
        (value.size() > 4 || value.find_first_not_of("0123456789") != std::string::npos ||
         (std::atoi(value.c_str()) != 0 && (std::atoi(value.c_str()) < 5 || std::atoi(value.c_str()) > 3600))))
      return "@error clock must be 0 (off) or 5..3600 seconds";
    if (key == "time_format" && !value.empty() && value != "12h" && value != "24h")
      return "@error time_format must be 12h or 24h";
    if (key == "language" && !value.empty() && value != "auto" && !i18n::parse(value))
      return "@error language must be one of:" + language_codes();
    if (!hal_.storage) return "@error no storage";
    if (value.empty()) hal_.storage->erase(setting_storage_key(key));
    else if (!hal_.storage->set(setting_storage_key(key), value)) return "@error could not save " + key;
    load_settings();
    if (key == "wifi_ssid" || key == "wifi_pass") {
      // These keys hold the first saved network: bring the list in line.
      WifiNetworks networks = WifiNetworks::load(*hal_.storage);
      if (networks.dirty()) networks.save(*hal_.storage);
    }
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
      for (const char* k : kSettingKeys) hal_.storage->erase(setting_storage_key(k));
      hal_.storage->erase("profile");
      for (const auto& k : profile_.extra_settings) hal_.storage->erase(k);
      hal_.storage->erase("wifi_nets");
      hal_.storage->erase("wifi_fast");  // where the last access point was (drivers/wifi_fast.hpp)
      if (on_phone_forget) on_phone_forget();
    }
    return "@ok " + cmd + " (restart the device to apply)";
  }
  return "@error unknown command (try: help)";
}

}  // namespace hg
