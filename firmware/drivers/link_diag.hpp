// What a lost link says about itself, in the words the log, the connecting screen and
// the console's `diag` report use. Apart from ESP-IDF so it can be tested on the host
// (firmware/esp32/main/port_wifi.cpp and port_ws.cpp fill in the numbers).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hg::link {

// ESP-IDF's wifi_err_reason_t, the ones a station sees in practice. Others print as
// their number alone.
inline const char* wifi_reason_name(int reason) {
  switch (reason) {
    case 1: return "unspecified";
    case 2: return "auth expired";
    case 3: return "auth leave";
    case 4: return "inactivity";
    case 5: return "AP full";
    case 6: return "not authenticated";
    case 7: return "not associated";
    case 8: return "assoc leave";
    case 15: return "4-way handshake timeout";
    case 200: return "beacon timeout";
    case 201: return "no AP found";
    case 202: return "auth failed";
    case 203: return "assoc failed";
    case 204: return "handshake timeout";
    case 205: return "connection failed";
    case 206: return "AP TSF reset";
    case 207: return "roaming";
    default: return nullptr;
  }
}

// "Wi-Fi lost (reason 200 beacon timeout, rssi -78)". `rssi` 0: unknown, left out.
inline std::string wifi_lost(int reason, int rssi) {
  char buf[80];
  const char* name = wifi_reason_name(reason);
  int n = std::snprintf(buf, sizeof(buf), "Wi-Fi lost (reason %d%s%s", reason, name ? " " : "", name ? name : "");
  if (rssi && n > 0 && static_cast<size_t>(n) < sizeof(buf))
    n += std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), ", rssi %d", rssi);
  if (n > 0 && static_cast<size_t>(n) < sizeof(buf)) std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), ")");
  return buf;
}

// A send in flight when the connection was lost: a send that runs out of time closes the
// connection before it returns, so this is usually the send that gave up.
struct SendFailure {
  uint32_t took_ms = 0;  // how long it had been sending
  size_t bytes = 0;      // the message's size
};

// What esp_websocket_client reports with a disconnect or an error.
struct WsError {
  int error_type = 0;   // esp_websocket_error_type_t: 1 transport, 2 pong timeout, 3 handshake, 4 server close
  int sock_errno = 0;   // the socket's errno
  int tls_err = 0;      // esp-tls's last esp_err_t, 0 if none
  int http_status = 0;  // the upgrade's HTTP status, for a refused handshake
  int close_code = 0;   // the server's RFC 6455 close code, 0 if none
};

// The upgrade's HTTP status worth reporting: esp_websocket_client stores it when a connect
// attempt fails (and reports that failure as a transport error, not a handshake one) and never
// clears it. So it explains only the event that ends a connect attempt, never a later drop of a
// connection that was up, and only when it is set and isn't 101 (switching protocols).
inline int upgrade_status(int stored, bool ends_connect_attempt) {
  return ends_connect_attempt && stored > 0 && stored != 101 ? stored : 0;
}

inline const char* ws_error_type_name(int type) {
  switch (type) {
    case 1: return "transport";
    case 2: return "pong timeout";
    case 3: return "handshake";
    case 4: return "server close";
    default: return nullptr;
  }
}

// The close reason the app logs and shows, with whatever is known:
// "disconnected: a 1286 B send in flight for 10000 ms; transport errno 11; rssi -74".
// `what` is the event ("disconnected", "connection error"); `send` is null when no send was in flight.
inline std::string ws_closed(const char* what, const WsError& e, const SendFailure* send, int rssi) {
  std::string out = what ? what : "closed";
  char buf[96];
  const char* sep = ": ";
  if (send) {
    std::snprintf(buf, sizeof(buf), "%sa %u B send in flight for %u ms", sep, static_cast<unsigned>(send->bytes),
                  static_cast<unsigned>(send->took_ms));
    out += buf;
    sep = "; ";
  }
  if (const char* type = ws_error_type_name(e.error_type)) {
    out += sep;
    out += type;
    sep = "; ";
    if (e.sock_errno) {
      std::snprintf(buf, sizeof(buf), " errno %d", e.sock_errno);
      out += buf;
    }
    if (e.tls_err) {
      std::snprintf(buf, sizeof(buf), " tls 0x%x", static_cast<unsigned>(e.tls_err));
      out += buf;
    }
    if (e.http_status) {
      std::snprintf(buf, sizeof(buf), " http %d", e.http_status);
      out += buf;
    }
  }
  if (e.close_code) {
    std::snprintf(buf, sizeof(buf), "%sclose code %d", sep, e.close_code);
    out += buf;
    sep = "; ";
  }
  if (rssi) {
    std::snprintf(buf, sizeof(buf), "%srssi %d", sep, rssi);
    out += buf;
  }
  return out;
}

}  // namespace hg::link
