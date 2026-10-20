// CrowPanel P4 power adapter for its STC8 battery companion. The driver maps
// only the existing Gadget PowerStatus fields.
#include "port.hpp"

#include "esp_log.h"

namespace hgp {

Stc8Power::Stc8Power()
    : chip_([this](uint8_t reg, uint8_t& value) {
        if (!dev_) return false;
        return i2c_master_transmit_receive(dev_, &reg, 1, &value, 1, 50) == ESP_OK;
      }) {}

bool Stc8Power::begin(i2c_master_bus_handle_t bus) {
  if (!bus) {
    ESP_LOGE("hg.power", "STC8 battery adapter has no I2C bus");
    return false;
  }
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = hg::Stc8Battery::kI2cAddress;
  cfg.scl_speed_hz = 400000;
  const esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev_);
  if (err != ESP_OK) {
    ESP_LOGE("hg.power", "STC8 battery registration failed: %s", esp_err_to_name(err));
    dev_ = nullptr;
    return false;
  }
  ESP_LOGI("hg.power", "STC8 battery interface registered at 0x2F, 400 kHz");
  return true;
}

std::optional<hg::PowerStatus> Stc8Power::read() { return chip_.read(); }

}  // namespace hgp
