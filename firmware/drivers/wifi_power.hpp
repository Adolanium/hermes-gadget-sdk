// How deeply Wi-Fi may sleep, apart from ESP-IDF so it can be tested on the host
// (firmware/esp32/main/port_wifi.cpp). With modem sleep the access point holds
// downlink frames until the station wakes, so audio arrives in bursts that leave
// the speaker short and then overflow it. Modem sleep stays on only on battery and
// outside a turn. The battery reading is refreshed every 5 s; a turn starting or
// ending applies at once, so the radio is fully awake before the reply arrives.
//
// Dozing (screen dark, idle, on battery: App::dozing) with no frame from Hermes
// but its heartbeat ping for kQuietMs, the radio sleeps deeper: it wakes every
// third beacon (kListenInterval, about 300 ms) instead of every DTIM. A frame from
// Hermes then waits at most that long at the access point; the heartbeat (every
// 20 s, dropped after 60 s) is unaffected. The screen lighting, a turn, or any
// other frame from Hermes goes back to minimum modem sleep at once.
#pragma once
#include <cstdint>
#include <optional>

namespace hg {

enum class WifiSleep : uint8_t { None, Min, Max };

inline const char* wifi_sleep_name(WifiSleep s) {
  switch (s) {
    case WifiSleep::None: return "off";
    case WifiSleep::Min: return "min modem";
    case WifiSleep::Max: return "max modem";
  }
  return "?";
}

class WifiPowerSave {
 public:
  static constexpr uint32_t kBatteryCheckMs = 5000;
  // Max modem sleep only after this long without a frame from Hermes but a ping.
  static constexpr uint32_t kQuietMs = 10000;
  // Beacons between wake-ups in max modem sleep; also sent to the access point when joining.
  static constexpr uint8_t kListenInterval = 3;

  // The sleep to switch to, or nothing when the applied one is right. `dozing` is
  // App::dozing(); `traffic_at` App::traffic_at(), the last frame from
  // Hermes other than a ping (0: none yet).
  template <class OnBattery>
  std::optional<WifiSleep> change(uint32_t now, OnBattery&& on_battery, bool in_turn, bool dozing = false,
                                  uint32_t traffic_at = 0) {
    if (!battery_checked_ || now - battery_checked_at_ >= kBatteryCheckMs) {
      on_battery_ = on_battery();
      battery_checked_ = true;
      battery_checked_at_ = now;
    }
    WifiSleep want = WifiSleep::None;
    if (on_battery_ && !in_turn) {
      const bool quiet = traffic_at == 0 || now - traffic_at >= kQuietMs;
      want = dozing && quiet ? WifiSleep::Max : WifiSleep::Min;
    }
    if (applied_ && *applied_ == want) return std::nullopt;
    return want;
  }
  // The caller switched the sleep (only then: a failed switch is retried next time).
  void applied(WifiSleep sleep) { applied_ = sleep; }
  std::optional<WifiSleep> current() const { return applied_; }

 private:
  std::optional<WifiSleep> applied_;
  bool on_battery_ = false, battery_checked_ = false;
  uint32_t battery_checked_at_ = 0;
};

}  // namespace hg
