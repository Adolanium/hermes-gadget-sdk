#include "port.hpp"

#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {

bool StickS3Board::begin(i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  constexpr uint8_t kPm1 = 0x6E;
  if (i2c_master_probe(bus, kPm1, 50) != ESP_OK) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = kPm1;
  cfg.scl_speed_hz = 100000;  // the vendor's PM1 clock
  if (i2c_master_bus_add_device(bus, &cfg, &pm1_) != ESP_OK) return false;
  control_ = std::make_unique<hg::StickS3Control>(
      [this](uint8_t reg, uint8_t* data, size_t size) {
        return i2c_master_transmit_receive(pm1_, &reg, 1, data, size, 50) == ESP_OK;
      },
      [this](uint8_t reg, uint8_t value) {
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(pm1_, data, sizeof(data), 50) == ESP_OK;
      },
      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });
  if (control_->begin()) return true;
  control_.reset();
  ESP_LOGE("hg.sticks3", "M5PM1 initialization failed");
  return false;
}

void StickS3Board::set_amplifier(bool on) {
  if (control_ && !control_->set_amplifier(on)) ESP_LOGW("hg.sticks3", "amplifier switch failed");
}

}  // namespace hgp
