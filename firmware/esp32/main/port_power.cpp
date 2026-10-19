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

std::optional<hg::PowerStatus> Stc8Power::read() {
  const auto status = chip_.read();
  const bool read_ok = status.has_value();
  if (!read_result_seen_ || read_ok != last_read_ok_) {
    if (read_ok) ESP_LOGI("hg.power", "STC8 battery sample available");
    else ESP_LOGW("hg.power", "STC8 battery sample read failed");
  }
  read_result_seen_ = true;
  last_read_ok_ = read_ok;
  return status;
}

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

void AxpPower::use_power_key() {
  key_ = chip_ && chip_->enable_key_press();
  if (!key_) ESP_LOGW("hg.power", "can't enable the PWR key's short press on the AXP2101");
}

}  // namespace hgp
