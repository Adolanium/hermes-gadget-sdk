// Touch controllers, a key mirrored on a TCA9554 or PCF8574 expander and a
// rotary encoder, polled from their own task. Samples become Touch, Key and
// Encoder events; the app task turns them into gestures (hg::TouchGestures)
// and button presses.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>

#include "driver/gpio.h"
#include "esp_log.h"
#include "cst816.hpp"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_tt21100.h"
#include "esp_lcd_touch_ft5x06.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.touch";
constexpr uint32_t kPollMs = 20;
constexpr uint32_t kDozePollMs = 50;
constexpr uint8_t kTca9554Input = 0x00;
constexpr uint8_t kCstAck = 0xAB;

}  // namespace

bool TouchInput::begin(const TouchConfig& touch, const ExpanderKeyConfig& key, const EncoderConfig& encoder,
                       i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  touch_ = touch;
  key_ = key;
  if (touch.enabled && touch.controller == TouchController::Ft5x06) {
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    io_cfg.scl_speed_hz = 100000;
    io_cfg.control_phase_bytes = 1;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.flags.disable_control_phase = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_touch_config_t cfg = {};
    cfg.x_max = touch.width;
    cfg.y_max = touch.height;
    cfg.rst_gpio_num = GPIO_NUM_NC;  // The board's expander already released reset.
    cfg.int_gpio_num = GPIO_NUM_NC;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) == ESP_OK &&
        esp_lcd_touch_new_i2c_ft5x06(io, &cfg, &managed_touch_) != ESP_OK) esp_lcd_panel_io_del(io);
  } else if (touch.enabled && touch.controller == TouchController::Box3) {
    begin_box_touch(bus);
  } else if (touch.enabled) {
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
      const uint8_t command_mode[2] = {touch.controller == TouchController::Cst816 ? uint8_t(0xFE) : uint8_t(0xD1), 0x01};
      if (i2c_master_transmit(touch_dev_, command_mode, sizeof(command_mode), 50) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller at 0x%02x did not answer", touch.addr);
        if (touch.controller == TouchController::Cst816) {
          i2c_master_bus_rm_device(touch_dev_);
          touch_dev_ = nullptr;
        }
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
    if (key_dev_ && key.pcf8574) {
      // A PCF8574 pin reads as an input only while its output latch is high.
      const uint8_t release = 0xFF;
      if (i2c_master_transmit(key_dev_, &release, 1, 50) != ESP_OK) ESP_LOGW(TAG, "PCF8574 at 0x%02x did not answer", key.addr);
    }
  }
  if (encoder.a >= 0 && encoder.b >= 0) {
    gpio_config_t pins = {};
    pins.pin_bit_mask = (1ULL << encoder.a) | (1ULL << encoder.b);
    pins.mode = GPIO_MODE_INPUT;
    pins.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&pins) == ESP_OK) {
      encoder_a_ = static_cast<gpio_num_t>(encoder.a);
      encoder_b_ = static_cast<gpio_num_t>(encoder.b);
      encoder_state_ = static_cast<uint8_t>((gpio_get_level(encoder_a_) << 1) | gpio_get_level(encoder_b_));
    }
  }
  const bool has_encoder = encoder_a_ != GPIO_NUM_NC;
  if (!has_touch() && !key_dev_ && !has_encoder) return false;
  if (xTaskCreate(&TouchInput::task, "hg-touch", 3072, this, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "could not start the touch task");
    return false;
  }
  ESP_LOGI(TAG, "touch %s, key %s, encoder %s", has_touch() ? "ready" : "off", key_dev_ ? "ready" : "off",
           has_encoder ? "ready" : "off");
  return true;
}

bool TouchInput::read_touch(TouchSample& out) {
  if (managed_touch_) {
    if (esp_lcd_touch_read_data(managed_touch_) != ESP_OK) return false;
    uint16_t x = 0, y = 0;
    uint8_t points = 0;
    const bool down = esp_lcd_touch_get_coordinates(managed_touch_, &x, &y, nullptr, &points, 1);
    out = {down && points > 0, static_cast<int16_t>(x), static_cast<int16_t>(y)};
    return true;
  }
  if (touch_.controller == TouchController::Cst816) {
    const uint8_t reg = 0x02;
    uint8_t data[5] = {};
    if (i2c_master_transmit_receive(touch_dev_, &reg, 1, data, sizeof(data), 20) != ESP_OK) return false;
    return hg::cst816_touch(data, touch_.width, touch_.height, out.touching, out.x, out.y);
  }
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

bool TouchInput::begin_box_touch(i2c_master_bus_handle_t bus) {
  esp_lcd_panel_io_i2c_config_t io_cfg = {};
  io_cfg.scl_speed_hz = 100000;
  io_cfg.control_phase_bytes = 1;
  io_cfg.lcd_cmd_bits = 16;
  io_cfg.flags.disable_control_phase = 1;
  bool tt21100 = false;
  if (i2c_master_probe(bus, 0x5d, 50) == ESP_OK || i2c_master_probe(bus, 0x14, 50) == ESP_OK) {
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(bus, 0x5d, 50) != ESP_OK) io_cfg.dev_addr = 0x14;
  } else if (i2c_master_probe(bus, 0x24, 50) == ESP_OK) {
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_TT21100_ADDRESS;
    tt21100 = true;
  } else {
    return false;
  }
  esp_lcd_panel_io_handle_t io = nullptr;
  if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) return false;
  esp_lcd_touch_config_t cfg = {};
  cfg.x_max = touch_.width;
  cfg.y_max = touch_.height;
  cfg.rst_gpio_num = GPIO_NUM_NC;  // Display initialization already reset the shared line.
  cfg.int_gpio_num = touch_.int_gpio >= 0 ? static_cast<gpio_num_t>(touch_.int_gpio) : GPIO_NUM_NC;
  cfg.flags.mirror_x = tt21100;
  const esp_err_t err = tt21100 ? esp_lcd_touch_new_i2c_tt21100(io, &cfg, &managed_touch_)
                              : esp_lcd_touch_new_i2c_gt911(io, &cfg, &managed_touch_);
  if (err != ESP_OK) { esp_lcd_panel_io_del(io); return false; }
  return true;
}

bool TouchInput::read_key(bool& pressed) {
  uint8_t reg = kTca9554Input, value = 0;
  const esp_err_t err = key_.pcf8574 ? i2c_master_receive(key_dev_, &value, 1, 20)
                                     : i2c_master_transmit_receive(key_dev_, &reg, 1, &value, 1, 20);
  if (err != ESP_OK) return false;
  bool high = (value >> key_.bit) & 1;
  pressed = key_.active_high ? high : !high;
  return true;
}

// A quadrature step table: four steps make one detent.
bool TouchInput::sample_encoder(int& direction) {
  direction = 0;
  if (encoder_a_ == GPIO_NUM_NC) return false;
  static constexpr int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t state = static_cast<uint8_t>((gpio_get_level(encoder_a_) << 1) | gpio_get_level(encoder_b_));
  encoder_accumulator_ = static_cast<int8_t>(encoder_accumulator_ + kStep[(encoder_state_ << 2) | state]);
  encoder_state_ = state;
  if (encoder_accumulator_ >= 4 || encoder_accumulator_ <= -4) {
    direction = encoder_accumulator_ > 0 ? 1 : -1;
    encoder_accumulator_ = 0;
  }
  return true;
}

void TouchInput::task(void* arg) {
  auto* self = static_cast<TouchInput*>(arg);
  bool was_touching = false, key_down = false;
  hg::Cst816Fault fault;
  TouchSample last{};
  for (;;) {
    TouchSample s{};
    if (self->has_touch() && self->read_touch(s)) {
      s.y = static_cast<int16_t>(s.y - self->touch_.offset_y);
      fault.valid();
      // Every sample while the finger is down (gestures need the motion), plus the lift.
      if (s.touching || was_touching) {
        const bool posted = events::post(EventType::Touch, &s, sizeof(s));
        // CST816: keep the delivered state until enqueue succeeds, including a lift.
        if (posted || self->touch_.controller != TouchController::Cst816) was_touching = s.touching;
      }
      last = s;
    } else if (self->touch_.controller == TouchController::Cst816 &&
               fault.release_due(was_touching)) {
      // Five failed polls (~100–200 ms plus scheduler delay): release once.
      // Retry if the event queue is full; do not silently lose the release.
      last.touching = false;
      if (events::post(EventType::Touch, &last, sizeof(last))) {
        was_touching = false;
        ESP_LOGW(TAG, "CST816 fault: held touch released after five failed polls");
      }
    }
    bool pressed = false;
    if (self->key_dev_ && self->read_key(pressed) && pressed != key_down) {
      key_down = pressed;
      KeySample k{pressed};
      events::post(EventType::Key, &k, sizeof(k));
    }
    int direction = 0;
    if (self->sample_encoder(direction) && direction) {
      EncoderSample e{static_cast<int8_t>(direction)};
      events::post(EventType::Encoder, &e, sizeof(e));
    }
    // Dozing (screen off, on battery), poll less often so the chip can light-sleep
    // between polls; a touch still wakes the screen within about 50 ms.
    vTaskDelay(pdMS_TO_TICKS(EspSystem::dozing_now() ? kDozePollMs : kPollMs));
  }
}

}  // namespace hgp
