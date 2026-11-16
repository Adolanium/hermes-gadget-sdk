// The log copy `diag log` reads: a ring of the last few KB of log text that a restart
// leaves in place, so the next boot can report what led up to a panic or a watchdog.
// Apart from ESP-IDF so it can be tested on the host (firmware/esp32/main/port_diag.cpp
// keeps one in RAM that startup does not clear).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace hg {

// No constructor and no member initializers: a ring in RAM that startup leaves alone must
// not be written by C++ static initialization either. Call adopt() once at boot.
template <size_t N>
struct LogRing {
  static_assert(N >= 16, "a ring this small holds no useful line");
  static constexpr uint32_t kMagic = 0x48474c47;  // "HGLG"

  uint32_t magic;
  uint32_t head;     // next byte to write
  uint32_t wrapped;  // 1 once the text has gone round
  uint32_t check;    // seal(head, wrapped): a torn header, or RAM after power loss, fails it
  char text[N];

  static constexpr uint32_t seal(uint32_t h, uint32_t w) {
    return kMagic ^ 0x9e3779b9u ^ h ^ (w << 31) ^ static_cast<uint32_t>(N);
  }

  // Whether the header describes text written by a previous boot.
  bool valid() const { return magic == kMagic && head < N && wrapped <= 1 && check == seal(head, wrapped); }

  // At boot: returns the previous boot's text when `kept` (RAM survived the reset) and the
  // header is intact, else empty. Either way the ring starts over empty.
  std::string adopt(bool kept) {
    std::string previous;
    if (kept && valid()) previous = contents();
    magic = kMagic;
    head = 0;
    wrapped = 0;
    check = seal(0, 0);
    return previous;
  }

  void put(char c) {
    text[head] = c;
    if (++head == N) {
      head = 0;
      wrapped = 1;
    }
    check = seal(head, wrapped);
  }

  // The text, oldest first. Once the ring has gone round, the oldest line lost its start
  // and is left out.
  std::string contents() const {
    std::string out;
    if (wrapped) out.assign(text + head, N - head);
    out.append(text, head);
    if (wrapped) {
      const size_t nl = out.find('\n');
      out.erase(0, nl == std::string::npos ? 0 : nl + 1);
    }
    return out;
  }
};

}  // namespace hg
