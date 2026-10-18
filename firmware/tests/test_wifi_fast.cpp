#include <optional>
#include <string>

#include "check.hpp"
#include "wifi_fast.hpp"

namespace {

hg::WifiFastAp home() {
  hg::WifiFastAp ap;
  ap.ssid = "Home";
  const uint8_t bssid[6] = {0x24, 0x0a, 0xc4, 0x01, 0xfe, 0x7b};
  for (int i = 0; i < 6; ++i) ap.bssid[i] = bssid[i];
  ap.channel = 11;
  ap.ip = 0x0501a8c0;  // 192.168.1.5, as lwIP stores it
  ap.netmask = 0x00ffffff;
  ap.gateway = 0x0101a8c0;
  ap.dns = 0x0101a8c0;
  return ap;
}

}  // namespace

TEST("wifi fast: a record survives a round trip through storage") {
  const auto ap = home();
  const auto back = hg::WifiFastAp::parse(ap.encode());
  CHECK(back.has_value());
  CHECK(back && *back == ap);
  CHECK(back && back->has_lease());
}

TEST("wifi fast: an SSID with spaces and bytes outside ASCII is kept exactly") {
  auto ap = home();
  ap.ssid = std::string("Caf\xc3\xa9 2 GHz \x01", 13);
  const auto back = hg::WifiFastAp::parse(ap.encode());
  CHECK(back && back->ssid == ap.ssid);
}

TEST("wifi fast: a damaged or foreign record is ignored") {
  const std::string good = home().encode();
  CHECK(!hg::WifiFastAp::parse(""));
  CHECK(!hg::WifiFastAp::parse("2" + good.substr(1)));         // another format
  CHECK(!hg::WifiFastAp::parse(good.substr(0, good.rfind(' '))));  // last field missing
  CHECK(!hg::WifiFastAp::parse(good + " 7"));                   // extra field
  CHECK(!hg::WifiFastAp::parse("1 486f6d65 240ac401fe 6 0 0 0 0"));    // short BSSID
  CHECK(!hg::WifiFastAp::parse("1 486f6d65 240ac401fe7b 0 0 0 0 0"));  // no channel
  CHECK(!hg::WifiFastAp::parse("1 486f6d65 240ac401fe7b f 0 0 0 0"));  // channel 15
  CHECK(!hg::WifiFastAp::parse("1  240ac401fe7b 6 0 0 0 0"));          // no SSID
  CHECK(!hg::WifiFastAp::parse("1 486f6d6 240ac401fe7b 6 0 0 0 0"));   // odd hex
  CHECK(!hg::WifiFastAp::parse("1 486f6d65 240ac401fe7b 6 0 0 0 1ffffffff"));
  const auto no_lease = hg::WifiFastAp::parse("1 486f6d65 240ac401fe7b 6 0 0 0 0");
  CHECK(no_lease && !no_lease->has_lease());
}

TEST("wifi fast: with a record, the first attempt on that network aims at its access point") {
  hg::WifiFastJoin f;
  f.load(home().encode());
  const hg::WifiFastAp* aim = f.aim("Home");
  CHECK(aim != nullptr);
  CHECK(aim && aim->channel == 11 && aim->bssid[5] == 0x7b);
  CHECK(f.trying());
}

TEST("wifi fast: no record, a damaged one, or another network joins normally") {
  hg::WifiFastJoin f;
  CHECK(f.aim("Home") == nullptr);
  f.load(std::string("garbage"));
  CHECK(f.aim("Home") == nullptr);
  f.load(home().encode());
  CHECK(f.aim("Office") == nullptr);
  CHECK(!f.trying());
}

TEST("wifi fast: after the aimed attempt fails, attempts join normally until a join works") {
  hg::WifiFastJoin f;
  f.load(home().encode());
  CHECK(f.aim("Home") != nullptr);
  CHECK(f.failed());             // the aimed attempt: fall back to the scan path
  CHECK(f.aim("Home") == nullptr);
  CHECK(!f.failed());            // a normal attempt failing is the caller's to count
  CHECK(f.aim("Home") == nullptr);
  CHECK(!f.joined(home()));      // joined the same access point: nothing to write
  CHECK(f.aim("Home") != nullptr);  // the next outage aims again
}

TEST("wifi fast: a join on another access point or lease replaces the record") {
  hg::WifiFastJoin f;
  f.load(home().encode());
  auto moved = home();
  moved.channel = 6;
  moved.bssid[5] = 0x7c;
  const auto written = f.joined(moved);
  CHECK(written.has_value());
  CHECK(written && hg::WifiFastAp::parse(*written) == std::optional<hg::WifiFastAp>(moved));
  const hg::WifiFastAp* aim = f.aim("Home");
  CHECK(aim && aim->channel == 6);
  f.failed();
  auto renewed = moved;
  renewed.ip = 0x0601a8c0;
  CHECK(f.joined(renewed).has_value());
}

TEST("wifi fast: the first join with no record writes one") {
  hg::WifiFastJoin f;
  f.load(std::nullopt);
  CHECK(f.joined(home()).has_value());
  CHECK(f.aim("Home") != nullptr);
}

TEST("wifi fast: an access point without a usable channel is not recorded") {
  hg::WifiFastJoin f;
  auto ap = home();
  ap.channel = 0;
  CHECK(!f.joined(ap));
  CHECK(f.aim("Home") == nullptr);
}

TEST("wifi fast: forgetting the network drops the record") {
  hg::WifiFastJoin f;
  f.load(home().encode());
  CHECK(f.forget());
  CHECK(f.aim("Home") == nullptr);
  CHECK(!f.forget());
}

TEST("wifi fast: an aimed attempt that never reports back counts as failed") {
  hg::WifiFastJoin f;
  f.load(home().encode());
  CHECK(f.aim("Home") != nullptr);
  CHECK(f.aim("Home") == nullptr);  // timed out: the next attempt joins normally
  CHECK(!f.failed());
}
