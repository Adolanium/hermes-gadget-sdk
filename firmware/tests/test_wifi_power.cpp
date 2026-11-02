#include <cstdint>
#include <optional>

#include "check.hpp"
#include "wifi_power.hpp"

namespace {

using hg::WifiSleep;
using Want = std::optional<WifiSleep>;

// Applies whatever the rule asks for, like Wifi::apply_power_save, and returns it.
Want step(hg::WifiPowerSave& p, uint32_t now, bool on_battery, bool in_turn, int* reads = nullptr,
          bool dozing = false, uint32_t traffic_at = 0) {
  const auto want = p.change(now, [&] {
    if (reads) ++*reads;
    return on_battery;
  }, in_turn, dozing, traffic_at);
  if (want) p.applied(*want);
  return want;
}

Want doze(hg::WifiPowerSave& p, uint32_t now, bool on_battery, bool dozing, uint32_t traffic_at = 0,
          bool in_turn = false) {
  return step(p, now, on_battery, in_turn, nullptr, dozing, traffic_at);
}

}  // namespace

TEST("wifi power save: modem sleep only on battery, and full power when plugged in") {
  hg::WifiPowerSave plugged;
  CHECK(step(plugged, 0, false, false) == Want(WifiSleep::None));  // the first call always applies
  CHECK(!step(plugged, 10, false, false));                         // then nothing until it changes
  hg::WifiPowerSave battery;
  CHECK(step(battery, 0, true, false) == Want(WifiSleep::Min));
  CHECK(!step(battery, 10, true, false));
}

TEST("wifi power save: a turn wakes the radio at once and lets it sleep again when it ends") {
  hg::WifiPowerSave p;
  CHECK(step(p, 0, true, false) == Want(WifiSleep::Min));
  // Well inside the 5 s battery check: the turn still applies immediately.
  CHECK(step(p, 100, true, true) == Want(WifiSleep::None));
  CHECK(!step(p, 200, true, true));
  CHECK(step(p, 300, true, false) == Want(WifiSleep::Min));
}

TEST("wifi power save: the battery is read every 5 s, and unplugging applies at the next read") {
  hg::WifiPowerSave p;
  int reads = 0;
  step(p, 0, true, false, &reads);
  for (uint32_t t = 10; t < hg::WifiPowerSave::kBatteryCheckMs; t += 10) step(p, t, true, t % 20 == 0, &reads);
  CHECK_EQ(reads, 1);
  // Plugged in now: the reading only changes at the next check.
  CHECK(!step(p, hg::WifiPowerSave::kBatteryCheckMs - 10, false, false, &reads));
  CHECK(step(p, hg::WifiPowerSave::kBatteryCheckMs, false, false, &reads) == Want(WifiSleep::None));
  CHECK_EQ(reads, 2);
}

TEST("wifi power save: a switch that failed is asked for again") {
  hg::WifiPowerSave p;
  CHECK(p.change(0, [] { return true; }, false) == Want(WifiSleep::Min));  // not applied
  CHECK(p.change(10, [] { return true; }, false) == Want(WifiSleep::Min));
  p.applied(WifiSleep::Min);
  CHECK(!p.change(20, [] { return true; }, false));
  CHECK(p.current() == Want(WifiSleep::Min));
}

TEST("wifi power save: dozing quietly on battery sleeps deeper, and the screen lighting undoes it at once") {
  constexpr uint32_t q = hg::WifiPowerSave::kQuietMs;
  hg::WifiPowerSave p;
  CHECK(doze(p, 1000, true, false) == Want(WifiSleep::Min));
  // Nothing from Hermes yet: max at once.
  CHECK(doze(p, 1100, true, true) == Want(WifiSleep::Max));
  CHECK(!doze(p, 1200, true, true));
  CHECK(doze(p, 1300, true, false) == Want(WifiSleep::Min));  // the screen lit
  // A frame from Hermes a moment ago: max only once it has been quiet for kQuietMs.
  CHECK(!doze(p, 2000, true, true, 1500));
  CHECK(!doze(p, 1500 + q - 1, true, true, 1500));
  CHECK(doze(p, 1500 + q, true, true, 1500) == Want(WifiSleep::Max));
  // Another frame while dozing: back to min at once, then max again after the quiet.
  CHECK(doze(p, 30000, true, true, 30000) == Want(WifiSleep::Min));
  CHECK(doze(p, 30000 + q, true, true, 30000) == Want(WifiSleep::Max));
}

TEST("wifi power save: a turn or USB beats dozing") {
  hg::WifiPowerSave p;
  CHECK(doze(p, 0, true, true) == Want(WifiSleep::Max));
  CHECK(doze(p, 10, true, true, 0, true) == Want(WifiSleep::None));  // a turn: full power
  CHECK(doze(p, 20, true, true) == Want(WifiSleep::Max));
  hg::WifiPowerSave plugged;
  CHECK(doze(plugged, 0, false, true) == Want(WifiSleep::None));
}

TEST("wifi power save: the quiet time survives the clock wrapping") {
  constexpr uint32_t q = hg::WifiPowerSave::kQuietMs;
  const uint32_t frame = UINT32_MAX - 1000;
  hg::WifiPowerSave p;
  CHECK(doze(p, frame + 10, true, true, frame) == Want(WifiSleep::Min));
  CHECK(doze(p, frame + q, true, true, frame) == Want(WifiSleep::Max));  // past the wrap
}
