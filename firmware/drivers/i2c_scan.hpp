// The diag report's I2C scan, apart from ESP-IDF so it can be tested on the
// host (firmware/esp32/main/port_diag.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hg::i2c_scan {

enum class Probe { Ack, Nack, Timeout };

// A probe can time out on a healthy bus: another task holds the bus (touch
// polling, codec and battery reads share it), a device stretches the clock, or
// ESP-IDF's wait ends at a tick boundary before the transfer does. So an
// address that times out is tried again, and only a run of addresses that
// never answer means a line is held low.
constexpr int kTries = 3;
constexpr size_t kStuckAfter = 4;  // addresses in a row that time out on every try
constexpr uint16_t kFirst = 0x08;
constexpr uint16_t kEnd = 0x78;  // 0x78 and up are reserved

// The report's entries: "0x18" for an address that answered, "timeout at
// 0x19" for one that timed out on every try, and "bus stuck at 0x19" for the
// first of kStuckAfter such addresses in a row, which ends the scan.
// Ops supplies:
//   Probe probe(uint16_t addr)   one try at an address
//   void  pause()                between tries, so another transfer can finish
template <class Ops>
std::vector<std::string> scan(Ops& ops) {
  std::vector<std::string> out;
  std::vector<uint16_t> timeouts;  // the current run of addresses that timed out
  char buf[24];
  auto flush = [&] {
    for (uint16_t addr : timeouts) {
      std::snprintf(buf, sizeof(buf), "timeout at 0x%02x", addr);
      out.push_back(buf);
    }
    timeouts.clear();
  };
  for (uint16_t addr = kFirst; addr < kEnd; ++addr) {
    Probe p = ops.probe(addr);
    for (int i = 1; i < kTries && p == Probe::Timeout; ++i) {
      ops.pause();
      p = ops.probe(addr);
    }
    if (p == Probe::Timeout) {
      timeouts.push_back(addr);
      if (timeouts.size() < kStuckAfter) continue;
      std::snprintf(buf, sizeof(buf), "bus stuck at 0x%02x", timeouts.front());
      out.push_back(buf);
      return out;
    }
    flush();
    if (p == Probe::Ack) {
      std::snprintf(buf, sizeof(buf), "0x%02x", addr);
      out.push_back(buf);
    }
  }
  flush();
  return out;
}

}  // namespace hg::i2c_scan
