// When a speaker starts playing a stream, apart from FreeRTOS so it can be tested on
// the host (port_codec.cpp). Playing from the first frame leaves no margin: a reply
// whose next frames are a little late stutters in its first second, before the
// server's lead (docs/protocol.md#audio-to-the-device) has built up in the buffer.
// So the speaker waits until `bytes` are buffered, then plays. It never waits on a
// stream that has closed (a short reply plays at once), nor longer than `max_wait_ms`
// after its first audio arrived (a slow stream still starts). Only the start of a
// stream waits; a gap later on resumes as soon as audio arrives.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hg {

class SpeakerPrebuffer {
 public:
  SpeakerPrebuffer(size_t bytes, uint32_t max_wait_ms) : bytes_(bytes), max_wait_ms_(max_wait_ms) {}

  // Whether to start playing now. Call while not playing, with what the queue holds and
  // whether the stream is still open; once it says yes it keeps saying yes until reset().
  bool ready(size_t buffered, bool open, uint32_t now) {
    if (started_) return true;
    if (!buffered) return false;  // nothing to play yet, and the wait hasn't begun
    if (!waiting_) {
      waiting_ = true;
      since_ = now;
    }
    started_ = !open || buffered >= bytes_ || now - since_ >= max_wait_ms_;
    return started_;
  }
  // The stream ended or was aborted: the next one waits again.
  void reset() { started_ = waiting_ = false; }

 private:
  size_t bytes_;
  uint32_t max_wait_ms_;
  bool started_ = false, waiting_ = false;
  uint32_t since_ = 0;
};

}  // namespace hg
