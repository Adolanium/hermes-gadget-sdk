#include <cstdint>
#include <utility>
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
  bool ok() const { return true; }
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

// Model the LCD side of the GPIO wire, not the driver's indexed-read API.
// Commands are decoded on rising edges. The dummy clock consumes bit 7;
// the LCD releases SDA after bit 0, so the following sample reads high.
struct PanelGpio {
  enum class Panel { C, E, Absent };
  explicit PanelGpio(Panel panel) : panel(panel) {}
  Panel panel;
  bool selected = false, clock_high = false, dc_high = false, reading = false, data_bit = false;
  bool healthy = true, unlocked = false;
  int operations = 0, fail_at = -1, read_clocks = 0, samples = 0, unlocks = 0;
  uint8_t byte = 0, command = 0, index = 0, register_value = 0xff;
  uint8_t dd_register = 0, cb_register = 0;
  unsigned bits = 0;
  std::vector<uint8_t> parameters;
  std::vector<std::pair<uint8_t, uint8_t>> reads;

  void operation() { if (operations++ == fail_at) healthy = false; }
  bool ok() const { return healthy; }
  void cs(bool high) {
    operation();
    if (!high) { byte = 0; bits = 0; }
    selected = !high;
  }
  void dc(bool high) { operation(); dc_high = high; }
  void data(bool high) { operation(); data_bit = high; }
  void input(bool enabled) {
    operation();
    if (enabled) {
      CHECK(selected && dc_high && !clock_high);
      reads.emplace_back(command, index);
      read_clocks = samples = 0;
      register_value = 0xff;
      if (panel == Panel::E && index == 1 && (command == 0xdd || command == 0xcb))
        register_value = command == 0xdd ? dd_register : cb_register;
      if (panel == Panel::C && unlocked && command == 0xd3)
        register_value = index == 2 ? 0x93 : index == 3 ? 0x42 : 0xff;
    } else if (reading) {
      CHECK(!selected);  // never drive SDA while the LCD is still selected
    }
    reading = enabled;
  }
  void sclk(bool high) {
    operation();
    if (high && !clock_high && selected) {
      if (reading) {
        ++read_clocks;
      } else {
        byte = static_cast<uint8_t>((byte << 1) | data_bit);
        if (++bits == 8) {
          if (!dc_high) {
            command = byte;
            parameters.clear();
          } else {
            parameters.push_back(byte);
            if (command == 0xd9) index = byte == 0 ? 0 : (byte & 0xf0) == 0x10 ? byte & 0x0f : 0xff;
            if (panel == Panel::E && command == 0xdd) dd_register = byte;
            if (panel == Panel::E && command == 0xcb) cb_register = byte;
            if (command == 0xc8 && parameters == std::vector<uint8_t>({0xff, 0x93, 0x42})) {
              unlocked = true;
              ++unlocks;
            }
          }
          bits = 0;
          byte = 0;
        }
      }
    }
    clock_high = high;
  }
  bool sample() {
    CHECK(selected && reading && dc_high && !clock_high);
    CHECK_EQ(read_clocks, samples + 1);
    ++samples;
    return read_clocks >= 1 && read_clocks <= 7
        ? ((register_value >> (7 - read_clocks)) & 1) : true;
  }
  void delay() {}
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

TEST("CoreS3 identifies C through the complete GPIO probe") {
  PanelGpio gpio(PanelGpio::Panel::C);
  hg::CoreS3PanelWire<PanelGpio> wire(gpio);
  CHECK(hg::cores3_panel(0x05, wire) == hg::CoreS3Panel::C);
  CHECK_EQ(wire.id_high, 0x13);
  CHECK_EQ(wire.id_low, 0x42);
  CHECK_EQ(gpio.unlocks, 1);
  CHECK((gpio.reads == std::vector<std::pair<uint8_t, uint8_t>>{{0xdd, 1}, {0xcb, 1}, {0xd3, 2}, {0xd3, 3}}));
  CHECK(!gpio.selected && !gpio.reading && !gpio.clock_high);
  CHECK_EQ(gpio.index, 0);
}

TEST("CoreS3 identifies E through GPIO without the C unlock") {
  PanelGpio gpio(PanelGpio::Panel::E);
  hg::CoreS3PanelWire<PanelGpio> wire(gpio);
  CHECK(hg::cores3_panel(0x05, wire) == hg::CoreS3Panel::E);
  CHECK_EQ(gpio.unlocks, 0);
  CHECK((gpio.reads == std::vector<std::pair<uint8_t, uint8_t>>{{0xdd, 1}, {0xcb, 1}}));
  CHECK(!gpio.selected && !gpio.reading && !gpio.clock_high);
  CHECK_EQ(gpio.index, 0);
}

TEST("CoreS3 an absent LCD stays unknown through GPIO") {
  PanelGpio gpio(PanelGpio::Panel::Absent);
  hg::CoreS3PanelWire<PanelGpio> wire(gpio);
  CHECK(hg::cores3_panel(0x05, wire) == hg::CoreS3Panel::Unknown);
  CHECK(!gpio.selected && !gpio.reading && !gpio.clock_high);
  CHECK_EQ(gpio.index, 0);
}

TEST("CoreS3 known touch hints never drive the GPIO probe") {
  PanelGpio gpio(PanelGpio::Panel::Absent);
  hg::CoreS3PanelWire<PanelGpio> wire(gpio);
  CHECK(hg::cores3_panel(0x10, wire) == hg::CoreS3Panel::C);
  CHECK(hg::cores3_panel(0x12, wire) == hg::CoreS3Panel::E);
  CHECK_EQ(gpio.operations, 0);
}

TEST("CoreS3 GPIO failures cannot select a panel despite a matching signature") {
  for (auto panel : {PanelGpio::Panel::C, PanelGpio::Panel::E}) {
    PanelGpio reference(panel);
    hg::CoreS3PanelWire<PanelGpio> good(reference);
    CHECK(hg::cores3_panel(0x05, good) != hg::CoreS3Panel::Unknown);
    for (int fail = 0; fail < reference.operations; ++fail) {
      PanelGpio gpio(panel);
      gpio.fail_at = fail;
      hg::CoreS3PanelWire<PanelGpio> wire(gpio);
      CHECK(hg::cores3_panel(0x05, wire) == hg::CoreS3Panel::Unknown);
      CHECK(!gpio.selected && !gpio.reading && !gpio.clock_high);
    }
  }
}
