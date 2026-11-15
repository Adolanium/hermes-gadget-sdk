// Host tests for the RX8130CE driver: the register facts it encodes, its
// conversions and its failure reporting, against a fake bus.
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "check.hpp"
#include "rx8130.hpp"

namespace {

// A register file standing in for a chip on the bus. `fail_read`/`fail_write`
// fail every access, like a dead line. The flag register clears on 0 and
// ignores 1, as the chip's does.
struct FakeRx8130 {
  std::array<uint8_t, 256> regs{};
  bool fail_read = false, fail_write = false;
  std::vector<std::pair<uint8_t, size_t>> writes;  // start register and length of each write
  hg::Rx8130::BlockRead read = [this](uint8_t reg, uint8_t* out, size_t n) {
    if (fail_read) return false;
    for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
    return true;
  };
  hg::Rx8130::BlockWrite write = [this](uint8_t reg, const uint8_t* data, size_t n) {
    if (fail_write) return false;
    writes.emplace_back(reg, n);
    for (size_t i = 0; i < n; ++i) {
      const uint8_t at = static_cast<uint8_t>(reg + i);
      regs[at] = at == 0x1D ? static_cast<uint8_t>(regs[at] & data[i]) : data[i];
    }
    return true;
  };
  // Tuesday 2026-10-06 23:59:07, running.
  void set_time() {
    const uint8_t t[7] = {0x07, 0x59, 0x23, 0x04, 0x06, 0x10, 0x26};
    for (size_t i = 0; i < 7; ++i) regs[0x10 + i] = t[i];
    regs[0x1D] = 0;
    regs[0x1E] = 0;
  }
};

}  // namespace

TEST("RX8130: a written time becomes one BCD burst from 0x10 with the weekday as one bit, and reads back") {
  FakeRx8130 chip;
  hg::Rx8130 rtc(chip.read, chip.write);
  const hg::WallTime t{2026, 10, 6, 2, 23, 59, 7};  // a Tuesday, 24 h clock
  CHECK(rtc.write(t));
  CHECK(chip.writes.size() == 2 && chip.writes[0] == (std::pair<uint8_t, size_t>{0x10, 7}));
  CHECK_EQ(chip.regs[0x10], uint8_t(0x07));  // seconds
  CHECK_EQ(chip.regs[0x11], uint8_t(0x59));  // minutes
  CHECK_EQ(chip.regs[0x12], uint8_t(0x23));  // hours, 24 h
  CHECK_EQ(chip.regs[0x13], uint8_t(0x04));  // weekday: bit 2, bit 0 is Sunday
  CHECK_EQ(chip.regs[0x14], uint8_t(0x06));  // day
  CHECK_EQ(chip.regs[0x15], uint8_t(0x10));  // month
  CHECK_EQ(chip.regs[0x16], uint8_t(0x26));  // year
  hg::WallTime back{};
  CHECK(rtc.read(back));
  CHECK(back.year == 2026 && back.month == 10 && back.day == 6 && back.weekday == 2);
  CHECK(back.hour == 23 && back.minute == 59 && back.second == 7);
}

TEST("RX8130: writing clears VLF alone among the flags, and starts a stopped clock without touching its other control bits") {
  FakeRx8130 chip;
  hg::Rx8130 rtc(chip.read, chip.write);
  chip.regs[0x1D] = 0x9A;  // VBLF, TF, AF and VLF
  chip.regs[0x1E] = 0x48;  // STOP and AIE
  chip.regs[0x1F] = 0x30;  // INIEN and CHGEN: the board's backup supply, not the driver's to change
  CHECK(rtc.write(hg::WallTime{2026, 11, 14, 6, 16, 30, 0}));
  CHECK_EQ(chip.regs[0x1D], uint8_t(0x98));
  CHECK_EQ(chip.regs[0x1E], uint8_t(0x08));
  CHECK_EQ(chip.regs[0x1F], uint8_t(0x30));
  hg::WallTime back{};
  CHECK(rtc.read(back));
  CHECK(back.weekday == 6 && back.hour == 16);

  FakeRx8130 running;
  hg::Rx8130 rtc2(running.read, running.write);
  running.regs[0x1E] = 0x08;
  CHECK(rtc2.write(hg::WallTime{2026, 11, 14, 6, 16, 30, 0}));
  CHECK(running.writes.size() == 2 && running.writes[1].first == 0x1D);  // control 0 is not rewritten
}

TEST("RX8130: lost power, a stopped clock, a dead bus and a time that cannot be report failure") {
  FakeRx8130 chip;
  hg::Rx8130 rtc(chip.read, chip.write);
  hg::WallTime t{};
  chip.set_time();
  CHECK(rtc.read(t));
  chip.regs[0x1D] = 0x02;  // VLF
  CHECK(!rtc.read(t));
  chip.set_time();
  chip.regs[0x1E] = 0x40;  // STOP
  CHECK(!rtc.read(t));
  chip.regs[0x1E] = 0x80;  // TEST
  CHECK(!rtc.read(t));
  chip.set_time();
  chip.fail_read = true;
  CHECK(!rtc.read(t));
  chip.fail_read = false;

  for (uint8_t week : {uint8_t(0x00), uint8_t(0x06), uint8_t(0x7F)}) {  // no day, two days, every day
    chip.set_time();
    chip.regs[0x13] = week;
    CHECK(!rtc.read(t));
  }
  const std::pair<uint8_t, uint8_t> impossible[] = {
      {0x10, 0x60}, {0x10, 0x0A}, {0x11, 0x60}, {0x12, 0x24}, {0x14, 0x00},
      {0x14, 0x32}, {0x15, 0x00}, {0x15, 0x13}, {0x16, 0xA0},
  };
  for (const auto& [reg, value] : impossible) {
    chip.set_time();
    chip.regs[reg] = value;
    CHECK(!rtc.read(t));
  }
  chip.set_time();
  chip.regs[0x14] = 0x29;  // 29 February
  chip.regs[0x15] = 0x02;
  CHECK(!rtc.read(t));  // 2026 is no leap year
  chip.regs[0x16] = 0x28;
  CHECK(rtc.read(t));  // 2028 is

  chip.set_time();
  CHECK(!rtc.write(hg::WallTime{1999, 12, 31, 5, 0, 0, 0}));
  CHECK(!rtc.write(hg::WallTime{2100, 1, 1, 5, 0, 0, 0}));
  CHECK(!rtc.write(hg::WallTime{2026, 1, 1, 7, 0, 0, 0}));  // no eighth weekday
  chip.fail_write = true;
  CHECK(!rtc.write(hg::WallTime{2026, 1, 1, 4, 0, 0, 0}));
}
