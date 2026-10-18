// Rejoining the access point that last worked without a scan, apart from ESP-IDF
// so it can be tested on the host (firmware/esp32/main/port_wifi.cpp).
//
// A join that names no access point scans every channel before it associates.
// After each successful join the port records the access point's BSSID and
// channel, with the DHCP lease it got, in NVS ("wifi_fast"). The next attempt on
// the same network (at boot, or after the link drops) aims at that BSSID on that
// channel, which skips the scan. If that one attempt fails, the attempts after it
// join normally until the device is connected again, and the next good join
// replaces the record. The record is written only when it changes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace hg {

struct WifiFastAp {
  std::string ssid;
  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  // The DHCP lease that came with it, IPv4 as lwIP stores it; 0 when unknown.
  uint32_t ip = 0, netmask = 0, gateway = 0, dns = 0;

  bool has_lease() const { return ip && netmask && gateway; }
  bool operator==(const WifiFastAp& o) const {
    for (size_t i = 0; i < sizeof(bssid); ++i)
      if (bssid[i] != o.bssid[i]) return false;
    return ssid == o.ssid && channel == o.channel && ip == o.ip && netmask == o.netmask &&
           gateway == o.gateway && dns == o.dns;
  }
  bool operator!=(const WifiFastAp& o) const { return !(*this == o); }

  // "1 <ssid as hex> <bssid as hex> <channel> <ip> <netmask> <gateway> <dns>", numbers in hex.
  std::string encode() const {
    std::string out = "1 ";
    char b[16];
    for (unsigned char c : ssid) {
      std::snprintf(b, sizeof(b), "%02x", c);
      out += b;
    }
    out += ' ';
    for (uint8_t c : bssid) {
      std::snprintf(b, sizeof(b), "%02x", c);
      out += b;
    }
    for (uint32_t v : {uint32_t(channel), ip, netmask, gateway, dns}) {
      std::snprintf(b, sizeof(b), " %x", static_cast<unsigned>(v));
      out += b;
    }
    return out;
  }

  // Nothing for a record that is damaged, from another format, or names no usable access point.
  static std::optional<WifiFastAp> parse(std::string_view s) {
    auto word = [&s]() {
      while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
      size_t n = s.find(' ');
      std::string_view w = s.substr(0, n);
      s.remove_prefix(n == std::string_view::npos ? s.size() : n);
      return w;
    };
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    auto hex_bytes = [&nibble](std::string_view w, std::string& out) {
      if (w.size() % 2) return false;
      for (size_t i = 0; i < w.size(); i += 2) {
        int hi = nibble(w[i]), lo = nibble(w[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi << 4 | lo);
      }
      return true;
    };
    auto hex_u32 = [&nibble](std::string_view w, uint32_t& out) {
      if (w.empty() || w.size() > 8) return false;
      out = 0;
      for (char c : w) {
        int v = nibble(c);
        if (v < 0) return false;
        out = out << 4 | static_cast<uint32_t>(v);
      }
      return true;
    };
    if (word() != "1") return std::nullopt;
    WifiFastAp ap;
    std::string bssid;
    if (!hex_bytes(word(), ap.ssid) || ap.ssid.empty() || ap.ssid.size() > 32) return std::nullopt;
    if (!hex_bytes(word(), bssid) || bssid.size() != sizeof(ap.bssid)) return std::nullopt;
    for (size_t i = 0; i < sizeof(ap.bssid); ++i) ap.bssid[i] = static_cast<uint8_t>(bssid[i]);
    uint32_t channel = 0;
    if (!hex_u32(word(), channel) || channel < 1 || channel > 14) return std::nullopt;
    ap.channel = static_cast<uint8_t>(channel);
    for (uint32_t* v : {&ap.ip, &ap.netmask, &ap.gateway, &ap.dns})
      if (!hex_u32(word(), *v)) return std::nullopt;
    if (!word().empty()) return std::nullopt;
    return ap;
  }
};

class WifiFastJoin {
 public:
  static constexpr const char* kStorageKey = "wifi_fast";

  // The stored record, at boot. A damaged one is ignored (and replaced by the next join).
  void load(const std::optional<std::string>& stored) {
    cached_ = stored ? WifiFastAp::parse(*stored) : std::nullopt;
    trying_ = spent_ = false;
  }
  // Before an attempt on `ssid`: the access point to aim at, or nullptr to join
  // normally. Aims at most once until the next successful join.
  const WifiFastAp* aim(std::string_view ssid) {
    if (trying_) spent_ = true;  // the aimed attempt never reported back (WifiRetry's timeout)
    trying_ = cached_ && !spent_ && cached_->ssid == ssid;
    return trying_ ? &*cached_ : nullptr;
  }
  // The attempt failed. True when it was the aimed one: the caller then joins
  // normally, and need not count it against the network.
  bool failed() {
    const bool was = trying_;
    if (trying_) spent_ = true;
    trying_ = false;
    return was;
  }
  // Joined `ap`. The record to store, or nothing when it hasn't changed.
  std::optional<std::string> joined(const WifiFastAp& ap) {
    trying_ = spent_ = false;
    if (ap.ssid.empty() || ap.channel < 1 || ap.channel > 14 || (cached_ && *cached_ == ap)) return std::nullopt;
    cached_ = ap;
    return ap.encode();
  }
  // The network was forgotten. True when there was a record to erase.
  bool forget() {
    trying_ = spent_ = false;
    const bool had = cached_.has_value();
    cached_.reset();
    return had;
  }
  bool trying() const { return trying_; }
  const std::optional<WifiFastAp>& cached() const { return cached_; }

 private:
  std::optional<WifiFastAp> cached_;
  bool trying_ = false, spent_ = false;
};

}  // namespace hg
