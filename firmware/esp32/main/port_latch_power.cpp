#include "port.hpp"

#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

namespace hgp {

bool LatchPower::begin(const LatchPowerConfig& cfg) {
  cfg_ = cfg;
  // Set the output latch before enabling the pin to avoid a low pulse at boot.
  if (gpio_set_level(static_cast<gpio_num_t>(cfg.enable), 1) != ESP_OK) return false;
  gpio_config_t output = {};
  output.pin_bit_mask = 1ULL << cfg.enable;
  output.mode = GPIO_MODE_OUTPUT;
  if (gpio_config(&output) != ESP_OK) return false;
  gpio_config_t input = {};
  input.pin_bit_mask = 1ULL << cfg.charging;
  input.mode = GPIO_MODE_INPUT;
  input.pull_up_en = GPIO_PULLUP_ENABLE;
  if (gpio_config(&input) != ESP_OK) return false;

  adc_unit_t unit;
  if (adc_oneshot_io_to_channel(cfg.adc, &unit, &channel_) != ESP_OK) return false;
  adc_oneshot_unit_init_cfg_t init = {};
  init.unit_id = unit;
  if (adc_oneshot_new_unit(&init, &adc_) != ESP_OK) return false;
  adc_oneshot_chan_cfg_t channel = {};
  channel.atten = ADC_ATTEN_DB_12;
  channel.bitwidth = ADC_BITWIDTH_DEFAULT;
  if (adc_oneshot_config_channel(adc_, channel_, &channel) != ESP_OK) {
    adc_oneshot_del_unit(adc_);
    adc_ = nullptr;
    return false;
  }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t calibration = {};
  calibration.unit_id = unit;
  calibration.chan = channel_;
  calibration.atten = channel.atten;
  calibration.bitwidth = channel.bitwidth;
  if (adc_cali_create_scheme_curve_fitting(&calibration, &calibration_) != ESP_OK) calibration_ = nullptr;
#endif
  ESP_LOGI("hg.power", "battery latch enabled; calibrated voltage %s", calibration_ ? "available" : "unavailable");
  return true;
}

std::optional<hg::PowerStatus> LatchPower::read() {
  hg::PowerStatus status;
  status.charging = gpio_get_level(static_cast<gpio_num_t>(cfg_.charging)) == 0;
  if (!calibration_) return status;
  int total = 0;
  for (int i = 0; i < 8; ++i) {
    int raw;
    if (adc_oneshot_read(adc_, channel_, &raw) != ESP_OK) return std::nullopt;
    total += raw;
  }
  int mv;
  if (adc_cali_raw_to_voltage(calibration_, total / 8, &mv) != ESP_OK) return std::nullopt;
  // Waveshare schematic R27=200k, R32=100k: VBAT = 3 * VADC.
  // This circuit has no presence detector or fuel gauge; do not infer either.
  if (mv >= 0 && mv <= 1666) status.battery_mv = static_cast<uint16_t>(mv * 3);
  return status;
}

bool LatchPower::power_off() {
  // USB feeds VSYS independently, so this only disconnects the battery path.
  return gpio_set_level(static_cast<gpio_num_t>(cfg_.enable), 0) == ESP_OK;
}

}  // namespace hgp
