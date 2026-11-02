// What a lost link says about itself (firmware/drivers/link_diag.hpp).
#include <string>

#include "check.hpp"
#include "link_diag.hpp"

TEST("link diag: a lost Wi-Fi names its reason and the signal it went at") {
  CHECK_EQ(hg::link::wifi_lost(200, -78), std::string("Wi-Fi lost (reason 200 beacon timeout, rssi -78)"));
  CHECK_EQ(hg::link::wifi_lost(8, -55), std::string("Wi-Fi lost (reason 8 assoc leave, rssi -55)"));
  CHECK_EQ(hg::link::wifi_lost(99, 0), std::string("Wi-Fi lost (reason 99)"));  // unknown reason, no signal
  CHECK_EQ(hg::link::wifi_lost(0, 0), std::string("Wi-Fi lost (reason 0)"));
}

TEST("link diag: a close during a send says how long it had been sending, and the signal") {
  hg::link::WsError e;
  e.error_type = 1;
  e.sock_errno = 11;
  hg::link::SendFailure s;
  s.took_ms = 10000;
  s.bytes = 1286;
  CHECK_EQ(hg::link::ws_closed("disconnected", e, &s, -74),
           std::string("disconnected: a 1286 B send in flight for 10000 ms; transport errno 11; rssi -74"));
}

TEST("link diag: a close names the client's error, a refused upgrade and the server's close code") {
  hg::link::WsError reset;
  reset.error_type = 1;
  reset.sock_errno = 104;
  CHECK_EQ(hg::link::ws_closed("connection error", reset, nullptr, -60),
           std::string("connection error: transport errno 104; rssi -60"));
  hg::link::WsError refused;
  refused.error_type = 3;
  refused.http_status = 403;
  CHECK_EQ(hg::link::ws_closed("connection error", refused, nullptr, 0), std::string("connection error: handshake http 403"));
  hg::link::WsError closed;
  closed.error_type = 4;
  closed.close_code = 1008;
  CHECK_EQ(hg::link::ws_closed("disconnected", closed, nullptr, -50),
           std::string("disconnected: server close; close code 1008; rssi -50"));
  CHECK_EQ(hg::link::ws_closed("disconnected", hg::link::WsError{}, nullptr, 0), std::string("disconnected"));
}

TEST("link diag: the upgrade status explains only a failed connect attempt") {
  CHECK_EQ(hg::link::upgrade_status(401, true), 401);   // a proxy refused the upgrade
  CHECK_EQ(hg::link::upgrade_status(404, true), 404);
  CHECK_EQ(hg::link::upgrade_status(101, true), 0);     // switching protocols: not a refusal
  CHECK_EQ(hg::link::upgrade_status(0, true), 0);       // the attempt failed before any reply
  CHECK_EQ(hg::link::upgrade_status(401, false), 0);    // a stale value on a later drop
}

TEST("link diag: a refused upgrade keeps its HTTP status though the client reports a transport error") {
  // esp_websocket_client 1.8.0 aborts a refused upgrade as WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT.
  hg::link::WsError e;
  e.error_type = 1;
  e.http_status = 401;
  CHECK_EQ(hg::link::ws_closed("connection error", e, nullptr, -58),
           std::string("connection error: transport http 401; rssi -58"));
}
