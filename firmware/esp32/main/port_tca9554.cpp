#include "port.hpp"
#include "tca9554.hpp"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {
i2c_master_dev_handle_t g_levels = nullptr;
uint16_t g_backlight_bit = 0;
bool g_pca9535 = false;
uint16_t g_power_hold = 0;

// PCA9535 registers come in pairs, port 0 then port 1; a two-byte access covers both.
constexpr uint8_t kPca9535Output = 2, kPca9535Config = 6;

bool pca9535_read(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t& value) {
  uint8_t data[2] = {};
  if (i2c_master_transmit_receive(dev, &reg, 1, data, 2, 50) != ESP_OK) return false;
  value = static_cast<uint16_t>(data[0] | (data[1] << 8));
  return true;
}

bool pca9535_write(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value) {
  const uint8_t data[] = {reg, static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  return i2c_master_transmit(dev, data, sizeof(data), 50) == ESP_OK;
}

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
  if (levels.pca9535) {
    i2c_master_dev_handle_t dev = nullptr;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = levels.addr ? levels.addr : 0x20;
    cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) {
      ESP_LOGE("hg.tca9554", "PCA9535 0x%02x: no device handle", cfg.device_address);
      return false;
    }
    auto fail = [dev, &cfg](const char* what) {
      i2c_master_bus_rm_device(dev);
      ESP_LOGE("hg.tca9554", "PCA9535 0x%02x: %s failed", cfg.device_address, what);
      return false;
    };
    // Outputs before directions, so no pin glitches when it turns into an output. The
    // supply latch goes first, as in Seeed's BSP, then the rails behind it.
    uint16_t output = 0, config = 0;
    if (!pca9535_read(dev, kPca9535Output, output) || !pca9535_read(dev, kPca9535Config, config)) return fail("read");
    const uint16_t mask = levels.low | levels.reset | levels.startup_high | levels.backlight | levels.power_hold;
    const uint16_t high = static_cast<uint16_t>((levels.startup_high | levels.backlight | levels.power_hold) & ~levels.low);
    if (levels.power_hold) {
      output = static_cast<uint16_t>(output | levels.power_hold);
      config = static_cast<uint16_t>(config & ~levels.power_hold);
      if (!pca9535_write(dev, kPca9535Output, output) || !pca9535_write(dev, kPca9535Config, config))
        return fail("supply latch");
    }
    output = static_cast<uint16_t>((output & ~mask) | high);
    if (!pca9535_write(dev, kPca9535Output, output) ||
        !pca9535_write(dev, kPca9535Config, static_cast<uint16_t>(config & ~mask))) {
      return fail("levels");
    }
    vTaskDelay(pdMS_TO_TICKS(levels.hold_ms));
    if (levels.reset && !pca9535_write(dev, kPca9535Output, static_cast<uint16_t>(output | levels.reset)))
      return fail("reset release");
    vTaskDelay(pdMS_TO_TICKS(levels.settle_ms));
    g_levels = dev;
    g_backlight_bit = levels.backlight;
    g_power_hold = levels.power_hold;
    g_pca9535 = true;
    ESP_LOGI("hg.tca9554", "PCA9535 0x%02x levels ready (high 0x%04x low 0x%04x)", cfg.device_address, high,
             levels.low);
    return true;
  }
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
  if (g_pca9535) {
    uint16_t output = 0;
    if (!pca9535_read(g_levels, kPca9535Output, output)) return;
    const uint16_t next = percent ? static_cast<uint16_t>(output | g_backlight_bit)
                                  : static_cast<uint16_t>(output & ~g_backlight_bit);
    if (next != output && !pca9535_write(g_levels, kPca9535Output, next))
      ESP_LOGW("hg.tca9554", "PCA9535 backlight update failed");
    return;
  }
  uint8_t output = 0;
  if (!reg_read(g_levels, 1, output)) {
    ESP_LOGW("hg.tca9554", "backlight update failed");
    return;
  }
  const uint8_t next = percent ? static_cast<uint8_t>(output | g_backlight_bit)
                               : static_cast<uint8_t>(output & ~g_backlight_bit);
  if (next != output && !reg_write(g_levels, 1, next)) {
    ESP_LOGW("hg.tca9554", "backlight update failed");
  }
}

bool expander_power_off() {
  if (!g_levels || !g_pca9535 || !g_power_hold) return false;
  uint16_t output = 0;
  if (!pca9535_read(g_levels, kPca9535Output, output)) return false;
  ESP_LOGI("hg.tca9554", "releasing the supply latch 0x%04x", g_power_hold);
  // On USB power the board keeps running; the app reports the request either way.
  return pca9535_write(g_levels, kPca9535Output, static_cast<uint16_t>(output & ~g_power_hold));
}
}  // namespace hgp
