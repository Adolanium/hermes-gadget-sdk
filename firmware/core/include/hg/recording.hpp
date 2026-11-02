// A recording kept whole on the device while it is uploaded (hg::App, "Recording
// buffer" in docs/architecture.md). The microphone's PCM goes here first and the
// upload sends from here as the link takes it, so a slow link delays audio
// instead of dropping it, and a connection lost mid-recording leaves all of it to
// send again once the device is back online. One buffer, set aside at start-up
// (PSRAM on hardware) and reused for every recording.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace hg {

class RecordingBuffer {
 public:
  // Bytes for `max_ms` of 16-bit mono PCM at `rate`, plus one second for the samples
  // that arrive between the limit and the tick that ends the recording. Raising the
  // utterance limit is a change to `max_ms` alone.
  static constexpr size_t bytes_for(uint32_t max_ms, uint32_t rate) {
    return (static_cast<size_t>(max_ms) + 1000) * rate / 1000 * 2;
  }

  // PSRAM that must stay free after the buffer is set aside: faces, WebSocket send copies and
  // event payloads prefer PSRAM and fall back to internal RAM, so a small PSRAM part keeps the
  // buffer off rather than push them there (main.cpp's recording_alloc).
  static constexpr size_t kPsramHeadroom = 1024 * 1024;
  static constexpr bool leaves_headroom(size_t free_bytes, size_t bytes) {
    return free_bytes >= bytes && free_bytes - bytes >= kPsramHeadroom;
  }

  // Sets aside `bytes` with `alloc` (released with std::free); false leaves the
  // buffer off, and the caller streams as before.
  bool reserve(void* (*alloc)(size_t), size_t bytes) {
    data_.reset();
    cap_ = 0;
    clear();
    if (!alloc || !bytes) return false;
    data_.reset(static_cast<uint8_t*>(alloc(bytes)));
    cap_ = data_ ? bytes : 0;
    return data_ != nullptr;
  }
  bool ready() const { return data_ != nullptr; }
  size_t capacity() const { return cap_; }

  void clear() {
    len_ = 0;
    sent_ = 0;
  }
  // Appends whole samples; what doesn't fit is dropped. Returns the samples kept.
  size_t append(const int16_t* samples, size_t count) {
    const size_t room = (cap_ - len_) / 2;
    const size_t n = count < room ? count : room;
    if (n) std::memcpy(data_.get() + len_, samples, n * 2);
    len_ += n * 2;
    return n;
  }
  size_t size() const { return len_; }
  size_t sent() const { return sent_; }
  size_t unsent() const { return len_ - sent_; }
  const uint8_t* next() const { return data_.get() + sent_; }
  void advance(size_t bytes) { sent_ += bytes < unsent() ? bytes : unsent(); }
  // The connection that carried the upload is gone: everything goes again.
  void rewind() { sent_ = 0; }

 private:
  struct Free {
    void operator()(uint8_t* p) const { std::free(p); }
  };
  std::unique_ptr<uint8_t, Free> data_;
  size_t cap_ = 0, len_ = 0, sent_ = 0;
};

}  // namespace hg
