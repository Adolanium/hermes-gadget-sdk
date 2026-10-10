#include <cstdint>
#include <vector>

#include "check.hpp"
#include "wifi_retry.hpp"

namespace {

// Fails every attempt on battery and records the waits between them.
std::vector<uint32_t> waits_on_battery(hg::WifiRetry& r, uint32_t& now, int attempts) {
  std::vector<uint32_t> waits;
  for (int i = 0; i < attempts; ++i) {
    r.lost(now, true);
    const uint32_t from = now;
    while (!r.due(now)) now += 100;
    waits.push_back(now - from);
    now += 500;  // the attempt runs, then fails
  }
  return waits;
}

}  // namespace

TEST("wifi retry: on battery the wait doubles from 3 s up to 30 s, like Hermes") {
  hg::WifiRetry r;
  uint32_t now = 1000;
  r.started(now);
  CHECK(waits_on_battery(r, now, 7) == std::vector<uint32_t>({3000, 6000, 12000, 24000, 30000, 30000, 30000}));
}

TEST("wifi retry: plugged in it keeps retrying every 3 s") {
  hg::WifiRetry r;
  uint32_t now = 1000;
  for (int i = 0; i < 5; ++i) {
    r.lost(now, false);
    CHECK_EQ(r.retry_at(), now + 3000);
    now += 3000;
    CHECK(r.due(now));
    now += 500;
  }
}

TEST("wifi retry: a connection resets the wait") {
  hg::WifiRetry r;
  uint32_t now = 1000;
  waits_on_battery(r, now, 4);
  r.connected();
  CHECK(!r.attempting());
  r.lost(now, true);
  CHECK_EQ(r.retry_at(), now + 3000);
}

TEST("wifi retry: waking the screen or plugging in retries at once") {
  hg::WifiRetry r;
  uint32_t now = 1000;
  waits_on_battery(r, now, 5);
  r.lost(now, true);
  CHECK(!r.due(now + 1000));  // 30 s away
  r.retry_now(now + 1000);
  CHECK(r.due(now + 1000));
}

TEST("wifi retry: nothing scheduled, nothing to hurry") {
  hg::WifiRetry r;
  r.retry_now(5000);
  CHECK(!r.due(5000));
  r.started(5000);
  r.retry_now(5100);  // an attempt is already running
  CHECK(!r.due(5100));
  CHECK(r.attempting());
}

TEST("wifi retry: the chip is held awake only while an attempt runs") {
  hg::WifiRetry r;
  uint32_t now = 1000;
  r.started(now);
  CHECK(r.attempting());
  r.lost(now + 400, true);  // failed: may doze until the next slot
  CHECK(!r.attempting());
  CHECK(r.due(now + 400 + 3000));
  CHECK(r.attempting());
  r.connected();
  CHECK(!r.attempting());
}

TEST("wifi retry: an attempt that never answers is abandoned after 20 s") {
  hg::WifiRetry r;
  r.started(1000);
  CHECK(!r.due(1000 + 19900));
  CHECK(r.due(1000 + 20000));  // timed out, and retried at once
  CHECK(r.attempting());
}

TEST("wifi retry: a start that fails waits 3 s, without holding the chip awake") {
  hg::WifiRetry r;
  r.lost(1000, true);
  CHECK(r.due(4000));
  r.start_failed(4000);
  CHECK(!r.attempting());
  CHECK_EQ(r.retry_at(), 7000u);
}
