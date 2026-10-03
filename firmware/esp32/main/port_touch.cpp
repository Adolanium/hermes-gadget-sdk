// CST9217 touchscreen and a key mirrored on a TCA9554 expander, polled over
// I2C from their own task. Samples become Touch and Key events; the app task
// turns them into gestures (hg::TouchGestures) and button presses.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.touch";
constexpr uint32_t kPollMs = 20;
constexpr uint8_t kTca9554Input = 0x00;
constexpr uint8_t kCstAck = 0xAB;

}  // namespace

bool TouchInput::begin(const TouchConfig& touch, const ExpanderKeyConfig& key, i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  touch_ = touch;
  key_ = key;
  if (touch.enabled) {
    if (touch.rst >= 0) {
      gpio_config_t rst = {};
      rst.pin_bit_mask = 1ULL << touch.rst;
      rst.mode = GPIO_MODE_OUTPUT;
      gpio_config(&rst);
      gpio_set_level(static_cast<gpio_num_t>(touch.rst), 0);
      vTaskDelay(pdMS_TO_TICKS(10));
      gpio_set_level(static_cast<gpio_num_t>(touch.rst), 1);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = touch.addr;
    dev.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &dev, &touch_dev_) == ESP_OK) {
      const uint8_t command_mode[2] = {0xD1, 0x01};
      if (i2c_master_transmit(touch_dev_, command_mode, sizeof(command_mode), 50) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller at 0x%02x did not answer", touch.addr);
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  if (key.enabled) {
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = key.addr;
    dev.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &dev, &key_dev_) != ESP_OK) key_dev_ = nullptr;
  }
  if (!touch_dev_ && !key_dev_) return false;
  xTaskCreate(&TouchInput::task, "hg-touch", 3072, this, 5, nullptr);
  ESP_LOGI(TAG, "touch %s, key %s", touch_dev_ ? "ready" : "off", key_dev_ ? "ready" : "off");
  return true;
}

bool TouchInput::read_touch(TouchSample& out) {
  const uint8_t reg[2] = {0xD0, 0x00};
  if (i2c_master_transmit(touch_dev_, reg, sizeof(reg), 20) != ESP_OK) return false;
  // The controller needs ~2 ms before the read; at least one tick whatever the tick rate.
  vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(2)));
  uint8_t buf[10] = {};
  if (i2c_master_receive(touch_dev_, buf, sizeof(buf), 20) != ESP_OK) return false;
  if (buf[6] != kCstAck) return false;  // not a valid report
  const int points = buf[5] & 0x7F;
  const bool down = points > 0 && (buf[0] & 0x0F) == 0x06;
  int x = (buf[1] << 4) | (buf[3] >> 4);
  int y = (buf[2] << 4) | (buf[3] & 0x0F);
  if (touch_.mirror_x && touch_.width) x = touch_.width - 1 - x;
  if (touch_.mirror_y && touch_.height) y = touch_.height - 1 - y;
  out = {down, static_cast<int16_t>(x), static_cast<int16_t>(y)};
  return true;
}

bool TouchInput::read_key(bool& pressed) {
  uint8_t reg = kTca9554Input, value = 0;
  if (i2c_master_transmit_receive(key_dev_, &reg, 1, &value, 1, 20) != ESP_OK) return false;
  bool high = (value >> key_.bit) & 1;
  pressed = key_.active_high ? high : !high;
  return true;
}

void TouchInput::task(void* arg) {
  auto* self = static_cast<TouchInput*>(arg);
  bool was_touching = false, key_down = false;
  for (;;) {
    TouchSample s{};
    if (self->touch_dev_ && self->read_touch(s)) {
      // Every sample while the finger is down (gestures need the motion), plus the lift.
      if (s.touching || was_touching) events::post(EventType::Touch, &s, sizeof(s));
      was_touching = s.touching;
    }
    bool pressed = false;
    if (self->key_dev_ && self->read_key(pressed) && pressed != key_down) {
      key_down = pressed;
      KeySample k{pressed};
      events::post(EventType::Key, &k, sizeof(k));
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
  }
}

}  // namespace hgp
