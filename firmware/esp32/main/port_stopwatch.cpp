#include "port.hpp"

#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {
const char* TAG = "hg.stopwatch";
constexpr uint8_t kPm1 = 0x6E, kIoe = 0x4F, kTouch = 0x15;

bool add(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t hz, i2c_master_dev_handle_t* dev) {
  if (i2c_master_probe(bus, addr, 50) != ESP_OK) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = addr;
  cfg.scl_speed_hz = hz;
  return i2c_master_bus_add_device(bus, &cfg, dev) == ESP_OK;
}

hg::StopWatchControl::Read reader(i2c_master_dev_handle_t* dev) {
  return [dev](uint8_t reg, uint8_t* data, size_t size) {
    return i2c_master_transmit_receive(*dev, &reg, 1, data, size, 50) == ESP_OK;
  };
}
}  // namespace

bool StopWatchBoard::begin(i2c_master_bus_handle_t bus) {
  // 100 kHz for both, the vendor's clock for them.
  if (!bus || !add(bus, kPm1, 100000, &pm1_) || !add(bus, kIoe, 100000, &ioe_)) {
    ESP_LOGE(TAG, "M5PM1 or M5IOE1 not found");
    return false;
  }
  control_ = std::make_unique<hg::StopWatchControl>(
      hg::StopWatchControl::Chip{reader(&pm1_),
                                 [this](uint8_t reg, uint8_t value) {
                                   const uint8_t data[] = {reg, value};
                                   return i2c_master_transmit(pm1_, data, sizeof(data), 50) == ESP_OK;
                                 }},
      reader(&ioe_),
      [this](uint8_t reg, uint16_t value) {
        const uint8_t data[] = {reg, static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>(value >> 8)};
        return i2c_master_transmit(ioe_, data, sizeof(data), 50) == ESP_OK;
      },
      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });
  if (!control_->begin()) {
    control_.reset();
    ESP_LOGE(TAG, "power sequence failed");
    return false;
  }
  // The CST820 falls asleep after a few idle seconds and then answers no read; its
  // INT line is not used here, so it is told to stay awake (register 0xFE = 1).
  i2c_master_dev_handle_t touch = nullptr;
  if (add(bus, kTouch, 400000, &touch)) {
    const uint8_t stay_awake[] = {0xFE, 0x01};
    if (i2c_master_transmit(touch, stay_awake, sizeof(stay_awake), 50) != ESP_OK)
      ESP_LOGW(TAG, "touch controller kept its auto sleep");
    i2c_master_bus_rm_device(touch);
  }
  return true;
}

void StopWatchBoard::set_amplifier(bool on) {
  if (control_ && !control_->set_amplifier(on)) ESP_LOGW(TAG, "amplifier switch failed");
}

}  // namespace hgp
