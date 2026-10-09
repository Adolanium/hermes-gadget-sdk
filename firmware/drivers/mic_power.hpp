// When the microphone ADC is powered, apart from ESP-IDF so it can be tested on
// the host (firmware/esp32/main/port_codec.cpp drives it from the mic task).
//
// A codec that only records (an ES7210 next to a separate DAC) is switched off
// while nothing records: it otherwise runs, and keeps the CPU busy reading it,
// around the clock. A codec shared with the speaker stays on, as before.
#pragma once
#include <cstdint>

namespace hg {

class MicPower {
 public:
  // Chunks dropped after power-up while the ADC settles (20 ms each).
  static constexpr int kSettleChunks = 1;

  // The codec starts open (CodecAudio::begin opens it), so on.
  explicit MicPower(bool switchable) : switchable_(switchable) {}
  bool on() const { return on_; }

  // One turn of the mic task. Ops supplies:
  //   bool power_up()               start the ADC and restore its gain; false on failure
  //   void power_down()
  //   void drain()                  drop audio buffered while the ADC was off
  //   bool read(int16_t* chunk)     one chunk; false on a read error
  //   void deliver(const int16_t* chunk)
  //   void wait()                   block until a capture may be wanted
  //   void pause()                  back off after an error
  template <class Ops>
  void turn(bool capturing, Ops& ops, int16_t* chunk) {
    if (switchable_ && !capturing) {
      if (on_) {
        ops.power_down();
        on_ = false;
      }
      ops.wait();
      return;
    }
    if (!on_) {
      if (!ops.power_up()) {
        ops.pause();
        return;
      }
      on_ = true;
      ops.drain();
      discard_ = kSettleChunks;
    }
    if (!ops.read(chunk)) {
      ops.pause();
      return;
    }
    if (discard_ > 0) {
      --discard_;
      return;
    }
    // A shared codec keeps reading while idle so a capture starts with fresh
    // samples; only a capture is delivered.
    if (capturing) ops.deliver(chunk);
  }

 private:
  bool switchable_;
  bool on_ = true;
  int discard_ = 0;
};

}  // namespace hg
