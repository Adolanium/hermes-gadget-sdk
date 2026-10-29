#include "hg/setup.hpp"

#include <cstring>

#include "hg/json.hpp"
#include "hg/wifi_networks.hpp"

namespace hg {
namespace {

bool server_url(std::string_view url) {
  size_t start = url.substr(0, 5) == "ws://" ? 5 : url.substr(0, 6) == "wss://" ? 6 : 0;
  if (!start || url.size() > 200) return false;
  for (unsigned char c : url) if (c <= 0x20 || c >= 0x7f || c == '#' || c == '\\') return false;
  std::string_view authority = url.substr(start, url.find_first_of("/?", start) - start);
  if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
  size_t port = std::string_view::npos;
  if (authority.front() == '[') {
    size_t end = authority.find(']');
    if (end == std::string_view::npos || end <= 2) return false;
    for (char c : authority.substr(1, end - 1))
      if (std::string_view("0123456789abcdefABCDEF:.").find(c) == std::string_view::npos) return false;
    if (end + 1 < authority.size()) {
      if (authority[end + 1] != ':') return false;
      port = end + 2;
    }
  } else {
    size_t colon = authority.find(':');
    auto host = authority.substr(0, colon);
    if (host.empty()) return false;
    for (char c : host)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    if (colon != std::string_view::npos) port = colon + 1;
  }
  if (port != std::string_view::npos) {
    auto digits = authority.substr(port);
    if (digits.empty() || digits.size() > 5) return false;
    unsigned value = 0;
    for (char c : digits) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (!value || value > 65535) return false;
  }
  return true;
}

}  // namespace

bool valid_tailnet_key(std::string_view key) {
  if (key.size() < 12 || key.size() > 120 || key.substr(0, 6) != "tskey-") return false;
  for (unsigned char c : key)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  return true;
}

bool parse_wifi_setup(std::string_view body, std::string_view nonce, WifiCredentials& out, std::string& error) {
  json::Value form;
  if (body.size() > 1024 || !json::parse(body, form, nullptr, 2) || !form.is_object()) {
    error = "Invalid setup request"; return false;
  }
  if (nonce.empty() || form["nonce"].as_string() != nonce) {
    error = "Setup session expired. Reload this page."; return false;
  }
  if (!form["ssid"].is_string() || !form["password"].is_string() || !form["server"].is_string()) {
    error = "Enter the network name, password and gateway address"; return false;
  }
  const auto& ssid = form["ssid"].as_string();
  const auto& password = form["password"].as_string();
  const auto& server = form["server"].as_string();
  error = wifi_network_error(ssid, password);
  if (!error.empty()) return false;
  if (!server_url(server)) { error = "Enter a ws:// or wss:// gateway address with a host"; return false; }
  // Optional: the page sends these only on a board that can join a tailnet.
  const json::Value& tailnet = form["tailnet"];
  const json::Value& ts_key = form["ts_key"];
  if ((!tailnet.is_null() && !tailnet.is_bool()) || (!ts_key.is_null() && !ts_key.is_string())) {
    error = "Invalid setup request"; return false;
  }
  const std::string key = ts_key.is_string() ? ts_key.as_string() : std::string();
  if (!key.empty() && !valid_tailnet_key(key)) {
    error = "Enter a Tailscale auth key (tskey-...), or leave it empty"; return false;
  }
  WifiCredentials valid;
  std::memcpy(valid.ssid, ssid.data(), ssid.size());
  std::memcpy(valid.password, password.data(), password.size());
  std::memcpy(valid.server, server.data(), server.size());
  valid.tailnet_given = tailnet.is_bool();
  valid.tailnet_on = tailnet.is_bool() && tailnet.as_bool();
  std::memcpy(valid.ts_key, key.data(), key.size());
  out = valid;
  error.clear();
  return true;
}

}  // namespace hg
