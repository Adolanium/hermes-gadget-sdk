#include <array>
#include <cstdint>

#include "camera_rgb565.hpp"
#include "check.hpp"
#include "cores3.hpp"

namespace {

// Two fake chips: 256 registers each for the AXP2101 (0x34) and the AW9523B
// (0x58), plus the writes seen so far. Reads keep a sticky high byte so
// read-modify-write sequences round-trip like the real chips do.
class FakeBus {
 public:
  bool read(uint8_t address, uint8_t reg, uint8_t& value) {
    if (address != 0x34 && address != 0x58) return false;
    value = state_[address][reg];
    return true;
  }
  bool write(uint8_t address, uint8_t reg, uint8_t value) {
    if (address != 0x34 && address != 0x58) return false;
    state_[address][reg] = value;
    write_log_.emplace_back(address, reg, value);
    return true;
  }
  const std::vector<std::tuple<uint8_t, uint8_t, uint8_t>>& write_log() const { return write_log_; }
  uint8_t get(uint8_t address, uint8_t reg) const { return state_[address][reg]; }
  void seed(uint8_t address, uint8_t reg, uint8_t value) { state_[address][reg] = value; }

 private:
  std::array<std::array<uint8_t, 256>, 256> state_{};
  std::vector<std::tuple<uint8_t, uint8_t, uint8_t>> write_log_;
};

}  // namespace

TEST("CoreS3 camera power pulses the AW9523B reset and leaves it released") {
  FakeBus bus;
  hg::CoreS3Control control(
      [&](uint8_t a, uint8_t r, uint8_t& v) { return bus.read(a, r, v); },
      [&](uint8_t a, uint8_t r, uint8_t v) { return bus.write(a, r, v); },
      [](uint32_t) {});

  CHECK(control.camera_power(true));

  bool saw_low = false, saw_high = false;
  for (const auto& [address, reg, value] : bus.write_log()) {
    if (address == 0x58 && reg == 0x03) {
      if ((value & 0x01) == 0) saw_low = true;
      if ((value & 0x01) == 0x01) saw_high = true;
    }
  }
  CHECK(saw_low);
  CHECK(saw_high);
  CHECK_EQ(bus.get(0x58, 0x03) & 0x01, uint8_t(0x01));  // reset released at the end
  CHECK_EQ(bus.get(0x58, 0x05) & 0x01, uint8_t(0x00));  // P1_0 an output (0 in the direction register)
}

TEST("CoreS3 camera power preserves the unrelated AW9523B outputs") {
  FakeBus bus;
  bus.seed(0x58, 0x03, 0x82);  // the begin() end state: LCD reset and boost enable high
  bus.seed(0x58, 0x05, 0x7c);  // P1_1 and P1_7 outputs (0), the other P1 pins inputs
  hg::CoreS3Control control(
      [&](uint8_t a, uint8_t r, uint8_t& v) { return bus.read(a, r, v); },
      [&](uint8_t a, uint8_t r, uint8_t v) { return bus.write(a, r, v); },
      [](uint32_t) {});

  CHECK(control.camera_power(true));
  CHECK_EQ(bus.get(0x58, 0x03) & 0x82, uint8_t(0x82));  // bit 7 and bit 1 untouched
  CHECK_EQ(bus.get(0x58, 0x05) & 0xfe, uint8_t(0x7c));  // only P1_0's direction changed
}

TEST("CoreS3 camera power off drives the reset low") {
  FakeBus bus;
  bus.seed(0x58, 0x03, 0x01);
  hg::CoreS3Control control(
      [&](uint8_t a, uint8_t r, uint8_t& v) { return bus.read(a, r, v); },
      [&](uint8_t a, uint8_t r, uint8_t v) { return bus.write(a, r, v); },
      [](uint32_t) {});

  CHECK(control.camera_power(false));
  CHECK_EQ(bus.get(0x58, 0x03) & 0x01, uint8_t(0x00));
}

TEST("CoreS3 camera power turns on the camera's rails and leaves the others") {
  FakeBus bus;
  bus.seed(0x34, 0x90, 0x8b);  // as one board powered up: bits 0, 1, 3 and 7 on; ALDO3, BLDO1/2 off
  bus.seed(0x34, 0x94, 0xe0);  // upper bits are not the voltage code
  hg::CoreS3Control control(
      [&](uint8_t a, uint8_t r, uint8_t& v) { return bus.read(a, r, v); },
      [&](uint8_t a, uint8_t r, uint8_t v) { return bus.write(a, r, v); },
      [](uint32_t) {});

  CHECK(control.camera_power(true));
  CHECK_EQ(bus.get(0x34, 0x90), uint8_t(0x8b | 0x34));  // ALDO3, BLDO1 and BLDO2 on; nothing else changed
  CHECK_EQ(bus.get(0x34, 0x94), uint8_t(0xe0 | 0x1c));  // ALDO3 at 3.3 V
}

TEST("CoreS3 camera rails off takes down only the camera's own rails") {
  FakeBus bus;
  bus.seed(0x34, 0x90, 0x8b | 0x34);  // the amp, the microphones and the camera's rails, as a start leaves them
  bus.seed(0x34, 0x94, 0xe0 | 0x1c);  // ALDO3 at 3.3 V
  hg::CoreS3Control control(
      [&](uint8_t a, uint8_t r, uint8_t& v) { return bus.read(a, r, v); },
      [&](uint8_t a, uint8_t r, uint8_t v) { return bus.write(a, r, v); },
      [](uint32_t) {});

  CHECK(control.camera_rails_off());
  CHECK_EQ(bus.get(0x34, 0x90), uint8_t(0x8b));  // ALDO3, BLDO1 and BLDO2 off; the amp and microphones stay
  CHECK_EQ(bus.get(0x34, 0x94), uint8_t(0xe0));   // ALDO3 at 0 V; the other rail voltage codes untouched
}

TEST("CoreS3 camera power off leaves the rails alone") {
  FakeBus bus;
  bus.seed(0x34, 0x90, 0x8b);
  hg::CoreS3Control control(
      [&](uint8_t a, uint8_t r, uint8_t& v) { return bus.read(a, r, v); },
      [&](uint8_t a, uint8_t r, uint8_t v) { return bus.write(a, r, v); },
      [](uint32_t) {});

  CHECK(control.camera_power(false));
  CHECK_EQ(bus.get(0x34, 0x90), uint8_t(0x8b));
}
