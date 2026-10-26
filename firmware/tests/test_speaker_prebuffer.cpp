#include <cstdint>

#include "check.hpp"
#include "speaker_prebuffer.hpp"

namespace {
constexpr size_t kBytes = 24000;  // 500 ms at 24 kHz mono PCM16
constexpr uint32_t kWait = 1000;
}  // namespace

TEST("speaker prebuffer: a stream starts once enough is buffered, not on its first frame") {
  hg::SpeakerPrebuffer p(kBytes, kWait);
  CHECK(!p.ready(0, true, 0));       // nothing yet
  CHECK(!p.ready(1920, true, 10));   // one 40 ms frame
  CHECK(!p.ready(kBytes - 1, true, 400));
  CHECK(p.ready(kBytes, true, 450));
  CHECK(p.ready(0, true, 500));      // once started, a dry queue is an underrun, not a new wait
}

TEST("speaker prebuffer: a short reply that has closed plays at once") {
  hg::SpeakerPrebuffer p(kBytes, kWait);
  CHECK(p.ready(4800, false, 0));
}

TEST("speaker prebuffer: a slow stream starts after the longest wait, counted from its first audio") {
  hg::SpeakerPrebuffer p(kBytes, kWait);
  CHECK(!p.ready(0, true, 0));
  CHECK(!p.ready(0, true, 5000));          // time with nothing buffered doesn't count
  CHECK(!p.ready(1920, true, 5100));        // the wait starts here
  CHECK(!p.ready(3840, true, 5100 + kWait - 1));
  CHECK(p.ready(3840, true, 5100 + kWait));
}

TEST("speaker prebuffer: each stream waits again after reset") {
  hg::SpeakerPrebuffer p(kBytes, kWait);
  CHECK(p.ready(kBytes, true, 0));
  p.reset();
  CHECK(!p.ready(1920, true, 100));
  CHECK(!p.ready(1920, true, 100 + kWait - 1));  // the old stream's wait doesn't carry over
  CHECK(p.ready(1920, true, 100 + kWait));
}
