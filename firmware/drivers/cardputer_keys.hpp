// Cardputer ADV's TCA8418 FIFO -> physical key layout. Hardware facts from
// M5Stack K132-Adv and M5Cardputer bf64d3178ab43928fa02365fbe3937cec99716ad.
// Independent implementation; no Arduino or vendor driver dependency.
#pragma once
#include <cstdint>

namespace hg {
class CardputerKeys {
 public:
  enum class Kind : uint8_t { None, Character, Enter, Backspace, Cancel, Up, Down, Settings, Power, Profile };
  struct Key { Kind kind = Kind::None; char text = 0; };

  /** Clear held modifiers after a FIFO overflow or lost release event. */
  void reset() { held_ = 0; }
  /** Consume one TCA8418 press/release byte and return a new key action.
   * Releases, repeated presses and unwired scan positions produce no action.
   * Modifier state persists until release or reset; text is printed ASCII.
   */
  Key feed(uint8_t event) {
    const int code = event & 0x7f;
    if (code < 1 || code > 68) return {};
    const int scan = code - 1, row = scan / 10, col = scan % 10;
    if (row >= 7 || col >= 8) return {};  // ignore unwired/GPI events
    const int key = (col % 4) * 14 + row * 2 + col / 4;
    const uint64_t mask = uint64_t{1} << key;
    const bool down = (event & 0x80) != 0;
    if (!down) { held_ &= ~mask; return {}; }
    if (held_ & mask) return {};  // duplicate press, not another character
    held_ |= mask;
    const bool fn = held_ & (uint64_t{1} << 28);
    const bool shift = held_ & (uint64_t{1} << 29);
    const bool ctrl = held_ & (uint64_t{1} << 42);
    if (key == 28 || key == 29 || key == 42 || key == 43 || key == 44) return {};
    if (key == 13) return {Kind::Backspace};
    if (key == 41) return {fn ? Kind::Settings : Kind::Enter};
    if (key == 14) return {Kind::Settings};
    if (fn) {
      if (key == 0) return {Kind::Cancel};
      if (key == 39) return {Kind::Up};
      if (key == 53) return {Kind::Down};
      if (key == 45) return {Kind::Power};  // Fn+Z
      if (key == 24) return {Kind::Profile};  // Fn+P
      return {};
    }
    if (ctrl) return {};  // don't accidentally send a control chord as text
    // Printed legends, indexed by physical row (4) and column (14).
    static constexpr char plain[4][15] = {"`1234567890-=\0", "\0qwertyuiop[]\\", "\0\0asdfghjkl;'\0", "\0\0\0zxcvbnm,./ "};
    static constexpr char upper[4][15] = {"~!@#$%^&*()_+\0", "\0QWERTYUIOP{}|", "\0\0ASDFGHJKL:\"\0", "\0\0\0ZXCVBNM<>? "};
    const char ch = (shift ? upper : plain)[key / 14][key % 14];
    return ch ? Key{Kind::Character, ch} : Key{};
  }
 private:
  uint64_t held_ = 0;
};
}  // namespace hg
