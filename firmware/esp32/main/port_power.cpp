#include "port.hpp"

#include "esp_log.h"

namespace hgp {

bool AxpPower::begin(i2c_master_bus_handle_t bus) {
  if (!bus || i2c_master_probe(bus, 0x34, 50) != ESP_OK) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = 0x34;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) return false;
  chip_ = std::make_unique<hg::Axp2101>(
      [this](uint8_t reg, uint8_t* data, size_t size) {
        return i2c_master_transmit_receive(dev_, &reg, 1, data, size, 50) == ESP_OK;
      },
      [this](uint8_t reg, uint8_t value) {
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(dev_, data, sizeof(data), 50) == ESP_OK;
      });
  if (!chip_->read()) {
    chip_.reset();
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
    return false;
  }
  ESP_LOGI("hg.power", "AXP2101 reporting ready; charging and supply settings unchanged");
  return true;
}

}  // namespace hgp
