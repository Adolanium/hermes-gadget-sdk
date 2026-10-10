// When to retry joining Wi-Fi, apart from ESP-IDF so it can be tested on the
// host (firmware/esp32/main/port_wifi.cpp). On battery the wait doubles after
// each failed attempt, up to 30 s like the Hermes reconnect (app.cpp kBackoffMs),
// so a long stretch without coverage costs little. Plugged in it stays at 3 s.
// While an attempt runs the chip is held awake (light sleep would stall it);
// between attempts it may doze.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hg {

class WifiRetry {
 public:
  static constexpr uint32_t kDelaysMs[] = {3000, 6000, 12000, 24000, 30000};
  static constexpr size_t kSteps = sizeof(kDelaysMs) / sizeof(kDelaysMs[0]);
  // An attempt that never reports back is treated as failed after this long.
  static constexpr uint32_t kAttemptTimeoutMs = 20000;

  // Joining has started (credentials set, first attempt made by the caller).
  void started(uint32_t now) {
    attempting_ = true;
    attempt_at_ = now;
    retry_at_ = 0;
    failures_ = 0;
  }
  // The link is up: back to the shortest wait for the next loss.
  void connected() {
    attempting_ = false;
    retry_at_ = 0;
    failures_ = 0;
  }
  // The link went down, or an attempt failed (one Wi-Fi disconnect event each).
  void lost(uint32_t now, bool on_battery) {
    attempting_ = false;
    const size_t step = on_battery ? (failures_ < kSteps ? failures_ : kSteps - 1) : 0;
    retry_at_ = now + kDelaysMs[step];
    if (!retry_at_) retry_at_ = 1;  // 0 means "nothing scheduled"
    if (on_battery && failures_ < kSteps) ++failures_;
  }
  // The user is back (screen woken, USB plugged in): try now, not at the next slot.
  void retry_now(uint32_t now) {
    if (!retry_at_) return;
    retry_at_ = now ? now : 1;
  }
  // True when the caller should start an attempt now; the attempt holds the chip awake.
  bool due(uint32_t now) {
    if (attempting_ && static_cast<int32_t>(now - attempt_at_) >= static_cast<int32_t>(kAttemptTimeoutMs)) {
      attempting_ = false;  // gave no answer: count it as a failure, start over
      retry_at_ = now ? now : 1;
    }
    if (attempting_ || !retry_at_ || static_cast<int32_t>(now - retry_at_) < 0) return false;
    attempting_ = true;
    attempt_at_ = now;
    retry_at_ = 0;
    return true;
  }
  // The attempt couldn't even start: try again after the shortest wait.
  void start_failed(uint32_t now) {
    attempting_ = false;
    retry_at_ = now + kDelaysMs[0];
  }
  bool attempting() const { return attempting_; }
  uint32_t retry_at() const { return retry_at_; }

 private:
  bool attempting_ = false;
  uint32_t attempt_at_ = 0, retry_at_ = 0;
  size_t failures_ = 0;
};

}  // namespace hg
