#include <cstdint>
#include <vector>

#include "check.hpp"
#include "cores3_panel.hpp"

namespace {
struct PanelIo {
  int calls = 0;
  bool write(uint8_t, const uint8_t*, unsigned) { ++calls; return true; }
  bool read(uint8_t, uint8_t, uint8_t&) { ++calls; return false; }
};
struct CPanelIo {
  bool unlocked = false;
  bool write(uint8_t cmd, const uint8_t* data, unsigned size) {
    if (cmd == 0xc8 && size == 3 && data[0] == 0xff && data[1] == 0x93 && data[2] == 0x42)
      unlocked = true;
    return true;
  }
  bool read(uint8_t cmd, uint8_t index, uint8_t& value) {
    value = cmd == 0xd3 && unlocked ? (index == 2 ? 0x13 : 0x42) : 0;
    return true;
  }
};
struct WireGpio {
  bool selected = false, clock_high = false, reading = false, dc_high = false, bit = false;
  uint8_t raw = 0x27;
  int samples = 0, read_clocks = 0;
  std::vector<bool> sent;
  void cs(bool high) { selected = !high; }
  void dc(bool high) { dc_high = high; }
  void data(bool high) { bit = high; }
  void input(bool enabled) {
    if (!enabled && reading) CHECK(!selected);
    reading = enabled;
  }
  void sclk(bool high) {
    clock_high = high;
    if (high && reading) ++read_clocks;
    if (high && !reading) sent.push_back(bit);
  }
  bool sample() {
    CHECK(selected && reading && dc_high && !clock_high);
    CHECK_EQ(read_clocks, samples + 1);  // exactly one dummy clock before data
    return (raw >> (7 - samples++)) & 1;
  }
  void delay() {}
};

struct RegisterPanelIo {
  uint8_t dd = 0, cb = 0, id_high = 0, id_low = 0;
  int fail_at = -1, calls = 0, c_key_at = -1;
  bool c_key = false;
  uint8_t last_cmd = 0, last_value = 0xff;
  bool write(uint8_t cmd, const uint8_t* data, unsigned size) {
    last_cmd = cmd;
    last_value = size ? data[0] : 0xff;
    c_key |= cmd == 0xc8;
    if (cmd == 0xc8) c_key_at = calls;
    return calls++ != fail_at;
  }
  bool read(uint8_t cmd, uint8_t index, uint8_t& value) {
    value = cmd == 0xdd ? dd : cmd == 0xcb ? cb : index == 2 ? id_high : id_low;
    return calls++ != fail_at;
  }
};
}

TEST("CoreS3 3-wire read samples SDA low and restores write direction") {
  WireGpio gpio;
  hg::CoreS3PanelWire<WireGpio> wire(gpio);
  uint8_t value = 0;
  CHECK(wire.read(0xd3, 2, value));
  CHECK_EQ(value, 0x13);
  CHECK_EQ(wire.id_high, value);
  CHECK_EQ(gpio.samples, 8);
  CHECK_EQ(gpio.read_clocks, 9);
  CHECK(!gpio.reading && !gpio.selected && !gpio.clock_high);
  std::vector<uint8_t> bytes;
  for (unsigned i = 0; i < gpio.sent.size(); i += 8) {
    uint8_t byte = 0;
    for (unsigned b = 0; b < 8; ++b) byte = (byte << 1) | gpio.sent[i + b];
    bytes.push_back(byte);
  }
  CHECK(bytes == std::vector<uint8_t>({0xd9, 0x12, 0, 0xd3, 0}));
}

TEST("CoreS3 known touch firmware keeps its panel without SPI probing") {
  PanelIo io;
  CHECK(hg::cores3_panel(0x10, io) == hg::CoreS3Panel::C);
  CHECK(hg::cores3_panel(0x12, io) == hg::CoreS3Panel::E);
  CHECK_EQ(io.calls, 0);
}

TEST("CoreS3 touch firmware 0x05 uses a positively identified C panel") {
  CPanelIo io;
  CHECK(hg::cores3_panel(0x05, io) == hg::CoreS3Panel::C);
}

TEST("CoreS3 LCD E is identified before sending the C unlock key") {
  RegisterPanelIo io;
  io.dd = 1;
  io.cb = 0x1c;
  CHECK(hg::cores3_panel(0x05, io) == hg::CoreS3Panel::E);
  CHECK(!io.c_key);
  CHECK_EQ(io.last_cmd, 0xd9);
  CHECK_EQ(io.last_value, 0);
}

TEST("CoreS3 unknown or partial LCD signatures remain unavailable") {
  for (auto values : std::vector<std::vector<uint8_t>>{{0, 0, 0, 0}, {0x7f, 0x7f, 0x7f, 0x7f},
       {1, 0, 0, 0}, {0, 0x1c, 0, 0}, {0, 0, 0x13, 0}, {0, 0, 0, 0x42}}) {
    RegisterPanelIo io;
    io.dd = values[0]; io.cb = values[1]; io.id_high = values[2]; io.id_low = values[3];
    CHECK(hg::cores3_panel(0x05, io) == hg::CoreS3Panel::Unknown);
    CHECK_EQ(io.last_cmd, 0xd9);
    CHECK_EQ(io.last_value, 0);
  }
}

TEST("CoreS3 failed E transactions cannot select E using stale data") {
  RegisterPanelIo reference;
  reference.dd = 1; reference.cb = 0x1c;
  CHECK(hg::cores3_panel(0x05, reference) == hg::CoreS3Panel::E);
  for (int fail = 0; fail < reference.calls; ++fail) {
    RegisterPanelIo io;
    io.dd = 1; io.cb = 0x1c; io.fail_at = fail;
    CHECK(hg::cores3_panel(0x05, io) == hg::CoreS3Panel::Unknown);
  }
}

TEST("CoreS3 failed C transactions cannot select C using stale data") {
  RegisterPanelIo reference;
  reference.id_high = 0x13; reference.id_low = 0x42;
  CHECK(hg::cores3_panel(0x05, reference) == hg::CoreS3Panel::C);
  for (int fail = reference.c_key_at; fail < reference.calls; ++fail) {
    RegisterPanelIo io;
    io.id_high = 0x13; io.id_low = 0x42; io.fail_at = fail;
    CHECK(hg::cores3_panel(0x05, io) == hg::CoreS3Panel::Unknown);
  }
}

TEST("CoreS3 all unknown touch firmware values require LCD evidence") {
  for (unsigned firmware = 0; firmware < 256; ++firmware) {
    if (firmware == 0x10 || firmware == 0x12) continue;
    RegisterPanelIo io;
    CHECK(hg::cores3_panel(firmware, io) == hg::CoreS3Panel::Unknown);
  }
}

TEST("CoreS3 an unrecognized touch firmware is not a panel identity") {
  PanelIo io;
  CHECK(hg::cores3_panel(0x05, io) == hg::CoreS3Panel::Unknown);
}
