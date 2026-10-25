// Host tests for the PCF85063 driver: the register facts it encodes, its
// conversions and its failure reporting, against a fake bus.
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "check.hpp"
#include "pcf85063.hpp"

namespace {

struct RegWrite {
  uint8_t reg;
  uint8_t value;
  bool operator==(const RegWrite& o) const { return reg == o.reg && value == o.value; }
};

// A register file standing in for a chip on the bus. `fail_read`/`fail_write`
// fail every access, like a dead line.
struct FakeChip {
  std::array<uint8_t, 256> regs{};
  bool fail_read = false, fail_write = false;
  std::vector<RegWrite> block_writes;  // first byte of each burst write
  hg::Pcf85063::BlockRead read = [this](uint8_t reg, uint8_t* out, size_t n) {
    if (fail_read) return false;
    for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
    return true;
  };
  hg::Pcf85063::BlockWrite write = [this](uint8_t reg, const uint8_t* data, size_t n) {
    if (fail_write) return false;
    block_writes.push_back({reg, static_cast<uint8_t>(n)});
    for (size_t i = 0; i < n; ++i) regs[reg + i] = data[i];
    return true;
  };
};

}  // namespace

TEST("PCF85063: a written time becomes one BCD burst from 0x04 and reads back") {
  FakeChip chip;
  hg::Pcf85063 rtc(chip.read, chip.write);
  const hg::WallTime t{2026, 10, 6, 2, 23, 59, 7};  // a Tuesday, 24 h clock
  CHECK(rtc.write(t));
  CHECK((chip.block_writes == std::vector<RegWrite>{{0x04, 7}}));
  CHECK_EQ(chip.regs[0x04], uint8_t(0x07));  // seconds, OS cleared
  CHECK_EQ(chip.regs[0x05], uint8_t(0x59));  // minutes
  CHECK_EQ(chip.regs[0x06], uint8_t(0x23));  // hours, 24 h mode (bit 7 = 0)
  CHECK_EQ(chip.regs[0x07], uint8_t(0x06));  // days
  CHECK_EQ(chip.regs[0x08], uint8_t(0x02));  // weekday, 0 = Sunday
  CHECK_EQ(chip.regs[0x09], uint8_t(0x10));  // months
  CHECK_EQ(chip.regs[0x0A], uint8_t(0x26));  // years
  hg::WallTime back{};
  CHECK(rtc.read(back));
  CHECK(back.year == 2026 && back.month == 10 && back.day == 6 && back.weekday == 2);
  CHECK(back.hour == 23 && back.minute == 59 && back.second == 7);
}

TEST("PCF85063: a stopped oscillator, a dead bus, a time that cannot be and out-of-range years report failure") {
  FakeChip chip;
  hg::Pcf85063 rtc(chip.read, chip.write);
  hg::WallTime out{2030, 12, 31, 1, 23, 59, 59};
  CHECK(rtc.write(out));
  chip.regs[0x04] = 0x87;  // OS set (0x07 BCD): backup power was lost
  CHECK(!rtc.read(out));   // the invalid time never reaches the caller
  chip.regs[0x04] = 0x07;  // OS clear again
  chip.fail_read = true;
  CHECK(!rtc.read(out));
  chip.fail_read = false;
  chip.fail_write = true;
  CHECK(!rtc.write(out));
  chip.fail_write = false;
  CHECK(rtc.read(out));
  chip.regs[0x06] = 0x25;  // hour 25
  CHECK(!rtc.read(out));
  chip.regs[0x06] = 0x23;
  chip.regs[0x09] = 0x13;  // month 13
  CHECK(!rtc.read(out));
  chip.regs[0x09] = 0x12;
  chip.regs[0x07] = 0x00;  // day 0
  CHECK(!rtc.read(out));
  // A table that holds no real time: digits past 9, and days past the month's end.
  chip.regs[0x07] = 0x31;  // December 31 again
  chip.regs[0x05] = 0x1A;  // minutes 1 and 10: would decode to 20
  CHECK(!rtc.read(out));
  chip.regs[0x05] = 0x59;
  chip.regs[0x0A] = 0x2A;  // years 2 and 10: would decode to 2030
  CHECK(!rtc.read(out));
  chip.regs[0x0A] = 0x26;  // 2026, not a leap year
  chip.regs[0x09] = 0x02;
  chip.regs[0x07] = 0x31;  // February 31
  CHECK(!rtc.read(out));
  chip.regs[0x07] = 0x29;  // February 29, 2026
  CHECK(!rtc.read(out));
  chip.regs[0x0A] = 0x28;  // 2028 is a leap year, so February 29 reads
  CHECK(rtc.read(out));
  CHECK(out.year == 2028 && out.month == 2 && out.day == 29);
  chip.regs[0x09] = 0x04;
  chip.regs[0x07] = 0x31;  // April 31
  CHECK(!rtc.read(out));
  hg::WallTime far_future = out;
  far_future.year = 2100;  // the years register holds 00-99
  CHECK(!rtc.write(far_future));
}

TEST("days_from_civil: known UTC epochs, leap years included") {
  CHECK_EQ(hg::days_from_civil(1970, 1, 1), INT64_C(0));
  CHECK_EQ(hg::days_from_civil(2000, 3, 1), INT64_C(11017));  // 2000 is a leap year
  CHECK_EQ(hg::days_from_civil(2024, 2, 29), INT64_C(19782));
  CHECK_EQ(hg::days_from_civil(2025, 1, 1), INT64_C(20089));
  CHECK_EQ(hg::days_from_civil(2026, 10, 6), INT64_C(20732));  // a Tuesday
  CHECK_EQ((hg::days_from_civil(2026, 10, 6) + 4) % 7, INT64_C(2));  // 1970-01-01: Thursday
  CHECK_EQ(hg::days_from_civil(2038, 1, 19), INT64_C(24855));
  CHECK_EQ(hg::days_from_civil(1970, 1, 2) - hg::days_from_civil(1970, 1, 1), INT64_C(1));
  CHECK_EQ(hg::days_from_civil(2025, 3, 1) - hg::days_from_civil(2025, 2, 28), INT64_C(1));
  CHECK_EQ(hg::days_from_civil(2100, 3, 1) - hg::days_from_civil(2100, 2, 28), INT64_C(1));  // 2100 is not
}
