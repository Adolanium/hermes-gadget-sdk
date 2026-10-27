// Host tests for the BMI270 driver: the configuration image upload, the
// accelerometer settings, the conversion and its failure reporting, against a
// fake bus.
#include <array>
#include <cstdint>
#include <vector>

#include "bmi270.hpp"
#include "check.hpp"
#include "third_party/bmi270/bmi270_config.hpp"

namespace {

// A register file standing in for the chip. INIT_DATA bursts land in `image`
// at the word address last written to INIT_ADDR_0/1; INIT_CTRL <- 1 reports
// init_ok unless `refuse_image`.
struct FakeChip {
  std::array<uint8_t, 256> regs{};
  std::vector<uint8_t> image = std::vector<uint8_t>(hg::kBmi270ConfigSize, 0);
  bool fail_read = false, refuse_image = false;
  size_t bursts = 0, largest_burst = 0;
  hg::Bmi270::Read read = [this](uint8_t reg, uint8_t* out, size_t n) {
    if (fail_read) return false;
    for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
    return true;
  };
  hg::Bmi270::Write write = [this](uint8_t reg, const uint8_t* data, size_t n) {
    if (reg == 0x5E) {
      const size_t at = 2 * (static_cast<size_t>(regs[0x5B] & 0x0F) | (static_cast<size_t>(regs[0x5C]) << 4));
      for (size_t i = 0; i < n && at + i < image.size(); ++i) image[at + i] = data[i];
      ++bursts;
      if (n > largest_burst) largest_burst = n;
      return true;
    }
    for (size_t i = 0; i < n; ++i) regs[reg + i] = data[i];
    if (reg == 0x59 && data[0] == 0x01 && !refuse_image) regs[0x21] = 0x01;
    return true;
  };
};

}  // namespace

TEST("BMI270: start loads Bosch's image, then turns on only the accelerometer at +-4 g") {
  FakeChip chip;
  chip.regs[0x00] = 0x24;
  std::vector<uint32_t> delays;
  hg::Bmi270 imu(chip.read, chip.write, [&](uint32_t ms) { delays.push_back(ms); });
  CHECK(imu.begin());
  CHECK(chip.image == std::vector<uint8_t>(hg::kBmi270Config, hg::kBmi270Config + hg::kBmi270ConfigSize));
  CHECK(chip.largest_burst <= hg::Bmi270::kBurst);
  CHECK_EQ(chip.bursts, (hg::kBmi270ConfigSize + hg::Bmi270::kBurst - 1) / hg::Bmi270::kBurst);
  CHECK_EQ(chip.regs[0x7E], 0xB6);  // soft reset first
  CHECK_EQ(chip.regs[0x7C], 0x00);  // advanced power save off
  CHECK_EQ(chip.regs[0x40], 0xA6);  // 25 Hz, normal filter
  CHECK_EQ(chip.regs[0x41], 0x01);  // +-4 g
  CHECK_EQ(chip.regs[0x7D], 0x04);  // accelerometer only
  CHECK(!delays.empty() && delays[0] >= 2);  // the reset's 2 ms
}

TEST("BMI270: a wrong id, a dead bus or a refused image stops start") {
  FakeChip wrong;
  wrong.regs[0x00] = 0x05;
  hg::Bmi270 a(wrong.read, wrong.write, [](uint32_t) {});
  CHECK(!a.begin());
  CHECK_EQ(wrong.bursts, size_t(0));

  FakeChip dead;
  dead.fail_read = true;
  hg::Bmi270 b(dead.read, dead.write, [](uint32_t) {});
  CHECK(!b.begin());

  FakeChip refusing;
  refusing.regs[0x00] = 0x24;
  refusing.refuse_image = true;
  int waits = 0;
  hg::Bmi270 c(refusing.read, refusing.write, [&](uint32_t) { ++waits; });
  CHECK(!c.begin());
  CHECK_EQ(refusing.regs[0x7D], 0x00);  // the accelerometer never started
  CHECK(waits > 2);                     // it polled the status before giving up
}

TEST("BMI270: samples are little-endian and scale 8192 LSB/g to milli-g") {
  FakeChip chip;
  const int16_t raw[3] = {8192, -4096, 0};
  for (int i = 0; i < 3; ++i) {
    chip.regs[0x0C + 2 * i] = static_cast<uint8_t>(raw[i] & 0xFF);
    chip.regs[0x0C + 2 * i + 1] = static_cast<uint8_t>((raw[i] >> 8) & 0xFF);
  }
  hg::Bmi270 imu(chip.read, chip.write, [](uint32_t) {});
  int16_t mg[3];
  CHECK(imu.read(mg));
  CHECK_EQ(mg[0], 1000);
  CHECK_EQ(mg[1], -500);
  CHECK_EQ(mg[2], 0);
  chip.fail_read = true;
  CHECK(!imu.read(mg));
}
