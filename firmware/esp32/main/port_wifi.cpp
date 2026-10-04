// Station changes run on the app task. IDF callbacks only post link events.
#include "port.hpp"

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

namespace hgp {
namespace {
uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
}

void Wifi::begin(NvsStorage& storage) {
  storage_ = &storage;
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
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
  ESP_ERROR_CHECK(esp_wifi_start());
}

void Wifi::join(const char* ssid, const char* password) {
  configured_ = false;
  retry_at_ = 0;
  esp_wifi_disconnect();
  const size_t ssid_size = std::strlen(ssid), pass_size = std::strlen(password);
  if (!ssid_size || ssid_size > 32 || pass_size > 64) {
    events::post(EventType::NetDown, "Wi-Fi not configured", 20);
    return;
  }
  wifi_config_t cfg = {};
  std::memcpy(cfg.sta.ssid, ssid, ssid_size);
  std::memcpy(cfg.sta.password, password, pass_size);
  cfg.sta.threshold.authmode = pass_size ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  cfg.sta.pmf_cfg.capable = true;
  if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return;
  configured_ = true;
  events::post(EventType::NetDown, "Joining Wi-Fi", 13);
  if (esp_wifi_connect() != ESP_OK) retry_at_ = now_ms() + 3000;
}

void Wifi::reconfigure() {
  joining_ = false;
  candidate_ = {};
  const std::string ssid = storage_->get("wifi_ssid").value_or(CONFIG_HG_DEFAULT_WIFI_SSID);
  const std::string pass = storage_->get("wifi_pass").value_or(CONFIG_HG_DEFAULT_WIFI_PASSWORD);
  auto_setup_ = ssid.empty() && !auto_setup_tried_;
  join(ssid.c_str(), pass.c_str());
}

void Wifi::disconnected() {
  wait_disconnect_ = false;
  if (configured_) retry_at_ = now_ms() + 3000;
}

void Wifi::connected(hg::App& app) {
  retry_at_ = 0;
  if (!joining_ || wait_disconnect_) return;
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
      std::strncmp(reinterpret_cast<const char*>(ap.ssid), candidate_.ssid, sizeof(ap.ssid)) != 0) return;
  storage_->set("wifi_ssid", candidate_.ssid);
  storage_->set("wifi_pass", candidate_.password);
  app.console(std::string("set server ") + candidate_.server);
  candidate_ = {};
  joining_ = false;
  setup_status("connected");
  close_at_ = now_ms() + 10000;
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
  if (configured_ && retry_at_ && static_cast<int32_t>(now - retry_at_) >= 0) {
    retry_at_ = 0;
    if (esp_wifi_connect() != ESP_OK) retry_at_ = now + 3000;
  }
}

void Wifi::on_event(void*, const char* base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    events::post(EventType::WifiStarted);
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
