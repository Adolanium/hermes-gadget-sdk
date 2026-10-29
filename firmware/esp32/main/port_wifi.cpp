// Station changes run on the app task. IDF callbacks only post link events.
#include "port.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "lwip/inet.h"

namespace hgp {
namespace {
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
esp_netif_t* station() { return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }
}

void Wifi::begin(NvsStorage& storage) {
  storage_ = &storage;
  fast_.load(storage.get(hg::WifiFastJoin::kStorageKey));
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  // Credentials remain in our NVS store until a phone setup succeeds.
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Wifi::on_event, this));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Wifi::on_event, this));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#if CONFIG_PM_ENABLE
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-wifi-join", &attempt_lock_) != ESP_OK) attempt_lock_ = nullptr;
#endif
  ESP_ERROR_CHECK(esp_wifi_start());
}

void Wifi::hold_while_attempting() {
  const bool want = retry_.attempting();
  if (!attempt_lock_ || want == attempt_held_) return;
  attempt_held_ = want;
  if (want) esp_pm_lock_acquire(attempt_lock_);
  else esp_pm_lock_release(attempt_lock_);
}

void Wifi::retry_now() {
  retry_.retry_now(now_ms());
}

bool Wifi::configure(const std::string& ssid, const std::string& password, const hg::WifiFastAp* aim) {
  wifi_config_t cfg = {};
  std::memcpy(cfg.sta.ssid, ssid.data(), ssid.size());
  std::memcpy(cfg.sta.password, password.data(), password.size());
  cfg.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  cfg.sta.pmf_cfg.capable = true;
  if (aim) {
    // The access point that last worked, on its channel: no scan first (wifi_fast.hpp).
    cfg.sta.bssid_set = true;
    std::memcpy(cfg.sta.bssid, aim->bssid, sizeof(cfg.sta.bssid));
    cfg.sta.channel = aim->channel;
  }
  if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
  ssid_ = ssid;
  password_ = password;
  aimed_ = aim != nullptr;
#if CONFIG_HG_WIFI_REUSE_IP
  // Opt-in: take the last lease as a static address and skip DHCP on a fast join.
  // Every other join, and the fallback after a failed fast one, uses DHCP.
  esp_netif_t* sta = station();
  const esp_err_t stopped = sta && aim && aim->has_lease() ? esp_netif_dhcpc_stop(sta) : ESP_FAIL;
  if (stopped == ESP_OK || stopped == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
    esp_netif_ip_info_t info = {};
    info.ip.addr = aim->ip;
    info.netmask.addr = aim->netmask;
    info.gw.addr = aim->gateway;
    if (esp_netif_set_ip_info(sta, &info) == ESP_OK) {
      static_ip_ = true;
      if (aim->dns) {
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = aim->dns;
        esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
      }
    } else {
      esp_netif_dhcpc_start(sta);
    }
  } else if (sta && static_ip_) {
    static_ip_ = false;
    esp_netif_dhcpc_start(sta);
  }
#endif
  return true;
}

void Wifi::remember_ap() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
  hg::WifiFastAp seen;
  seen.ssid = ssid_;
  std::memcpy(seen.bssid, ap.bssid, sizeof(seen.bssid));
  seen.channel = ap.primary;
  esp_netif_t* sta = station();
  esp_netif_ip_info_t ip = {};
  if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
    seen.ip = ip.ip.addr;
    seen.netmask = ip.netmask.addr;
    seen.gateway = ip.gw.addr;
  }
  esp_netif_dns_info_t dns = {};
  if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK && dns.ip.type == ESP_IPADDR_TYPE_V4)
    seen.dns = dns.ip.u_addr.ip4.addr;
  if (auto record = fast_.joined(seen)) storage_->set(hg::WifiFastJoin::kStorageKey, *record);
}

void Wifi::forget_unsaved_ap() {
  // The network was forgotten: so is where its access point was.
  const auto& cached = fast_.cached();
  if (!cached || cached->ssid == CONFIG_HG_DEFAULT_WIFI_SSID || networks_.find(cached->ssid) < networks_.size()) return;
  if (fast_.forget()) storage_->erase(hg::WifiFastJoin::kStorageKey);
}

void Wifi::join(const std::string& ssid, const std::string& password) {
  configured_ = false;
  if (scanning_) esp_wifi_scan_stop();
  scanning_ = false;
  retry_.connected();  // nothing pending until this join is under way
  hold_while_attempting();
  esp_wifi_disconnect();
  if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
    events::post(EventType::NetDown, "Wi-Fi not configured", 20);
    return;
  }
  if (!configure(ssid, password, fast_.aim(ssid))) return;
  configured_ = true;
  events::post(EventType::NetDown, "Joining Wi-Fi", 13);
  retry_.started(now_ms());
  if (esp_wifi_connect() != ESP_OK) retry_.start_failed(now_ms());
  hold_while_attempting();
}

void Wifi::reconfigure() {
  joining_ = false;
  candidate_ = {};
  networks_ = hg::WifiNetworks::load(*storage_);
  if (networks_.dirty()) networks_.save(*storage_);
  forget_unsaved_ap();
  pick_.reset(networks_.size());
  // With nothing saved, the network built into the firmware (if any).
  const std::string ssid = networks_.empty() ? CONFIG_HG_DEFAULT_WIFI_SSID : networks_.all()[0].ssid;
  const std::string pass = networks_.empty() ? CONFIG_HG_DEFAULT_WIFI_PASSWORD : networks_.all()[0].password;
  auto_setup_ = ssid.empty() && !auto_setup_tried_;
  join(ssid, pass);
}

void Wifi::networks_changed() {
  networks_ = hg::WifiNetworks::load(*storage_);
  if (networks_.dirty()) networks_.save(*storage_);
  forget_unsaved_ap();
  // On the device's network, the device has moved it first (connected()).
  const bool on_first = link_up_ && !networks_.empty() && networks_.all()[0].ssid == ssid_ &&
                        networks_.all()[0].password == password_;
  if (on_first) pick_.reset(networks_.size());
  else reconfigure();
}

void Wifi::disconnected() {
  wait_disconnect_ = false;
  link_up_ = false;
  if (configured_) {
    if (fast_.failed()) {
      // The fast join didn't work: join normally at once, without counting it against
      // the network or lengthening the battery backoff.
      retry_.lost(now_ms(), false);
      retry_.retry_now(now_ms());
    } else {
      pick_.failed();
      retry_.lost(now_ms(), on_battery && on_battery());
    }
  }
  hold_while_attempting();
}

void Wifi::connected(hg::App& app) {
  retry_.connected();
  pick_.connected();
  hold_while_attempting();
  link_up_ = true;
  remember_ap();
  if (!joining_ && networks_.joined(ssid_)) {
    networks_.save(*storage_);
    pick_.reset(networks_.size());
  }
  if (!joining_ || wait_disconnect_) return;
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
      std::strncmp(reinterpret_cast<const char*>(ap.ssid), candidate_.ssid, sizeof(ap.ssid)) != 0) return;
  // Set up from the phone page: saved first, alongside the others.
  networks_ = hg::WifiNetworks::load(*storage_);
  networks_.put_first(candidate_.ssid, candidate_.password, true);
  networks_.save(*storage_);
  pick_.reset(networks_.size());
  app.console(std::string("set server ") + candidate_.server);
  if (candidate_.tailnet_given) {
    // The key first: `tailnet on` starts the node with whatever key is saved.
    if (candidate_.ts_key[0]) app.console(std::string("set ts_key ") + candidate_.ts_key);
    app.console(std::string("set tailnet ") + (candidate_.tailnet_on ? "on" : "off"));
  }
  candidate_ = {};
  joining_ = false;
  setup_status("connected");
  close_at_ = now_ms() + 10000;
  esp_netif_ip_info_t station{};
  auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta && esp_netif_get_ip_info(sta, &station) == ESP_OK &&
      (station.ip.addr & station.netmask.addr) == (inet_addr("192.168.4.1") & station.netmask.addr))
    app.close_wifi_setup();
}

void Wifi::provision(const hg::WifiCredentials& credentials) {
  if (!http_ || joining_) return;
  candidate_ = credentials;
  wifi_ap_record_t ap = {};
  wait_disconnect_ = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  joining_ = true;
  trial_at_ = now_ms();
  setup_status("joining");
  join(candidate_.ssid, candidate_.password);
}

void Wifi::tick(hg::App& app, uint32_t now) {
  if (auto_setup_) {
    auto_setup_ = false;
    auto_setup_tried_ = true;
    app.start_wifi_setup();
  }
  if (joining_ && now - trial_at_ >= 30000) {
    reconfigure();
    setup_status("failed");
    std::lock_guard<std::mutex> lock(setup_mutex_);
    accepting_setup_ = true;
  }
  if (http_ && (static_cast<int32_t>(now - setup_until_) >= 0 ||
                (close_at_ && static_cast<int32_t>(now - close_at_) >= 0))) app.close_wifi_setup();
  if (configured_ && retry_.due(now)) attempt(now);
  hold_while_attempting();
  apply_power_save(now, app.in_turn());
}

void Wifi::attempt(uint32_t now) {
  scanning_ = false;
  // Not while a phone setup tries its network.
  if (!joining_ && pick_.should_scan()) {
    wifi_scan_config_t scan = {};
    if (esp_wifi_scan_start(&scan, false) == ESP_OK) {
      scanning_ = true;  // scan_done() makes the attempt
      return;
    }
  }
  // Fast while the last access point hasn't failed since the last join; otherwise drop the aim.
  const hg::WifiFastAp* aim = joining_ ? nullptr : fast_.aim(ssid_);
  if ((aim || aimed_) && !configure(ssid_, password_, aim)) {
    retry_.start_failed(now);
    return;
  }
  if (esp_wifi_connect() != ESP_OK) retry_.start_failed(now);
}

void Wifi::scan_done() {
  uint16_t found = 0;
  esp_wifi_scan_get_ap_num(&found);
  std::vector<wifi_ap_record_t> records(std::min<uint16_t>(found, 20));
  uint16_t count = static_cast<uint16_t>(records.size());
  if (count) esp_wifi_scan_get_ap_records(&count, records.data());  // frees the scan's list
  else esp_wifi_clear_ap_list();
  if (!scanning_ || !configured_ || joining_ || networks_.empty()) return;
  scanning_ = false;
  std::vector<std::string> saved;
  for (const auto& w : networks_.all()) saved.push_back(w.ssid);
  std::vector<hg::WifiPick::Seen> seen;
  for (uint16_t i = 0; i < count; ++i) {
    const char* name = reinterpret_cast<const char*>(records[i].ssid);
    seen.push_back({std::string_view(name, strnlen(name, sizeof(records[i].ssid))), records[i].rssi});
  }
  const hg::WifiNetwork& next = networks_.all()[pick_.choose(saved, seen)];
  ESP_LOGI("hg.wifi", "scan saw %u networks, trying %s", static_cast<unsigned>(found), next.ssid.c_str());
  if ((next.ssid != ssid_ || next.password != password_ || aimed_) && !configure(next.ssid, next.password)) {
    retry_.start_failed(now_ms());
    return;
  }
  if (esp_wifi_connect() != ESP_OK) retry_.start_failed(now_ms());
}

void Wifi::apply_power_save(uint32_t now, bool in_turn) {
  // Modem sleep only on battery and outside a turn (wifi_power.hpp); a board without a
  // battery reading counts as plugged in.
  const auto want = power_save_.change(now, [this] { return on_battery && on_battery(); }, in_turn);
  if (want && esp_wifi_set_ps(*want ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK) power_save_.applied(*want);
}

void Wifi::on_event(void*, const char* base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    events::post(EventType::WifiStarted);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    events::post(EventType::WifiScanDone);
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* info = static_cast<wifi_event_sta_disconnected_t*>(data);
    char detail[48];
    std::snprintf(detail, sizeof(detail), "Wi-Fi lost (reason %d)", info ? info->reason : 0);
    events::post(EventType::WifiDisconnected, detail, std::strlen(detail));
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* got = static_cast<ip_event_got_ip_t*>(data);
    char detail[32];
    std::snprintf(detail, sizeof(detail), IPSTR, IP2STR(&got->ip_info.ip));
    ESP_LOGI("hg.wifi", "connected, ip %s", detail);
    events::post(EventType::NetUp, detail, std::strlen(detail));
  }
}

}  // namespace hgp
