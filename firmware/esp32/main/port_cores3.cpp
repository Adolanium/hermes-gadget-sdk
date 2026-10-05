#include "port.hpp"

#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {

bool CoreS3Board::begin(i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  for (const uint8_t address : {uint8_t(0x34), uint8_t(0x58)}) {
    if (i2c_master_probe(bus, address, 50) != ESP_OK) return false;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = address;
    cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &cfg, address == 0x34 ? &pmic_ : &expander_) != ESP_OK) return false;
  }
  control_ = std::make_unique<hg::CoreS3Control>(
      [this](uint8_t address, uint8_t reg, uint8_t& value) {
        return i2c_master_transmit_receive(address == 0x34 ? pmic_ : expander_, &reg, 1, &value, 1, 50) == ESP_OK;
      },
      [this](uint8_t address, uint8_t reg, uint8_t value) {
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(address == 0x34 ? pmic_ : expander_, data, sizeof(data), 50) == ESP_OK;
      },
      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });
  if (control_->begin()) return true;
  control_.reset();
  ESP_LOGE("hg.cores3", "power/reset initialization failed");
  return false;
}

void CoreS3Board::set_backlight(uint8_t percent) {
  if (control_ && !control_->set_brightness(percent)) ESP_LOGW("hg.cores3", "backlight update failed");
}

}  // namespace hgp
