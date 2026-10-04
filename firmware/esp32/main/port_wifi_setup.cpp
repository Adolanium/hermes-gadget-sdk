#include "port.hpp"

#include <cstring>
#include <cstdio>

#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

namespace hgp {
namespace {

extern const uint8_t setup_page[] asm("_binary_wifi_setup_html_start");

std::string random_hex(size_t bytes) {
  uint8_t random[16];
  esp_fill_random(random, bytes);
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < bytes; ++i) { out += hex[random[i] >> 4]; out += hex[random[i] & 15]; }
  return out;
}

esp_err_t error_response(httpd_req_t* req, const char* status, const std::string& message) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  auto result = hg::json::Value::object();
  result.set("error", message);
  return httpd_resp_sendstr(req, result.dump().c_str());
}

bool local_request(httpd_req_t* req) {
  sockaddr_in local{}, peer{};
  socklen_t length = sizeof(local);
  esp_netif_ip_info_t info{};
  auto* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap || esp_netif_get_ip_info(ap, &info) != ESP_OK ||
      getsockname(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
      local.sin_family != AF_INET || local.sin_addr.s_addr != info.ip.addr) return false;
  length = sizeof(peer);
  if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr*>(&peer), &length) != 0 ||
      peer.sin_family != AF_INET || (peer.sin_addr.s_addr & info.netmask.addr) != (info.ip.addr & info.netmask.addr)) return false;
  char host[32], origin[64];
  if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
      (std::strcmp(host, "192.168.4.1") && std::strcmp(host, "192.168.4.1:80"))) return false;
  if (httpd_req_get_hdr_value_len(req, "Origin") &&
      (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
       (std::strcmp(origin, "http://192.168.4.1") && std::strcmp(origin, "http://192.168.4.1:80")))) return false;
  return true;
}

}  // namespace

void Wifi::setup_status(std::string status) {
  std::lock_guard<std::mutex> lock(setup_mutex_);
  setup_state_ = std::move(status);
}

std::string Wifi::start_setup() {
  if (!http_) {
    esp_netif_ip_info_t station{};
    auto* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &station) == ESP_OK && station.ip.addr &&
        (station.ip.addr & station.netmask.addr) == (inet_addr("192.168.4.1") & station.netmask.addr)) return {};
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) return {};
    char name[24];
    std::snprintf(name, sizeof(name), "Hermes-%02X%02X", mac[4], mac[5]);
    {
      std::lock_guard<std::mutex> lock(setup_mutex_);
      ap_name_ = name;
      ap_password_ = random_hex(8);
      nonce_ = random_hex(16);
      setup_state_ = "ready";
      accepting_setup_ = true;
    }
    wifi_config_t wifi = {};
    std::memcpy(wifi.ap.ssid, ap_name_.data(), ap_name_.size());
    wifi.ap.ssid_len = static_cast<uint8_t>(ap_name_.size());
    std::memcpy(wifi.ap.password, ap_password_.data(), ap_password_.size());
    wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.ap.max_connection = 2;
    wifi.ap.channel = 1;
    wifi.ap.pmf_cfg.capable = true;
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &wifi) != ESP_OK) {
      stop_setup(); return {};
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 3;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 3;
    config.lru_purge_enable = true;
    if (httpd_start(&http_, &config) != ESP_OK) { stop_setup(); return {}; }
    for (const auto& route : {std::pair<const char*, httpd_method_t>{"/", HTTP_GET},
                             {"/api/setup", HTTP_GET}, {"/api/setup", HTTP_POST}}) {
      httpd_uri_t uri = {};
      uri.uri = route.first;
      uri.method = route.second;
      uri.handler = &Wifi::setup_http;
      uri.user_ctx = this;
      if (httpd_register_uri_handler(http_, &uri) != ESP_OK) { stop_setup(); return {}; }
    }
    setup_until_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 600000;
    close_at_ = 0;
  }
  return "Network: " + ap_name_ + "\nPassword: " + ap_password_ +
         "\nOpen http://192.168.4.1\nAvailable for 10 minutes.";
}

void Wifi::stop_setup() {
  {
    std::lock_guard<std::mutex> lock(setup_mutex_);
    accepting_setup_ = false;
  }
  if (http_) { httpd_stop(http_); http_ = nullptr; }
  esp_wifi_set_mode(WIFI_MODE_STA);
  {
    std::lock_guard<std::mutex> lock(setup_mutex_);
    nonce_.clear();
    ap_password_.clear();
    setup_state_.clear();
  }
  close_at_ = 0;
  if (joining_) reconfigure();
}

esp_err_t Wifi::setup_http(httpd_req_t* req) {
  auto* self = static_cast<Wifi*>(req->user_ctx);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'");
  if (!local_request(req)) return error_response(req, "403 Forbidden", "Open setup from the device's temporary Wi-Fi network");
  if (req->method == HTTP_GET && std::strcmp(req->uri, "/") == 0) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, reinterpret_cast<const char*>(setup_page));
  }
  if (req->method == HTTP_GET) {
    auto result = hg::json::Value::object();
    {
      std::lock_guard<std::mutex> lock(self->setup_mutex_);
      result.set("nonce", self->nonce_).set("state", self->setup_state_);
    }
    result.set("ssid", self->storage_->get("wifi_ssid").value_or(CONFIG_HG_DEFAULT_WIFI_SSID));
    result.set("server", self->storage_->get("server").value_or(CONFIG_HG_DEFAULT_SERVER_URL));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, result.dump().c_str());
  }
  char type[64];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
      std::strncmp(type, "application/json", 16) != 0 || req->content_len == 0 || req->content_len > 1024)
    return error_response(req, "400 Bad Request", "Send a JSON setup request up to 1024 bytes");
  std::string body(req->content_len, '\0');
  size_t received = 0;
  const int64_t started = esp_timer_get_time();
  while (received < body.size()) {
    if (esp_timer_get_time() - started >= 3000000)
      return error_response(req, "408 Request Timeout", "Request timed out. Try again.");
    int size = httpd_req_recv(req, &body[received], body.size() - received);
    if (size <= 0) return error_response(req, "408 Request Timeout", "Request timed out. Try again.");
    received += static_cast<size_t>(size);
  }
  hg::WifiCredentials credentials;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(self->setup_mutex_);
    if (!self->accepting_setup_) return error_response(req, "409 Conflict", "Wait for the current connection attempt");
    if (!hg::parse_wifi_setup(body, self->nonce_, credentials, error)) return error_response(req, "400 Bad Request", error);
    if (!events::post(EventType::WifiProvision, &credentials, sizeof(credentials)))
      return error_response(req, "503 Service Unavailable", "Device busy. Try again.");
    self->accepting_setup_ = false;
    self->setup_state_ = "joining";
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"state\":\"joining\"}");
}

}  // namespace hgp
