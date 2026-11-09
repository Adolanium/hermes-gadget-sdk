// microSD log mirror: a copy of the device log on /sdcard/hermes.log.
//
// esp_log_set_vprintf is chained (diag capture first, this second), so the
// console keeps its output. The hook itself only copies formatted lines into
// a RAM ring — never touches the card — and a task drains the ring to the
// file. Mount is retried until a FAT32 card appears, so logging starts when
// a card is inserted, on by default (the sd_log setting).
#pragma once

#include <cstdarg>
#include <cstdio>

#include "board.hpp"  // SdConfig

namespace hgp {

class SdLogger {
 public:
  // Mount the slot (if the board has one) and start capturing. `enabled` is
  // the sd_log setting; logging runs unless it is off.
  void begin(const SdConfig& sd, bool enabled);
  // The sd_log setting changed: stop/start mirroring. The card stays mounted.
  void set_enabled(bool enabled);

 private:
  void mount();
  void open_file();
  void flush();
  static int hook(const char* fmt, va_list args);
  static void task(void* arg);

  SdConfig sd_;
  bool mounted_ = false;
  FILE* file_ = nullptr;
  size_t written_ = 0;  // bytes in the current file, for rotation mid-session
};

}  // namespace hgp
