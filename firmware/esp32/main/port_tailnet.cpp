// The device's own Tailscale node (TailnetLink in port.hpp, docs/tailnet.md): TailTiny
// (components/tailtiny) joins the tailnet once Wi-Fi is up, and the app connects to
// Hermes only after the WireGuard session with the Hermes host is up. TailTiny runs
// its own tasks; this file only polls it from the app task and never lets those
// tasks call the app. The decisions are in drivers/tailnet_watch.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_HG_TAILNET
#include "tailtiny.h"
// ml_wg_mgr_peer_is_up / _trigger_handshake and last_error. A C header: its inline
// helpers use designated initialisers that C++ warns about.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "tailtiny_internal.h"
#pragma GCC diagnostic pop
#endif

namespace hgp {
namespace {
const char* TAG = "hg.tailnet";
constexpr uint32_t kCheckMs = 1000;
constexpr uint32_t kRestartDelayMs = 1500;  // lets the console reply leave first
}  // namespace

bool TailnetLink::supported() {
#if CONFIG_HG_TAILNET
  return board_config().tailnet;
#else
  if (board_config().tailnet)
    ESP_LOGE(TAG, "this board enables the tailnet, but the build has no CONFIG_HG_TAILNET: add the tailnet "
                  "lines to its sdkconfig.defaults (docs/hardware-validation.md#tailnet)");
  return false;
#endif
}

#if !CONFIG_HG_TAILNET

void TailnetLink::begin(hg::App& app, const std::string& name) {
  app_ = &app;
  name_ = name;
}
void TailnetLink::network_up() {}
void TailnetLink::network_down() {}
void TailnetLink::settings_changed() {}
void TailnetLink::tick(uint32_t) {}
void TailnetLink::forget() {}
void TailnetLink::report(hg::json::Value&) {}
void TailnetLink::start() {}

#else

namespace {

// forget() leaves this flag; begin() erases TailTiny's keys at the next boot. Its own
// namespace, so the app's factory-reset, which asked for it, doesn't clear it.
constexpr char kForgetNamespace[] = "hg_tailnet";
constexpr char kForgetKey[] = "forget";

tailtiny_t* node(void* p) { return static_cast<tailtiny_t*>(p); }

// A tailnet hostname from the device name: "Nox" -> "nox", "Desk Gadget" -> "desk-gadget".
std::string hostname_for(const std::string& name) {
  std::string out;
  for (char c : name) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (ok) out += c;
    else if (!out.empty() && out.back() != '-') out += '-';
    if (out.size() >= 40) break;
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? std::string("hermes-gadget") : out;
}

std::string ip_text(uint32_t ip) {
  char buf[16];
  tailtiny_ip_to_str(ip, buf);
  return buf;
}

const char* state_name(tailtiny_state_t s) {
  switch (s) {
    case ML_STATE_IDLE: return "idle";
    case ML_STATE_WIFI_WAIT: return "waiting for Wi-Fi";
    case ML_STATE_CONNECTING: return "connecting";
    case ML_STATE_REGISTERING: return "registering";
    case ML_STATE_CONNECTED: return "connected";
    case ML_STATE_RECONNECTING: return "reconnecting";
    case ML_STATE_ERROR: return "error";
  }
  return "unknown";
}

}  // namespace

void TailnetLink::begin(hg::App& app, const std::string& name) {
  app_ = &app;
  name_ = name;
  // tailtiny_factory_reset() must run before tailtiny_init() (tailtiny.h): a running
  // node's tasks use the peer cache it frees, and could write their keys back.
  nvs_handle_t h;
  if (nvs_open(kForgetNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  uint8_t pending = 0;
  if (nvs_get_u8(h, kForgetKey, &pending) == ESP_OK && pending) {
    tailtiny_factory_reset();
    nvs_erase_key(h, kForgetKey);
    nvs_commit(h);
    ESP_LOGI(TAG, "forgot the tailnet identity, as factory-reset asked");
  }
  nvs_close(h);
}

void TailnetLink::start() {
  key_ = app_->tailnet_key();
  if (key_.empty()) {
    app_->on_tunnel(false, "Tailnet: no auth key (set ts_key)");
    return;
  }
  name_ = hostname_for(name_);
  tailtiny_config_t cfg = {};
  cfg.auth_key = key_.c_str();  // TailTiny keeps these pointers: key_ and name_ stay put
  cfg.device_name = name_.c_str();
  cfg.enable_derp = true;
  cfg.enable_stun = true;
  cfg.enable_disco = true;
  tailtiny_t* ml = tailtiny_init(&cfg);
  if (!ml) {
    ESP_LOGE(TAG, "TailTiny didn't start (out of memory?)");
    app_->on_tunnel(false, "Tailnet: could not start");
    return;
  }
  if (tailtiny_start(ml) != ESP_OK) {
    ESP_LOGE(TAG, "TailTiny's tasks didn't start");
    tailtiny_destroy(ml);
    app_->on_tunnel(false, "Tailnet: could not start");
    return;
  }
  node_ = ml;
  ESP_LOGI(TAG, "joining the tailnet as %s", name_.c_str());
  next_check_ = 0;
}

void TailnetLink::network_up() {
  wifi_up_ = true;
  if (!app_ || !app_->tailnet_wanted() || restart_) return;
  if (!node_) {
    start();
  } else {
    // Another network (or the same one again): new sockets, a new path to the peers.
    tailtiny_rebind(node(node_));
    next_check_ = 0;
  }
}

void TailnetLink::network_down() {
  wifi_up_ = false;
  next_check_ = 0;  // tell the app at the next tick
}

void TailnetLink::settings_changed() {
  if (!app_) return;
  const bool wanted = app_->tailnet_wanted();
  if (node_) {
    if (!wanted || app_->tailnet_key() != key_) {
      ESP_LOGW(TAG, "tailnet settings changed: restarting to apply them");
      restart_ = true;  // tick() sets the time
    }
    return;
  }
  if (wanted && wifi_up_) start();
  else if (wanted && app_->tailnet_key().empty()) app_->on_tunnel(false, "Tailnet: no auth key (set ts_key)");
}

void TailnetLink::tick(uint32_t now) {
  if (restart_) {
    if (!restart_timed_) {
      restart_timed_ = true;
      restart_at_ = now + kRestartDelayMs;
    } else if (static_cast<int32_t>(now - restart_at_) >= 0) {
      esp_restart();
    }
  }
  if (!node_ || static_cast<int32_t>(now - next_check_) < 0) return;
  next_check_ = now + kCheckMs;
  tailtiny_t* ml = node(node_);
  server_ip_ = hg::tailnet_ip(app_->server_url());

  hg::TailnetWatch::Sample s;
  const tailtiny_state_t state = tailtiny_get_state(ml);
  s.node = state == ML_STATE_CONNECTED ? hg::TailnetWatch::Node::Connected
           : state == ML_STATE_RECONNECTING || state == ML_STATE_ERROR ? hg::TailnetWatch::Node::Reconnecting
                                                                     : hg::TailnetWatch::Node::Starting;
  // Written by TailTiny's coord task: copy what is there (at worst a mix of two reasons).
  char error[sizeof(ml->last_error)];
  std::memcpy(error, ml->last_error, sizeof(error));
  error[sizeof(error) - 1] = '\0';
  s.error = error;
  s.peer_up = s.node == hg::TailnetWatch::Node::Connected && (!server_ip_ || ml_wg_mgr_peer_is_up(ml, server_ip_));

  const hg::TailnetWatch::Step step = watch_.update(now, wifi_up_, s);
  if (step.poke && server_ip_) {
    if (ml_wg_mgr_trigger_handshake(ml, server_ip_) == ESP_ERR_NOT_FOUND)
      ESP_LOGW(TAG, "%s isn't a peer on this tailnet (yet): is it online and allowed by the ACL?",
               ip_text(server_ip_).c_str());
    ml_wg_mgr_send_cmm(ml, server_ip_);
  }
  if (step.tell) {
    if (step.up) ESP_LOGI(TAG, "up: %s as %s", ip_text(tailtiny_get_vpn_ip(ml)).c_str(), name_.c_str());
    else ESP_LOGI(TAG, "down: %s", step.detail.c_str());
    app_->on_tunnel(step.up, step.detail);
  }
}

void TailnetLink::forget() {
  // Its node, WireGuard and DISCO keys and the cached peers go at the next boot, before
  // TailTiny starts (begin()); factory-reset asks for that restart. The next start
  // registers anew.
  nvs_handle_t h;
  esp_err_t err = nvs_open(kForgetNamespace, NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_set_u8(h, kForgetKey, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
  }
  if (err != ESP_OK) ESP_LOGE(TAG, "couldn't schedule forgetting the tailnet identity: %s", esp_err_to_name(err));
}

void TailnetLink::report(hg::json::Value& r) {
  hg::json::Value t = hg::json::Value::object();
  t.set("wanted", app_ && app_->tailnet_wanted());
  if (!node_) {
    t.set("state", "stopped");
    r.set("tailnet", t);
    return;
  }
  tailtiny_t* ml = node(node_);
  t.set("state", restart_ ? "restarting" : state_name(tailtiny_get_state(ml))).set("name", name_);
  if (const uint32_t ip = tailtiny_get_vpn_ip(ml)) t.set("ip", ip_text(ip));
  t.set("peers", tailtiny_get_peer_count(ml));
  if (ml->last_error[0]) t.set("error", std::string(ml->last_error, strnlen(ml->last_error, sizeof(ml->last_error))));
  if (server_ip_) {
    hg::json::Value h = hg::json::Value::object();
    h.set("ip", ip_text(server_ip_)).set("session", ml_wg_mgr_peer_is_up(ml, server_ip_));
    tailtiny_peer_info_t info = {};
    for (int i = 0; i < tailtiny_get_peer_count(ml); ++i) {
      if (tailtiny_get_peer_info(ml, i, &info) == ESP_OK && info.vpn_ip == server_ip_) {
        h.set("name", std::string(info.hostname, strnlen(info.hostname, sizeof(info.hostname))))
            .set("online", info.online)
            .set("path", info.direct_path ? "direct" : "relay");
        break;
      }
    }
    t.set("hermes", h);
  }
  r.set("tailnet", t);
}

#endif  // CONFIG_HG_TAILNET

}  // namespace hgp
