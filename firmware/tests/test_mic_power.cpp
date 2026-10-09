#include <cstdint>
#include <string>
#include <vector>

#include "check.hpp"
#include "mic_power.hpp"

namespace {

// Records what the mic task asks of the codec.
struct FakeMic {
  std::vector<std::string> calls;
  bool start_fails = false, read_fails = false;
  int16_t next = 0;  // sample value of the next chunk read

  bool power_up() {
    calls.push_back("power up");
    return !start_fails;
  }
  void power_down() { calls.push_back("power down"); }
  void drain() { calls.push_back("drain"); }
  bool read(int16_t* chunk) {
    if (read_fails) {
      calls.push_back("read failed");
      return false;
    }
    chunk[0] = next++;
    calls.push_back("read");
    return true;
  }
  void deliver(const int16_t* chunk) { calls.push_back("deliver " + std::to_string(chunk[0])); }
  void wait() { calls.push_back("wait"); }
  void pause() { calls.push_back("pause"); }
};

std::vector<std::string> run(hg::MicPower& power, FakeMic& mic, bool capturing, int turns) {
  mic.calls.clear();
  int16_t chunk[4] = {};
  for (int i = 0; i < turns; ++i) power.turn(capturing, mic, chunk);
  return mic.calls;
}

using Calls = std::vector<std::string>;

}  // namespace

TEST("mic power: a recording-only codec switches off while nothing records") {
  hg::MicPower power(true);
  FakeMic mic;
  CHECK(power.on());  // opened at boot
  CHECK(run(power, mic, false, 3) == Calls({"power down", "wait", "wait", "wait"}));
  CHECK(!power.on());
}

TEST("mic power: a capture powers it up, drops stale and settling audio, then delivers") {
  hg::MicPower power(true);
  FakeMic mic;
  run(power, mic, false, 1);
  CHECK(run(power, mic, true, 3) ==
        Calls({"power up", "drain", "read", "read", "deliver 1", "read", "deliver 2"}));
  CHECK(power.on());
  // and off again once the capture ends
  CHECK(run(power, mic, false, 1) == Calls({"power down", "wait"}));
}

TEST("mic power: back-to-back captures don't cycle the codec") {
  hg::MicPower power(true);
  FakeMic mic;
  run(power, mic, false, 1);
  run(power, mic, true, 2);
  CHECK(run(power, mic, true, 2) == Calls({"read", "deliver 2", "read", "deliver 3"}));
}

TEST("mic power: a codec that fails to start is retried after a pause, not read") {
  hg::MicPower power(true);
  FakeMic mic;
  run(power, mic, false, 1);
  mic.start_fails = true;
  CHECK(run(power, mic, true, 2) == Calls({"power up", "pause", "power up", "pause"}));
  CHECK(!power.on());
  mic.start_fails = false;
  CHECK(run(power, mic, true, 1) == Calls({"power up", "drain", "read"}));
}

TEST("mic power: a read error backs off instead of spinning") {
  hg::MicPower power(true);
  FakeMic mic;
  mic.read_fails = true;
  CHECK(run(power, mic, true, 2) == Calls({"read failed", "pause", "read failed", "pause"}));
}

TEST("mic power: a codec shared with the speaker stays on and keeps reading, delivering only captures") {
  hg::MicPower power(false);
  FakeMic mic;
  CHECK(run(power, mic, false, 2) == Calls({"read", "read"}));
  CHECK(power.on());
  CHECK(run(power, mic, true, 1) == Calls({"read", "deliver 2"}));
}
