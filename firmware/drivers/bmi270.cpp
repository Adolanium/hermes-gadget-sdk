#include "bmi270.hpp"

#include <algorithm>

#include "third_party/bmi270/bmi270_config.hpp"

namespace hg {
namespace {

constexpr uint8_t kChipId = 0x00, kChipIdValue = 0x24;
constexpr uint8_t kAccelBase = 0x0C;  // ACC_X_LSB .. ACC_Z_MSB
constexpr uint8_t kInternalStatus = 0x21, kInitOk = 0x01;
constexpr uint8_t kAccConf = 0x40, kAccRange = 0x41;
constexpr uint8_t kInitCtrl = 0x59, kInitAddr0 = 0x5B, kInitData = 0x5E;
constexpr uint8_t kPwrConf = 0x7C, kPwrCtrl = 0x7D, kCmd = 0x7E;
constexpr uint8_t kSoftReset = 0xB6;
constexpr uint8_t kAccConfValue = 0xA6, kAccRangeValue = 0x01, kPwrCtrlValue = 0x04;
constexpr uint32_t kResetMs = 2, kPowerSaveExitMs = 1, kInitPollMs = 5;
constexpr int kInitPolls = 8;          // 40 ms, twice the datasheet's 20 ms
constexpr int32_t kLsbPerG = 8192;     // at +-4 g

}  // namespace

bool Bmi270::begin() {
  uint8_t id = 0;
  if (!read_(kChipId, &id, 1) || id != kChipIdValue) return false;
  if (!write8(kCmd, kSoftReset)) return false;
  delay_ms_(kResetMs);
  if (!write8(kPwrConf, 0x00)) return false;  // advanced power save off while loading
  delay_ms_(kPowerSaveExitMs);
  if (!write8(kInitCtrl, 0x00)) return false;
  for (size_t at = 0; at < kBmi270ConfigSize; at += kBurst) {
    const uint8_t addr[2] = {static_cast<uint8_t>((at / 2) & 0x0F), static_cast<uint8_t>((at / 2) >> 4)};
    const size_t n = std::min(kBurst, kBmi270ConfigSize - at);
    if (!write_(kInitAddr0, addr, sizeof(addr)) || !write_(kInitData, kBmi270Config + at, n)) return false;
  }
  if (!write8(kInitCtrl, 0x01)) return false;
  uint8_t status = 0;
  for (int i = 0; i < kInitPolls && (status & 0x0F) != kInitOk; ++i) {
    delay_ms_(kInitPollMs);
    if (!read_(kInternalStatus, &status, 1)) status = 0;
  }
  if ((status & 0x0F) != kInitOk) return false;
  // Each setting is read back: a write that did not take leaves the chip unconfigured.
  const struct {
    uint8_t reg, value;
  } kConfig[] = {{kAccConf, kAccConfValue}, {kAccRange, kAccRangeValue}, {kPwrCtrl, kPwrCtrlValue}};
  for (const auto& c : kConfig) {
    uint8_t got = 0;
    if (!write8(c.reg, c.value) || !read_(c.reg, &got, 1) || got != c.value) return false;
  }
  return true;
}

bool Bmi270::read(int16_t mg[3]) {
  uint8_t raw[6];
  if (!read_(kAccelBase, raw, sizeof(raw))) return false;
  for (int i = 0; i < 3; ++i) {
    const auto v = static_cast<int16_t>(static_cast<uint16_t>(raw[2 * i]) |
                                        (static_cast<uint16_t>(raw[2 * i + 1]) << 8));
    mg[i] = static_cast<int16_t>(static_cast<int32_t>(v) * 1000 / kLsbPerG);
  }
  return true;
}

}  // namespace hg
