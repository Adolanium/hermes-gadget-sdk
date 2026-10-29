// Decisions the tailnet port (firmware/esp32/main/port_tailnet.cpp) makes about its
// TailTiny node, apart from ESP-IDF so they can be tested on the host: whether the
// Hermes address is a tailnet one, and from what TailTiny reports, when to tell the
// app the tunnel is up or down, with what words, and when to ask TailTiny for a
// fresh WireGuard handshake with the Hermes host.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace hg {

// The address in host byte order when `url` is ws://a.b.c.d[:port]/... with a.b.c.d in
// Tailscale's 100.64.0.0/10 range; 0 for a name or any other address.
inline uint32_t tailnet_ip(std::string_view url) {
  const size_t start = url.substr(0, 5) == "ws://" ? 5 : url.substr(0, 6) == "wss://" ? 6 : 0;
  if (!start) return 0;
  const std::string_view host = url.substr(start, url.find_first_of(":/?", start) - start);
  uint32_t ip = 0;
  int parts = 0;
  size_t i = 0;
  while (i < host.size() && parts < 4) {
    uint32_t v = 0;
    size_t digits = 0;
    while (i < host.size() && host[i] >= '0' && host[i] <= '9' && digits < 4) {
      v = v * 10 + static_cast<uint32_t>(host[i++] - '0');
      ++digits;
    }
    if (!digits || digits > 3 || v > 255) return 0;
    ip = (ip << 8) | v;
    ++parts;
    if (parts < 4) {
      if (i >= host.size() || host[i] != '.') return 0;
      ++i;
    }
  }
  if (parts != 4 || i != host.size()) return 0;
  return (ip & 0xFFC00000u) == 0x64400000u ? ip : 0;  // 100.64.0.0/10
}

class TailnetWatch {
 public:
  static constexpr uint32_t kPokeMs = 5000;        // a fresh handshake at most this often
  static constexpr uint32_t kDownGraceMs = 15000;  // a session that drops this long is down

  enum class Node : uint8_t { Starting, Connected, Reconnecting };

  // What TailTiny says now: its state, whether the WireGuard session with the Hermes
  // host is up (true when Hermes isn't at a tailnet address), and why registration
  // last failed ("" when it didn't).
  struct Sample {
    Node node = Node::Starting;
    bool peer_up = false;
    std::string_view error;
  };

  struct Step {
    bool tell = false;   // call App::on_tunnel(up, detail)
    bool up = false;
    std::string detail;
    bool poke = false;   // ask TailTiny for a handshake with the Hermes host
  };

  Step update(uint32_t now, bool wifi_up, const Sample& s) {
    Step out;
    bool up = false;
    std::string detail;
    if (!wifi_up) {
      detail = "Joining tailnet";
      gap_ = false;
    } else if (s.node != Node::Connected) {
      detail = !s.error.empty() ? "Tailnet: " + std::string(s.error)
               : s.node == Node::Reconnecting ? "Tailnet reconnecting" : "Joining tailnet";
      gap_ = false;
    } else if (s.peer_up) {
      up = true;
      gap_ = false;
    } else {
      if (!poked_ || static_cast<int32_t>(now - next_poke_) >= 0) {
        out.poke = true;
        poked_ = true;
        next_poke_ = now + kPokeMs;
      }
      if (told_up_) {
        // A rekey or a moment without a path: keep the session through a short gap.
        if (!gap_) {
          gap_ = true;
          gap_since_ = now;
        }
        up = now - gap_since_ < kDownGraceMs;
        if (!up) detail = "Tailnet: Hermes host unreachable";
      } else {
        detail = "Reaching Hermes over the tailnet";
      }
    }
    if (up) poked_ = false;
    if (!told_ || up != told_up_ || (!up && detail != told_detail_)) {
      out.tell = true;
      told_ = true;
      told_up_ = up;
      told_detail_ = up ? std::string() : detail;
    }
    out.up = up;
    out.detail = up ? std::string() : detail;
    return out;
  }

  bool told_up() const { return told_ && told_up_; }

 private:
  bool told_ = false, told_up_ = false, poked_ = false, gap_ = false;
  std::string told_detail_;
  uint32_t next_poke_ = 0, gap_since_ = 0;
};

}  // namespace hg
