// Copyright (c) 2021 M5Stack
// SPDX-License-Identifier: MIT
// CoreS3 probe adapted from M5GFX 28d47949bdc84f30881623476f555398e2194313:
// src/board_detect/m5/board_registry.inl and probe_hardware.inl.
// Changes: bounded CoreS3-only GPIO interface, retain known touch IDs, and
// reject unknown LCD signatures instead of assuming ILI9342C.
#pragma once

#include <cstdint>

namespace hg {
enum class CoreS3Panel { Unknown, C, E };

// The touch ID is only a hint. For unknown firmware, confirm LCD register
// read-back before choosing either initialization table. Register probes follow
// M5GFX 28d47949bdc84f30881623476f555398e2194313, board_registry.inl.
// Unlike M5GFX, no response remains Unknown rather than assuming C.
// IO supplies command writes and indexed parameter reads (7-bit values).
template <class IO>
CoreS3Panel cores3_panel(uint8_t touch_firmware, IO& io) {
  if (touch_firmware == 0x10) return CoreS3Panel::C;
  if (touch_firmware == 0x12) return CoreS3Panel::E;
  const uint8_t zero = 0, one = 1, cb = 0x1c;
  uint8_t dd_read = 0, cb_read = 0, id_high = 0, id_low = 0;
  const bool e_read = io.write(0xd9, &zero, 1) && io.write(0xdd, &one, 1) &&
      io.write(0xcb, &cb, 1) && io.read(0xdd, 1, dd_read) && io.read(0xcb, 1, cb_read);
  if (!io.write(0xd9, &zero, 1)) return CoreS3Panel::Unknown;
  if (e_read && dd_read == 1 && cb_read == 0x1c) return CoreS3Panel::E;
  const uint8_t c_key[] = {0xff, 0x93, 0x42};
  const bool c_read = io.write(0xc8, c_key, sizeof(c_key)) &&
      io.read(0xd3, 2, id_high) && io.read(0xd3, 3, id_low);
  if (!io.write(0xd9, &zero, 1)) return CoreS3Panel::Unknown;
  if (c_read && id_high == (0x93 & 0x7f) && id_low == 0x42) return CoreS3Panel::C;
  return CoreS3Panel::Unknown;
}
// A short mode-0, 3-wire probe before the hardware SPI bus takes ownership.
// M5GFX samples SDA during SCLK-low: this panel releases SDA at the rising
// edge. One dummy clock and the final right shift match its indexed reads.
template <class GPIO>
class CoreS3PanelWire {
 public:
  explicit CoreS3PanelWire(GPIO& gpio) : gpio_(gpio) {}
  bool write(uint8_t cmd, const uint8_t* data, unsigned size) {
    gpio_.input(false);
    gpio_.cs(false);
    gpio_.dc(false);
    send(cmd);
    gpio_.dc(true);
    for (unsigned i = 0; i < size; ++i) send(data[i]);
    gpio_.dc(false);
    send(0);  // M5GFX closes probe writes with NOP before raising CS.
    gpio_.cs(true);
    return true;
  }
  bool read(uint8_t cmd, uint8_t index, uint8_t& value) {
    const uint8_t select = 0x10 | index;
    write(0xd9, &select, 1);
    gpio_.cs(false);
    gpio_.dc(false);
    send(cmd);
    gpio_.dc(true);
    gpio_.input(true);
    clock();  // dummy bit
    uint8_t raw = 0;
    for (int bit = 0; bit < 8; ++bit) {
      gpio_.delay();
      raw = static_cast<uint8_t>((raw << 1) | gpio_.sample());
      gpio_.sclk(true);
      gpio_.delay();
      gpio_.sclk(false);
    }
    gpio_.cs(true);  // deselect before taking the shared SDA line back
    gpio_.data(false);
    gpio_.input(false);
    gpio_.dc(false);
    send(0);  // M5GFX clocks the read's closing NOP with CS already high.
    value = (raw >> 1) & 0x7f;
    if (cmd == 0xdd) dd = value;
    if (cmd == 0xcb) cb = value;
    if (cmd == 0xd3 && index == 2) id_high = value;
    if (cmd == 0xd3 && index == 3) id_low = value;
    return true;
  }
  uint8_t dd = 0, cb = 0, id_high = 0, id_low = 0;

 private:
  void clock() {
    gpio_.delay();
    gpio_.sclk(true);
    gpio_.delay();
    gpio_.sclk(false);
    gpio_.delay();
  }
  void send(uint8_t value) {
    for (int bit = 7; bit >= 0; --bit) {
      gpio_.data((value >> bit) & 1);
      clock();
    }
  }
  GPIO& gpio_;
};
}  // namespace hg
