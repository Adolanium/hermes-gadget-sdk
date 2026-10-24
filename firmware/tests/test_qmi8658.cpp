// Host tests for the QMI8658 driver: the register facts it encodes, its
// conversion and its failure reporting, against a fake bus.
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "check.hpp"
#include "qmi8658.hpp"

namespace {

struct RegWrite {
  uint8_t reg;
  uint8_t value;
  bool operator==(const RegWrite& o) const { return reg == o.reg && value == o.value; }
};

// A register file standing in for a chip on the bus. `fail_read`/`fail_write`
// fail every access, like a dead line; a write to `drop` is acknowledged but
// not kept, like a chip that is not ready yet.
struct FakeChip {
  std::array<uint8_t, 256> regs{};
  bool fail_read = false, fail_write = false;
  int drop = -1;
  std::vector<RegWrite> writes;
  hg::Qmi8658::Read read = [this](uint8_t reg, uint8_t* out, size_t n) {
    if (fail_read) return false;
    for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
    return true;
  };
  hg::Qmi8658::Write write = [this](uint8_t reg, uint8_t value) {
    if (fail_write) return false;
    writes.push_back({reg, value});
    if (reg != drop) regs[reg] = value;
    return true;
  };
};

}  // namespace

TEST("QMI8658: a chip with the wrong id or a dead bus refuses to start untouched") {
  FakeChip chip;
  std::vector<uint32_t> delays;
  chip.regs[0x00] = 0x00;  // not the QMI8658's 0x05
  hg::Qmi8658 wrong_id(chip.read, chip.write, [&](uint32_t ms) { delays.push_back(ms); });
  CHECK(!wrong_id.begin());
  chip.fail_read = true;
  hg::Qmi8658 silent(chip.read, chip.write, [&](uint32_t ms) { delays.push_back(ms); });
  CHECK(!silent.begin());
  chip.fail_read = false;
  chip.fail_write = true;
  hg::Qmi8658 stuck(chip.read, chip.write, [&](uint32_t ms) { delays.push_back(ms); });
  chip.regs[0x00] = 0x05;
  CHECK(!stuck.begin());
  CHECK(chip.writes.empty());
  CHECK(delays.empty());  // the reset wait only happens after a successful reset write
}

TEST("QMI8658: a setting the chip does not keep makes start fail") {
  for (const int reg : {0x02, 0x03, 0x08}) {  // CTRL1, CTRL2, CTRL7
    FakeChip chip;
    chip.regs[0x00] = 0x05;
    chip.drop = reg;
    hg::Qmi8658 imu(chip.read, chip.write, [](uint32_t) {});
    CHECK(!imu.begin());
  }
}

TEST("QMI8658: start writes the datasheet sequence and waits out the reset") {
  FakeChip chip;
  std::vector<uint32_t> delays;
  chip.regs[0x00] = 0x05;
  hg::Qmi8658 imu(chip.read, chip.write, [&](uint32_t ms) { delays.push_back(ms); });
  CHECK(imu.begin());
  const std::vector<RegWrite> expected = {
      {0x60, 0xB0},  // soft reset
      {0x02, 0x40},  // CTRL1: address auto-increment, little-endian
      {0x03, 0x1D},  // CTRL2: +-4 g, 21 Hz low-power
      {0x08, 0x01},  // CTRL7: accelerometer on
  };
  CHECK(chip.writes == expected);
  CHECK(delays == std::vector<uint32_t>{15});
}

TEST("QMI8658: raw little-endian counts become signed milli-g at 8192 LSB/g") {
  FakeChip chip;
  std::vector<uint32_t> delays;
  chip.regs[0x00] = 0x05;
  hg::Qmi8658 imu(chip.read, chip.write, [&](uint32_t ms) { delays.push_back(ms); });
  CHECK(imu.begin());
  const struct {
    int16_t raw;
    int16_t mg;
  } cases[] = {
      {-8192, -1000}, {8192, 1000}, {-16384, -2000}, {4096, 500}, {32767, 3999}, {0, 0},
  };
  for (const auto& c : cases) {
    const uint16_t u = static_cast<uint16_t>(c.raw);
    chip.regs[0x35] = static_cast<uint8_t>(u);       // AX_L
    chip.regs[0x36] = static_cast<uint8_t>(u >> 8);  // AX_H
    chip.regs[0x37] = chip.regs[0x38] = chip.regs[0x39] = chip.regs[0x3A] = 0;
    int16_t mg[3] = {1, 1, 1};
    CHECK(imu.read(mg));
    CHECK_EQ(mg[0], c.mg);
    CHECK_EQ(mg[1], 0);
    CHECK_EQ(mg[2], 0);
  }
  chip.fail_read = true;
  int16_t mg[3];
  CHECK(!imu.read(mg));
}
