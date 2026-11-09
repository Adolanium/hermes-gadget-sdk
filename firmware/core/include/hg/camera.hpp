// Camera HAL: an optional still camera the device owns (the CoreS3's GC0308).
// The core's camera screen drives it from its render step: draw_preview paints
// the live view into a window at a capped rate, capture grabs a full still in
// the format docs/protocol.md documents for camera results.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hg {

class Camera {
 public:
  virtual ~Camera() = default;
  // True once the port camera is usable (rails on, driver started, sensor seen).
  virtual bool available() const = 0;
  // Paint the live view into a window of the panel at (x, y), w x h pixels,
  // scaling the sensor frame as needed. The pixels go out in the panel's own
  // byte order, so the window shows right whatever info().swap_bytes says. The
  // core calls this from its render step at a capped rate.
  virtual bool draw_preview(uint16_t x, uint16_t y, uint16_t w, uint16_t h) = 0;
  // A full-resolution still: tightly packed little-endian RGB565, no padding —
  // the layout the core draws through a Canvas and docs/protocol.md documents.
  struct Still {
    std::vector<uint8_t> pixels;
    uint16_t width = 0;
    uint16_t height = 0;
  };
  virtual bool capture(Still& out, std::string& error) = 0;
};

// The camera screen's tap-to-capture countdown: tap starts 3 s, it ticks down,
// and it fires once. Pure logic so a host test can drive it without a camera.
class Countdown {
 public:
  void start(uint32_t now) {
    running_ = true;
    fired_ = false;
    start_ = now;
  }
  bool running() const { return running_; }
  bool fired() const { return fired_; }
  // Seconds left on the countdown, 3 when just started, 0 when it has fired.
  int seconds_left(uint32_t now) const {
    if (!running_) return 0;
    const uint32_t elapsed = now - start_;
    if (elapsed >= 3000) return 0;
    return static_cast<int>((3000 - elapsed + 999) / 1000);
  }
  void tick(uint32_t now) {
    if (running_ && now - start_ >= 3000) {
      running_ = false;
      fired_ = true;
    }
  }
  void reset() {
    running_ = false;
    fired_ = false;
  }

 private:
  bool running_ = false;
  bool fired_ = false;
  uint32_t start_ = 0;
};

}  // namespace hg
