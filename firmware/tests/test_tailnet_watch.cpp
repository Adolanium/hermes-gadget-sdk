#include <cstdint>
#include <string>

#include "check.hpp"
#include "tailnet_watch.hpp"

using hg::TailnetWatch;
using Node = TailnetWatch::Node;

TEST("tailnet address: only a 100.64.0.0/10 literal is a tailnet address") {
  CHECK_EQ(hg::tailnet_ip("ws://100.71.189.23:8765/gadget"), uint32_t(0x6447BD17));
  CHECK_EQ(hg::tailnet_ip("ws://100.64.0.1/gadget"), uint32_t(0x64400001));
  CHECK_EQ(hg::tailnet_ip("wss://100.127.255.254"), uint32_t(0x647FFFFE));
  CHECK_EQ(hg::tailnet_ip("ws://100.128.0.1:8765/gadget"), uint32_t(0));  // just past the range
  CHECK_EQ(hg::tailnet_ip("ws://100.63.255.255:8765/gadget"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("ws://192.168.1.20:8765/gadget"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("ws://dev02:8765/gadget"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("ws://100.71.189:8765/gadget"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("ws://100.71.189.23.5:8765/gadget"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("ws://100.71.1890.23/"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("ws://100.71.189.256/"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip("http://100.71.189.23/"), uint32_t(0));
  CHECK_EQ(hg::tailnet_ip(""), uint32_t(0));
}

TEST("tailnet watch: says what it is waiting for, then up once the Hermes host answers") {
  TailnetWatch w;
  auto s = w.update(0, true, {Node::Starting, false, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Joining tailnet"));
  CHECK(!w.update(100, true, {Node::Starting, false, ""}).tell);  // nothing new to say

  s = w.update(200, true, {Node::Connected, false, ""});
  CHECK(s.tell && !s.up && s.poke);
  CHECK_EQ(s.detail, std::string("Reaching Hermes over the tailnet"));
  s = w.update(1200, true, {Node::Connected, false, ""});
  CHECK(!s.tell && !s.poke);  // the next handshake waits kPokeMs
  s = w.update(200 + TailnetWatch::kPokeMs, true, {Node::Connected, false, ""});
  CHECK(s.poke);

  s = w.update(6000, true, {Node::Connected, true, ""});
  CHECK(s.tell && s.up && !s.poke);
  CHECK(w.told_up());
  CHECK(!w.update(7000, true, {Node::Connected, true, ""}).tell);
}

TEST("tailnet watch: a failed registration is shown with TailTiny's reason") {
  TailnetWatch w;
  w.update(0, true, {Node::Starting, false, ""});
  auto s = w.update(100, true, {Node::Starting, false, "auth key rejected"});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet: auth key rejected"));
}

TEST("tailnet watch: a short gap keeps the session, a long one ends it") {
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  CHECK(w.told_up());
  auto s = w.update(1000, true, {Node::Connected, false, ""});  // a rekey
  CHECK(!s.tell && s.up && s.poke);
  s = w.update(1000 + TailnetWatch::kDownGraceMs - 10, true, {Node::Connected, false, ""});
  CHECK(!s.tell && s.up);
  s = w.update(1000 + TailnetWatch::kDownGraceMs, true, {Node::Connected, false, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet: Hermes host unreachable"));

  TailnetWatch v;
  v.update(0, true, {Node::Connected, true, ""});
  v.update(1000, true, {Node::Connected, false, ""});
  CHECK(v.update(2000, true, {Node::Connected, true, ""}).up);  // back: the grace starts over
  CHECK(v.update(2000 + TailnetWatch::kDownGraceMs, true, {Node::Connected, false, ""}).up);
}

TEST("tailnet watch: during the grace, handshakes are still asked for at most every kPokeMs") {
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  int pokes = 0;
  for (uint32_t t = 1000; t < 1000 + TailnetWatch::kDownGraceMs; t += 1000) {  // the port polls each second
    auto s = w.update(t, true, {Node::Connected, false, ""});
    CHECK(s.up);
    if (s.poke) ++pokes;
  }
  CHECK_EQ(pokes, int(TailnetWatch::kDownGraceMs / TailnetWatch::kPokeMs));  // at 1, 6 and 11 s
  CHECK(w.update(20000, true, {Node::Connected, true, ""}).up);
  CHECK(w.update(21000, true, {Node::Connected, false, ""}).poke);  // a new gap pokes at once
}

TEST("tailnet watch: replacing the control server's connection keeps the session while WireGuard is up") {
  // Tailscale ends the control long-poll from time to time; TailTiny reconnects while the
  // WireGuard session with the Hermes host carries on.
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  for (Node n : {Node::Reconnecting, Node::Starting}) {  // RECONNECTING, then CONNECTING/REGISTERING
    auto s = w.update(1000, true, {n, true, ""});
    CHECK(!s.tell && s.up && !s.poke);
  }
  auto s = w.update(60000, true, {Node::Reconnecting, true, ""});  // however long it takes
  CHECK(!s.tell && s.up);
  CHECK(!w.update(61000, true, {Node::Connected, true, ""}).tell);
  CHECK(w.told_up());
}

TEST("tailnet watch: a control reconnect with WireGuard down too gets the grace, then ends it") {
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  auto s = w.update(1000, true, {Node::Reconnecting, false, ""});
  CHECK(!s.tell && s.up && !s.poke);  // no handshake asked for while the control server is away
  s = w.update(1000 + TailnetWatch::kDownGraceMs - 10, true, {Node::Starting, false, ""});
  CHECK(!s.tell && s.up);
  s = w.update(1000 + TailnetWatch::kDownGraceMs, true, {Node::Reconnecting, false, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet reconnecting"));
  CHECK(!w.update(20000, true, {Node::Reconnecting, false, ""}).tell);  // still saying the same

  TailnetWatch never;  // never up: a reconnect is shown at once, as before
  s = never.update(0, true, {Node::Reconnecting, true, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet reconnecting"));
}

TEST("tailnet watch: at boot, a session with the Hermes host is up before TailTiny reports Connected") {
  // TailTiny reports Connected once its DERP relay connects too (region choice and TLS, about
  // 5 s on an AMOLED-1.75C), while a direct WireGuard session to the Hermes host is up seconds
  // earlier. The connection to Hermes only needs the session.
  TailnetWatch w;
  auto s = w.update(0, true, {Node::Starting, false, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Joining tailnet"));
  s = w.update(1000, true, {Node::Starting, true, "", true});
  CHECK(s.tell && s.up && !s.poke);
  CHECK(w.told_up());
  CHECK(!w.update(5000, true, {Node::Connected, true, "", true}).tell);  // nothing new once Connected

  TailnetWatch reconnecting;  // the same before the first Connected, whatever the state
  CHECK(reconnecting.update(0, true, {Node::Reconnecting, true, "", true}).up);

  TailnetWatch refused;  // a registration error still wins over a session
  s = refused.update(0, true, {Node::Starting, true, "auth key rejected", true});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet: auth key rejected"));
}

TEST("tailnet watch: a Hermes host off the tailnet still waits for Connected") {
  // peer_up is true for a server that isn't at a tailnet address; that is no session.
  TailnetWatch w;
  auto s = w.update(0, true, {Node::Starting, true, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Joining tailnet"));
  CHECK(w.update(1000, true, {Node::Connected, true, ""}).up);
}

TEST("tailnet watch: after a network change, a kept session waits for Connected again") {
  // tailtiny_rebind() keeps WireGuard's keys: a session from the old network says nothing
  // about a path on the new one.
  TailnetWatch w;
  CHECK(w.update(0, true, {Node::Connected, true, "", true}).up);
  CHECK(!w.update(1000, false, {Node::Connected, true, "", true}).up);  // Wi-Fi lost
  w.network_changed();  // back on Wi-Fi: the port rebinds
  auto s = w.update(2000, true, {Node::Reconnecting, true, "", true});  // old keys
  CHECK(!s.up);
  CHECK_EQ(s.detail, std::string("Tailnet reconnecting"));
  CHECK(!w.update(3000, true, {Node::Starting, true, "", true}).up);
  CHECK(w.update(4000, true, {Node::Connected, true, "", true}).up);

  // A session made between two polls, then a network change, before it was ever reported.
  TailnetWatch unseen;
  unseen.update(0, true, {Node::Starting, false, ""});
  unseen.network_changed();
  CHECK(!unseen.update(1000, true, {Node::Reconnecting, true, "", true}).up);
  CHECK(unseen.update(2000, true, {Node::Connected, true, "", true}).up);
}

TEST("tailnet watch: a network change the watch didn't see go down still ends the tunnel") {
  // Wi-Fi lost and back between two polls: the watch never saw it down, and WireGuard kept
  // its keys through the rebind.
  TailnetWatch w;
  CHECK(w.update(0, true, {Node::Connected, true, "", true}).up);
  w.network_changed();
  auto s = w.update(1000, true, {Node::Reconnecting, true, "", true});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet reconnecting"));
  CHECK(!w.update(2000, true, {Node::Starting, true, "", true}).up);
  s = w.update(3000, true, {Node::Connected, true, "", true});
  CHECK(s.tell && s.up);
}

TEST("tailnet watch: on the same network, a fresh session after the grace is up again") {
  // Up early, then WireGuard down past the grace while the control server reconnects: a
  // new session on the same network is up again without waiting for Connected.
  TailnetWatch w;
  CHECK(w.update(0, true, {Node::Starting, true, "", true}).up);
  w.update(1000, true, {Node::Reconnecting, false, "", false});
  CHECK(!w.update(1000 + TailnetWatch::kDownGraceMs, true, {Node::Reconnecting, false, "", false}).up);
  auto s = w.update(20000, true, {Node::Reconnecting, true, "", true});
  CHECK(s.tell && s.up);
}

TEST("tailnet watch: up early, a session lost before Connected gets the grace without a handshake") {
  TailnetWatch w;
  CHECK(w.update(0, true, {Node::Starting, true, "", true}).up);
  auto s = w.update(1000, true, {Node::Starting, false, "", false});
  CHECK(!s.tell && s.up && !s.poke);  // no handshake asked for before Connected
  s = w.update(1000 + TailnetWatch::kDownGraceMs - 10, true, {Node::Starting, false, "", false});
  CHECK(!s.tell && s.up && !s.poke);
  s = w.update(1000 + TailnetWatch::kDownGraceMs, true, {Node::Starting, false, "", false});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet reconnecting"));

  TailnetWatch back;  // the session back within the grace: up, and the grace starts over
  back.update(0, true, {Node::Starting, true, "", true});
  back.update(1000, true, {Node::Starting, false, "", false});
  CHECK(back.update(2000, true, {Node::Starting, true, "", true}).up);
  CHECK(back.update(2000 + TailnetWatch::kDownGraceMs - 10, true, {Node::Starting, false, "", false}).up);

  TailnetWatch refused;  // an error ends an early session at once
  refused.update(0, true, {Node::Starting, true, "", true});
  s = refused.update(1000, true, {Node::Starting, true, "auth key rejected", true});
  CHECK(s.tell && !s.up);
}

TEST("tailnet watch: a registration refused during a reconnect ends the session at once") {
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  auto s = w.update(1000, true, {Node::Reconnecting, true, "auth key rejected"});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet: auth key rejected"));
}

TEST("tailnet watch: no Wi-Fi means no tunnel, at once") {
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  auto s = w.update(10, false, {Node::Connected, true, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Joining tailnet"));
}
