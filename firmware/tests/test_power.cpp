#include <array>
#include <vector>

#include "axp2101.hpp"
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
  CHECK(!power.read()->battery_percent);
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
