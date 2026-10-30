#include "port.hpp"

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace hgp {

bool LatchPower::begin(const LatchPowerConfig& cfg) {
  cfg_ = cfg;
  // A board can read the battery without a latch. Skip the enable pin then:
  // -1 would select a bogus mask and collide with the flash bus.
  if (cfg.enable >= 0) {
    // Set the output latch before enabling the pin to avoid a low pulse at boot.
    if (gpio_set_level(static_cast<gpio_num_t>(cfg.enable), 1) != ESP_OK) return false;
    gpio_config_t output = {};
    output.pin_bit_mask = 1ULL << cfg.enable;
    output.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&output) != ESP_OK) return false;
  }
  if (cfg.backlight >= 0) {
    gpio_reset_pin(static_cast<gpio_num_t>(cfg.backlight));
    gpio_set_direction(static_cast<gpio_num_t>(cfg.backlight), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(cfg.backlight), 0);
  }
  // Only some boards route the charging signal to a GPIO. Leave it unconfigured
  // otherwise: -1 would shift to a bogus pin mask and collide with the flash bus.
  if (cfg.charging >= 0) {
    gpio_config_t input = {};
    input.pin_bit_mask = 1ULL << cfg.charging;
    input.mode = GPIO_MODE_INPUT;
    input.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&input) != ESP_OK) return false;
  }

  if (cfg.key >= 0) {
    gpio_config_t key = {};
    key.pin_bit_mask = 1ULL << cfg.key;
    key.mode = GPIO_MODE_INPUT;
    key.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&key) != ESP_OK) return false;
  }

  if (cfg.adc < 0) {
    // The latch and power-off work without a battery reading.
    ESP_LOGI("hg.power", "battery latch enabled; no battery voltage reading");
    return true;
  }
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
  ESP_LOGI("hg.power", "%s; calibrated voltage %s", cfg.enable >= 0 ? "battery latch enabled" : "battery sense enabled",
           calibration_ ? "available" : "unavailable");
  return true;
}

std::optional<hg::PowerStatus> LatchPower::read() {
  hg::PowerStatus status;
  // Not every board brings the charging signal out to a pin.
  if (cfg_.charging >= 0) status.charging = charging_signal();
  status.external_power = external_power();
  if (!calibration_) return status;
  int total = 0;
  for (int i = 0; i < 8; ++i) {
    int raw;
    if (adc_oneshot_read(adc_, channel_, &raw) != ESP_OK) return std::nullopt;
    total += raw;
  }
  int mv;
  if (adc_cali_raw_to_voltage(calibration_, total / 8, &mv) != ESP_OK) return std::nullopt;
  // ADC measures the divided battery node. On the T-Display-S3, the charger
  // can hold this node above its no-battery threshold; it has no separate
  // presence signal, so treat that reading as unavailable.
  const int ratio = cfg_.mv_ratio > 0 ? cfg_.mv_ratio : 1;
  const int battery_mv = mv * ratio;
  if (mv >= 0 && battery_mv <= cfg_.max_battery_mv) status.battery_mv = static_cast<uint16_t>(battery_mv);
  return status;
}

bool LatchPower::charging_signal() const {
  return gpio_get_level(static_cast<gpio_num_t>(cfg_.charging)) == (cfg_.charging_active_high ? 1 : 0);
}

std::optional<bool> LatchPower::external_power() {
  if (!cfg_.usb_external_power) return std::nullopt;
  return (cfg_.charging >= 0 && charging_signal()) || usb_serial_jtag_is_connected();
}

// Polled every ~100 ms by the app. A release before the hold time is a press;
// reaching it powers off once, and the release that follows is not a press.
bool LatchPower::take_key_press() {
  if (cfg_.key < 0) return false;
  const bool down = gpio_get_level(static_cast<gpio_num_t>(cfg_.key)) == 0;
  const int64_t now_ms = esp_timer_get_time() / 1000;
  if (down && !key_down_) {
    key_down_ = true;
    key_held_ = false;
    key_since_ms_ = now_ms;
    return false;
  }
  if (down && !key_held_ && now_ms - key_since_ms_ >= kKeyHoldMs) {
    key_held_ = true;
    ESP_LOGI("hg.power", "power key held: power off");
    power_off();
    return true;  // still running on USB: the screen goes dark as a sign
  }
  if (!down && key_down_) {
    key_down_ = false;
    return !key_held_;
  }
  return false;
}

bool LatchPower::power_off() {
  if (!cfg_.power_off_supported) return false;
  // Supported boards use a battery latch that disconnects the supply. USB may
  // still power the device independently.
  return gpio_set_level(static_cast<gpio_num_t>(cfg_.enable), 0) == ESP_OK;
}

}  // namespace hgp
