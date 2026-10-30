#include "port.hpp"
#include "tca9554.hpp"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {
i2c_master_dev_handle_t g_levels = nullptr;
uint8_t g_backlight_bit = 0;

bool reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t& value) {
  return i2c_master_transmit_receive(dev, &reg, 1, &value, 1, 50) == ESP_OK;
}

bool reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return i2c_master_transmit(dev, data, sizeof(data), 50) == ESP_OK;
}
}  // namespace

bool tca9554_reset(i2c_master_bus_handle_t bus, const ExpanderResetConfig& reset) {
  if (!bus) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = reset.addr;
  cfg.scl_speed_hz = 400000;
  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) return false;
  hg::Tca9554Reset pulse(
      reset.mask,
      [dev](uint8_t reg, uint8_t& value) {
        return i2c_master_transmit_receive(dev, &reg, 1, &value, 1, 50) == ESP_OK;
      },
      [dev](uint8_t reg, uint8_t value) {
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(dev, data, sizeof(data), 50) == ESP_OK;
      },
      [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }, reset.hold_ms, reset.settle_ms);
  const bool ok = pulse.begin();
  i2c_master_bus_rm_device(dev);
  ESP_LOGI("hg.tca9554", "expander resets 0x%02x %s", reset.mask, ok ? "ready" : "failed");
  return ok;
}

bool tca9554_levels_begin(i2c_master_bus_handle_t bus, const ExpanderLevelConfig& levels) {
  if (!bus || !levels.enabled) return false;
  // Korvo-2 probes the TCA9554A address, then the TCA9554 address.
  static constexpr uint8_t kProbe[] = {0x38, 0x20};
  i2c_master_dev_handle_t dev = nullptr;
  uint8_t found = 0;
  for (uint8_t addr : kProbe) {
    if (levels.addr && addr != levels.addr) continue;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) continue;
    found = addr;
    break;
  }
  if (!dev) {
    ESP_LOGE("hg.tca9554", "expander levels: no TCA9554 at 0x38 or 0x20");
    return false;
  }
  constexpr uint8_t kOutput = 1, kConfig = 3;
  uint8_t output = 0, config = 0;
  const uint8_t mask = static_cast<uint8_t>(levels.low | levels.reset | levels.backlight);
  const bool wrote = reg_read(dev, kOutput, output) && reg_read(dev, kConfig, config) &&
      reg_write(dev, kOutput, static_cast<uint8_t>((output & ~levels.low & ~levels.reset) | levels.backlight)) &&
      reg_write(dev, kConfig, static_cast<uint8_t>(config & ~mask));
  if (!wrote) {
    i2c_master_bus_rm_device(dev);
    ESP_LOGE("hg.tca9554", "expander levels at 0x%02x failed", found);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(levels.hold_ms));
  if (levels.reset && !reg_write(dev, kOutput, static_cast<uint8_t>(
          ((output & ~levels.low & ~levels.reset) | levels.backlight) | levels.reset))) {
    i2c_master_bus_rm_device(dev);
    ESP_LOGE("hg.tca9554", "expander reset release at 0x%02x failed", found);
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(levels.settle_ms));
  g_levels = dev;
  g_backlight_bit = levels.backlight;
  ESP_LOGI("hg.tca9554", "expander 0x%02x levels ready (low 0x%02x reset 0x%02x backlight 0x%02x)",
           found, levels.low, levels.reset, levels.backlight);
  return true;
}

void tca9554_backlight(uint8_t percent) {
  if (!g_levels || !g_backlight_bit) return;
  uint8_t output = 0;
  if (!reg_read(g_levels, 1, output)) return;
  const uint8_t next = percent ? static_cast<uint8_t>(output | g_backlight_bit)
                               : static_cast<uint8_t>(output & ~g_backlight_bit);
  if (next != output) reg_write(g_levels, 1, next);
}
}  // namespace hgp
