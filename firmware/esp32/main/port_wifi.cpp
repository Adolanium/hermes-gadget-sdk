// Wi-Fi station with credentials from NVS (set over the serial console) or
// menuconfig defaults. Reconnects forever; link changes become app events.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdio>
#include <cstring>
#include <string>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

namespace hgp {
namespace {

const char* TAG = "hg.wifi";
TimerHandle_t retry_timer = nullptr;

void retry_cb(TimerHandle_t) { esp_wifi_connect(); }

}  // namespace

void Wifi::begin(NvsStorage& storage) {
  storage_ = &storage;
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Wifi::on_event, this));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &Wifi::on_event, this));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
  retry_timer = xTimerCreate("wifi-retry", pdMS_TO_TICKS(3000), pdFALSE, nullptr, retry_cb);
  ESP_ERROR_CHECK(esp_wifi_start());
}

void Wifi::reconfigure() {
  std::string ssid = storage_->get("wifi_ssid").value_or(CONFIG_HG_DEFAULT_WIFI_SSID);
  std::string pass = storage_->get("wifi_pass").value_or(CONFIG_HG_DEFAULT_WIFI_PASSWORD);
  if (ssid.empty()) {
    configured_ = false;
    ESP_LOGW(TAG, "no Wi-Fi configured; use the console: set wifi_ssid <name> / set wifi_pass <password>");
    events::post(EventType::NetDown, "Wi-Fi not configured", 20);
    return;
  }
  wifi_config_t cfg = {};
  std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(cfg.sta.password), pass.c_str(), sizeof(cfg.sta.password) - 1);
  cfg.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  cfg.sta.pmf_cfg.capable = true;
  configured_ = true;
  esp_wifi_disconnect();
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
  std::string detail = "Joining " + ssid;
  events::post(EventType::NetDown, detail.data(), detail.size());
  esp_wifi_connect();
}

void Wifi::on_event(void* arg, const char* base, int32_t id, void* data) {
  auto* self = static_cast<Wifi*>(arg);
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    self->reconfigure();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    auto* info = static_cast<wifi_event_sta_disconnected_t*>(data);
    char detail[48];
    std::snprintf(detail, sizeof(detail), "Wi-Fi lost (reason %d)", info ? info->reason : 0);
    events::post(EventType::NetDown, detail, std::strlen(detail));
    if (self->configured_) xTimerStart(retry_timer, 0);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* got = static_cast<ip_event_got_ip_t*>(data);
    char detail[32];
    std::snprintf(detail, sizeof(detail), IPSTR, IP2STR(&got->ip_info.ip));
    ESP_LOGI(TAG, "connected, ip %s", detail);
    events::post(EventType::NetUp, detail, std::strlen(detail));
  }
}

}  // namespace hgp
