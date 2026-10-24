#include "qmi8658.hpp"

namespace hg {
namespace {

constexpr uint8_t kWhoAmI = 0x00, kWhoAmIValue = 0x05;
constexpr uint8_t kCtrl1 = 0x02, kCtrl2 = 0x03, kCtrl7 = 0x08, kReset = 0x60;
constexpr uint8_t kResetCmd = 0xB0, kCtrl1Value = 0x40, kCtrl2Value = 0x1D, kCtrl7Value = 0x01;
constexpr uint32_t kResetBootMs = 15;  // datasheet maximum, waited out by the port
constexpr uint8_t kAccelBase = 0x35;   // AX_L .. AZ_H
constexpr int32_t kLsbPerG = 8192;     // at +-4 g

}  // namespace

bool Qmi8658::begin() {
  uint8_t id = 0;
  if (!read_(kWhoAmI, &id, 1) || id != kWhoAmIValue) return false;
  if (!write_(kReset, kResetCmd)) return false;
  delay_ms_(kResetBootMs);
  // Each setting is read back: a write that did not take leaves the chip unconfigured.
  const struct {
    uint8_t reg, value;
  } kConfig[] = {{kCtrl1, kCtrl1Value}, {kCtrl2, kCtrl2Value}, {kCtrl7, kCtrl7Value}};
  for (const auto& c : kConfig) {
    uint8_t got = 0;
    if (!write_(c.reg, c.value) || !read_(c.reg, &got, 1) || got != c.value) return false;
  }
  return true;
}

bool Qmi8658::read(int16_t mg[3]) {
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
