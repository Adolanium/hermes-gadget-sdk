#include <array>
#include <vector>

#include "axp2101.hpp"
#include "cores3.hpp"
#include "check.hpp"

TEST("AXP2101: battery, USB and charging readings never alter power configuration") {
  std::array<uint8_t, 256> regs{};
  regs[0x00] = 0x28;
  regs[0x01] = 0x20;
  regs[0x18] = 0x0a;
  regs[0x30] = 0x03;
  regs[0x34] = 0xcf;  // high status bits must not become voltage bits
  regs[0x35] = 0xa0;  // 4000 mV
  regs[0xa4] = 81;
  int failed_reg = -1;
  std::vector<std::pair<uint8_t, uint8_t>> writes;
  hg::Axp2101 power(
      [&](uint8_t reg, uint8_t* out, size_t n) {
        if (reg == failed_reg) return false;
        for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
        return true;
      },
      [&](uint8_t reg, uint8_t value) { writes.emplace_back(reg, value); return true; });
  auto p = power.read();
  CHECK(p.has_value());
  CHECK(p->battery_present == true);
  CHECK(p->external_power == true);
  CHECK(p->charging == true);
  CHECK(p->battery_mv == 4000);
  CHECK(p->battery_percent == 81);
  CHECK(writes.empty());

  regs[0xa4] = 255;
  CHECK_EQ(power.read()->battery_percent, uint8_t(77));  // junk gauge falls back to the voltage curve (4000 mV -> 77)
  regs[0xa4] = 0;
  p = power.read();
  CHECK_EQ(p->battery_percent, uint8_t(77));  // gauge stuck at 0: honest percent from voltage, no false "low battery"
  regs[0x34] = 0x0d;  // 3549 mV -> 27% on the 3300..4200 map
  regs[0x35] = 0xdd;
  CHECK_EQ(power.read()->battery_percent, uint8_t(27));
  regs[0x00] = 0x20;
  p = power.read();
  CHECK(p->battery_present == false);
  CHECK(p->charging == false);
  CHECK(!p->battery_mv);
  CHECK(!p->battery_percent);
  regs[0x00] = 0x08;
  regs[0x18] = regs[0x30] = 0;
  p = power.read();
  CHECK(!p->battery_mv);
  CHECK(!p->battery_percent);
  CHECK(writes.empty());
  failed_reg = 0x00;
  CHECK(!power.read());
  regs[0x00] = 0x28;
  regs[0x18] = 0x0a;
  regs[0x30] = 0x03;
  failed_reg = 0xa4;
  CHECK(!power.read());  // a gauge the bus can't read is unavailable, not estimated from voltage
  failed_reg = 0x10;
  CHECK(!power.power_off());
  CHECK(writes.empty());
  failed_reg = -1;
  regs[0x10] = 0x34;
  CHECK(power.power_off());
  CHECK_EQ(writes.size(), size_t(1));
  CHECK_EQ(writes[0].first, uint8_t(0x10));
  CHECK_EQ(writes[0].second, uint8_t(0x35));
}

TEST("AXP2101: audio supply enables ALDO1 at 3.3 V and preserves other rails") {
  std::array<uint8_t, 256> regs{};
  regs[0x90] = 0xa4;
  regs[0x92] = 0x60;
  regs[0x62] = 0x08;  // charge current must stay unchanged
  bool failed = false;
  hg::Axp2101 power(
      [&](uint8_t reg, uint8_t* out, size_t n) {
        if (failed) return false;
        for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
        return true;
      },
      [&](uint8_t reg, uint8_t value) { regs[reg] = value; return true; });
  CHECK(power.enable_aldo1_3v3());
  CHECK_EQ(regs[0x90], uint8_t(0xa5));
  CHECK_EQ(regs[0x92], uint8_t(0x7c));
  CHECK_EQ(regs[0x62], uint8_t(0x08));
  failed = true;
  CHECK(!power.enable_aldo1_3v3());
}

TEST("CoreS3: peripheral power preserves charger and external output configuration") {
  std::array<uint8_t, 256> pmic{}, io{};
  pmic.fill(0x60);
  pmic[0x90] = 0xe4;
  io.fill(0xff);
  auto expected_pmic = pmic;
  auto expected_io = io;
  std::vector<uint32_t> delays;
  hg::CoreS3Control control(
      [&](uint8_t addr, uint8_t reg, uint8_t& value) {
        value = (addr == 0x34 ? pmic : io)[reg]; return true;
      },
      [&](uint8_t addr, uint8_t reg, uint8_t value) {
        (addr == 0x34 ? pmic : io)[reg] = value; return true;
      },
      [&](uint32_t ms) {
        delays.push_back(ms);
        if (ms == 10) { CHECK_EQ(io[0x02], 0xfa); CHECK_EQ(io[0x03], 0xfd); }
        if (ms == 300) { CHECK_EQ(io[0x02], 0xff); CHECK_EQ(io[0x03], 0xff); }
      });
  CHECK(control.begin());
  expected_pmic[0x92] = 0x6d;  // ALDO1: 1.8 V
  expected_pmic[0x93] = 0x7c;  // ALDO2: 3.3 V
  expected_pmic[0x90] = 0x67;  // enable audio rails, disable backlight
  expected_io[0x04] = 0xfa;
  expected_io[0x05] = 0x7d;
  CHECK(pmic == expected_pmic);
  CHECK(io == expected_io);
  CHECK(delays == std::vector<uint32_t>({10, 300}));
  CHECK(control.set_brightness(50));
  CHECK_EQ(pmic[0x99], 0x78);  // DLDO1: 2.9 V, upper bits retained
  CHECK_EQ(pmic[0x90], 0xe7);
  CHECK(control.set_brightness(255));
  CHECK_EQ(pmic[0x99], 0x7c);  // clamp to 3.3 V
  CHECK(control.set_brightness(0));
  CHECK_EQ(pmic[0x90], 0x67);
  CHECK_EQ(pmic[0x62], 0x60);  // charging current is untouched
  CHECK(io == expected_io);
}

TEST("CoreS3: failed power access stops initialization without releasing reset") {
  bool read_ok = false;
  int writes = 0, delays = 0;
  hg::CoreS3Control control(
      [&](uint8_t, uint8_t, uint8_t& value) { value = 0; return read_ok; },
      [&](uint8_t, uint8_t, uint8_t) { ++writes; return false; },
      [&](uint32_t) { ++delays; });
  CHECK(!control.begin());
  CHECK_EQ(writes, 0);
  read_ok = true;
  CHECK(!control.begin());
  CHECK_EQ(writes, 1);
  CHECK_EQ(delays, 0);
  CHECK(!control.set_brightness(100));
  CHECK_EQ(writes, 2);
}

TEST("AXP2101: the power key's short press is read once and cleared; enabling touches only interrupts") {
  std::array<uint8_t, 256> regs{};
  regs[0x41] = 0x40;  // another interrupt already enabled
  regs[0x49] = 0x08;  // a press from before boot
  bool fail_reads = false;
  std::vector<std::pair<uint8_t, uint8_t>> writes;
  hg::Axp2101 power(
      [&](uint8_t reg, uint8_t* out, size_t n) {
        if (fail_reads) return false;
        for (size_t i = 0; i < n; ++i) out[i] = regs[reg + i];
        return true;
      },
      [&](uint8_t reg, uint8_t value) {
        writes.emplace_back(reg, value);
        if (reg == 0x49) regs[reg] = static_cast<uint8_t>(regs[reg] & ~value);  // write 1 to clear
        else regs[reg] = value;
        return true;
      });
  CHECK(power.enable_key_press());
  CHECK_EQ(writes.size(), size_t(2));
  CHECK(writes[0] == std::make_pair(uint8_t(0x41), uint8_t(0x48)));  // short press added, the rest kept
  CHECK(writes[1] == std::make_pair(uint8_t(0x49), uint8_t(0x08)));  // the old press is dropped
  CHECK(!power.take_key_press());

  writes.clear();
  regs[0x49] = 0x0c;  // short press, plus a long press that isn't ours to clear
  CHECK(power.take_key_press());
  CHECK(!power.take_key_press());
  CHECK_EQ(writes.size(), size_t(1));
  CHECK(writes[0] == std::make_pair(uint8_t(0x49), uint8_t(0x08)));
  CHECK_EQ(regs[0x49], uint8_t(0x04));

  regs[0x49] = 0x08;
  fail_reads = true;
  CHECK(!power.take_key_press());  // a failed read is no press
  CHECK(!power.enable_key_press());
}

TEST("AXP2101: external power is read from VBUS good, and a failed read is unknown") {
  uint8_t status = 0x20;
  bool fail = false;
  hg::Axp2101 power(
      [&](uint8_t reg, uint8_t* out, size_t n) {
        if (fail || reg != 0x00 || n != 1) return false;
        *out = status;
        return true;
      },
      [&](uint8_t, uint8_t) { return false; });
  CHECK(power.vbus_good() == std::optional<bool>(true));
  status = 0x08;  // battery present, no VBUS
  CHECK(power.vbus_good() == std::optional<bool>(false));
  fail = true;
  CHECK(!power.vbus_good().has_value());
}
