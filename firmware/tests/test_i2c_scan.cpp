#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "i2c_scan.hpp"

namespace {

using hg::i2c_scan::Probe;
using Entries = std::vector<std::string>;

// A bus of devices that answer, addresses that time out a given number of
// times before they answer normally, and a line held low from one address on.
struct FakeBus {
  std::set<uint16_t> present;
  std::map<uint16_t, int> busy;  // address -> probes that time out first (-1: every one)
  uint16_t stuck_from = 0xFFFF;
  int probes = 0, pauses = 0;

  Probe probe(uint16_t addr) {
    ++probes;
    if (addr >= stuck_from) return Probe::Timeout;
    auto b = busy.find(addr);
    if (b != busy.end() && b->second != 0) {
      if (b->second > 0) --b->second;
      return Probe::Timeout;
    }
    return present.count(addr) ? Probe::Ack : Probe::Nack;
  }
  void pause() { ++pauses; }
};

// The parts on the AMOLED-1.75 boards: ES8311, TCA9554, AXP2101, ES7210, CST9217.
FakeBus amoled() {
  FakeBus bus;
  bus.present = {0x18, 0x20, 0x34, 0x40, 0x5a};
  return bus;
}

const int kAddresses = hg::i2c_scan::kEnd - hg::i2c_scan::kFirst;

}  // namespace

TEST("i2c scan: lists the addresses that answer, once each") {
  FakeBus bus = amoled();
  CHECK(hg::i2c_scan::scan(bus) == Entries({"0x18", "0x20", "0x34", "0x40", "0x5a"}));
  CHECK_EQ(bus.probes, kAddresses);
  CHECK_EQ(bus.pauses, 0);
}

TEST("i2c scan: one timeout no longer hides the rest of the bus") {
  // The AMOLED-1.75C report: 0x18 answered, then 0x19 timed out once and the
  // scan said "bus stuck", while the AXP2101 at 0x34 was answering battery reads.
  FakeBus bus = amoled();
  bus.busy[0x19] = 1;
  CHECK(hg::i2c_scan::scan(bus) == Entries({"0x18", "0x20", "0x34", "0x40", "0x5a"}));
  CHECK_EQ(bus.probes, kAddresses + 1);
  CHECK_EQ(bus.pauses, 1);
}

TEST("i2c scan: an address that always times out is named and the scan goes on") {
  FakeBus bus = amoled();
  bus.busy[0x19] = -1;
  CHECK(hg::i2c_scan::scan(bus) == Entries({"0x18", "timeout at 0x19", "0x20", "0x34", "0x40", "0x5a"}));
  CHECK_EQ(bus.probes, kAddresses + hg::i2c_scan::kTries - 1);
}

TEST("i2c scan: a device busy for every try at its address is reported, not skipped") {
  FakeBus bus = amoled();
  bus.busy[0x34] = hg::i2c_scan::kTries;
  CHECK(hg::i2c_scan::scan(bus) == Entries({"0x18", "0x20", "timeout at 0x34", "0x40", "0x5a"}));
}

TEST("i2c scan: a few timeouts in a row are listed, including at the end of the range") {
  FakeBus bus = amoled();
  for (uint16_t a : {0x21, 0x22, 0x23, 0x75, 0x76, 0x77}) bus.busy[a] = -1;
  CHECK(hg::i2c_scan::scan(bus) == Entries({"0x18", "0x20", "timeout at 0x21", "timeout at 0x22", "timeout at 0x23",
                                            "0x34", "0x40", "0x5a", "timeout at 0x75", "timeout at 0x76",
                                            "timeout at 0x77"}));
}

TEST("i2c scan: a line held low is reported as a stuck bus and ends the scan") {
  FakeBus bus = amoled();
  bus.stuck_from = 0;
  CHECK(hg::i2c_scan::scan(bus) == Entries({"bus stuck at 0x08"}));
  CHECK_EQ(bus.probes, static_cast<int>(hg::i2c_scan::kStuckAfter) * hg::i2c_scan::kTries);
}

TEST("i2c scan: a bus that sticks partway keeps what answered before") {
  FakeBus bus = amoled();
  bus.stuck_from = 0x19;
  CHECK(hg::i2c_scan::scan(bus) == Entries({"0x18", "bus stuck at 0x19"}));
}
