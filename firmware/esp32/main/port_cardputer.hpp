#pragma once
#include "port.hpp"
#if CONFIG_HG_BOARD_CARDPUTER_ADV
#include "cardputer_keys.hpp"
#include "cardputer_compose.hpp"

namespace hgp {
class CardputerKeyboard {
 public:
  bool begin(i2c_master_bus_handle_t bus);
  void poll(hg::App& app, hg::CardputerCompose& display);
  bool ready() const { return dev_ != nullptr; }
 private:
  bool read(uint8_t reg, uint8_t& value);
  bool write(uint8_t reg, uint8_t value);
  bool flush();
  void recover();
  i2c_master_dev_handle_t dev_ = nullptr;
  hg::CardputerKeys keys_;
  uint32_t last_poll_ = 0;
};
}  // namespace hgp
#endif
