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

TEST("tailnet watch: a short gap keeps the session, a long one or a lost node ends it") {
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

  s = v.update(30000, true, {Node::Reconnecting, false, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Tailnet reconnecting"));
}

TEST("tailnet watch: no Wi-Fi means no tunnel, at once") {
  TailnetWatch w;
  w.update(0, true, {Node::Connected, true, ""});
  auto s = w.update(10, false, {Node::Connected, true, ""});
  CHECK(s.tell && !s.up);
  CHECK_EQ(s.detail, std::string("Joining tailnet"));
}
