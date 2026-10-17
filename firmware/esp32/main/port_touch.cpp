// Touch controllers, a key mirrored on a TCA9554 or PCF8574 expander and a
// rotary encoder, polled from their own task. Samples become Touch, Key and
// Encoder events; the app task turns them into gestures (hg::TouchGestures)
// and button presses.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
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
    // CoreS3's expander has already released reset; the 2.06" watch wires it to a GPIO.
    cfg.rst_gpio_num = touch.rst >= 0 ? static_cast<gpio_num_t>(touch.rst) : GPIO_NUM_NC;
    cfg.int_gpio_num = GPIO_NUM_NC;
    if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) == ESP_OK &&
        esp_lcd_touch_new_i2c_ft5x06(io, &cfg, &managed_touch_) != ESP_OK) esp_lcd_panel_io_del(io);
    if (managed_touch_ && touch.int_gpio >= 0) gate_on_int();
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
    encoder_pulse_per_detent_ = encoder.pulse_per_detent;
    gpio_config_t pins = {};
    pins.pin_bit_mask = (1ULL << encoder.a) | (1ULL << encoder.b);
    pins.mode = GPIO_MODE_INPUT;
    pins.pull_up_en = GPIO_PULLUP_ENABLE;
    pins.intr_type = GPIO_INTR_ANYEDGE;  // the ISR decoder needs the edges enabled
    if (gpio_config(&pins) == ESP_OK) {
      encoder_a_ = static_cast<gpio_num_t>(encoder.a);
      encoder_b_ = static_cast<gpio_num_t>(encoder.b);
      encoder_state_ = static_cast<uint8_t>((gpio_get_level(encoder_a_) << 1) | gpio_get_level(encoder_b_));
      // Edge-driven: polling loses most detents on fast turns (the quadrature
      // table needs every edge). Each edge steps the decoder in the ISR; the
      // task drains whole detents and posts them. The service may already be
      // installed by gate_on_int().
      const auto a_handler = encoder.pulse_per_detent ? &TouchInput::on_encoder_a_edge : &TouchInput::on_encoder_edge;
      const auto b_handler = encoder.pulse_per_detent ? &TouchInput::on_encoder_b_edge : &TouchInput::on_encoder_edge;
      const esp_err_t isr = gpio_install_isr_service(0);
      if (isr == ESP_OK || isr == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_add(encoder_a_, a_handler, this);
        gpio_isr_handler_add(encoder_b_, b_handler, this);
      } else {
        ESP_LOGW(TAG, "encoder ISR unavailable (%s); detents may drop on fast turns", esp_err_to_name(isr));
      }
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
// Runs on every A/B edge: one quadrature step toward the next detent; the
// fourth step is a whole detent, parked for the task to post. Polling at the
// task's 20 ms cadence skipped edges on fast turns and dropped most detents.
void IRAM_ATTR TouchInput::on_encoder_edge(void* arg) {
  auto* self = static_cast<TouchInput*>(arg);
  static constexpr int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t state = static_cast<uint8_t>((gpio_get_level(self->encoder_a_) << 1) | gpio_get_level(self->encoder_b_));
  int8_t accum = static_cast<int8_t>(self->encoder_accumulator_ + kStep[(self->encoder_state_ << 2) | state]);
  self->encoder_state_ = state;
  BaseType_t woken = pdFALSE;
  if (accum >= 4 || accum <= -4) {
    self->encoder_pending_ += accum > 0 ? 1 : -1;
    accum = 0;
    if (self->task_) vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(self->task_), &woken);
    if (woken) portYIELD_FROM_ISR();
  }
  self->encoder_accumulator_ = accum;
}

// Mouse-wheel style encoders (the Knob-1.8 ring): every detent emits a full low
// pulse on one channel and then, ~100-160 ms later, a full pulse on the other —
// the leading channel picks the direction (CW: A then B; CCW: B then A). A naive
// pulse-per-edge count cancels itself (each detent = +1 then -1), and a quadrature
// table sums to zero on the 11→01→11→10→11 walk, which is why both previous
// decoders never scrolled. So: the first assert edge arms the detent, the partner
// channel's assert edge completes it, and a 200 ms timeout posts solo pulses
// (fast spins blur the pairs together). Channel A leads = clockwise, B = counter-
// clockwise (mapping validated by the owner on hardware).
void TouchInput::encoder_pulse(int8_t channel) {
  auto* self = this;
  const gpio_num_t pin = channel > 0 ? self->encoder_a_ : self->encoder_b_;
  if (gpio_get_level(pin) != 0) return;  // only the assert edge
  const int64_t now = esp_timer_get_time();
  if (self->last_edge_us_ != INT64_MIN && now - self->last_edge_us_ < 20000) return;  // contact bounce
  self->last_edge_us_ = now;
  BaseType_t woken = pdFALSE;
  if (self->encoder_first_ == 0) {
    self->encoder_first_ = channel;  // leading edge: direction is decided here
    self->encoder_pair_deadline_us_ = now + 200000;
    return;                          // wait for the partner pulse or the task's timeout
  }
  if (channel != self->encoder_first_) {  // partner arrived: one full detent
    self->encoder_pending_ += self->encoder_first_ > 0 ? -1 : 1;  // A-led = CW
    self->encoder_first_ = 0;
    self->encoder_pair_deadline_us_ = 0;
    if (self->task_) vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(self->task_), &woken);
    if (woken) portYIELD_FROM_ISR();
  }
}

void IRAM_ATTR TouchInput::on_encoder_a_edge(void* arg) {
  static_cast<TouchInput*>(arg)->encoder_pulse(1);
}

void IRAM_ATTR TouchInput::on_encoder_b_edge(void* arg) {
  static_cast<TouchInput*>(arg)->encoder_pulse(-1);
}

// The 2.06's FT3168 dozes after idling: it then refuses most reads (every 20 ms poll failed for
// seconds at a time, each logging an error), and the touch that wakes it shows up only as one
// short INT pulse whose report is gone ~20 ms later (a polled task lost that tap). Its power
// registers (G_CTRL 0x86, the monitor delay 0x87) don't stay written. So, with INT wired, an
// interrupt on its falling edge wakes the task to read at once, and without a finger the chip is
// read only after an edge.
void TouchInput::gate_on_int() {
  gpio_config_t pin = {};
  pin.pin_bit_mask = 1ULL << touch_.int_gpio;
  pin.mode = GPIO_MODE_INPUT;
  pin.pull_up_en = GPIO_PULLUP_ENABLE;
  pin.intr_type = GPIO_INTR_NEGEDGE;
  if (gpio_config(&pin) != ESP_OK || !gpio_isr_service() ||
      gpio_isr_handler_add(static_cast<gpio_num_t>(touch_.int_gpio), &TouchInput::on_int, this) != ESP_OK) {
    ESP_LOGW(TAG, "touch INT on GPIO %d unavailable; polling the chip", touch_.int_gpio);
    return;
  }
  int_gated_ = true;
  ESP_LOGI(TAG, "touch read on INT (GPIO %d)", touch_.int_gpio);
}

void IRAM_ATTR TouchInput::on_int(void* arg) {
  auto* self = static_cast<TouchInput*>(arg);
  self->int_pending_ = true;
  BaseType_t woken = pdFALSE;
  if (self->task_) vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(self->task_), &woken);
  if (woken) portYIELD_FROM_ISR();
}

// Without INT every poll reads; with it, only a pending report or a finger still down does.
bool TouchInput::may_read(bool was_touching) {
  return !int_gated_ || int_pending_.exchange(false) || was_touching;
}

// Waits out the poll period, or less: an INT edge or an encoder detent ends
// the wait at once (both notify this task).
void TouchInput::wait(uint32_t ms) {
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

void TouchInput::task(void* arg) {
  auto* self = static_cast<TouchInput*>(arg);
  self->task_ = xTaskGetCurrentTaskHandle();
  bool was_touching = false, key_down = false;
  hg::Cst816Fault fault;
  TouchSample last{};
  for (;;) {
    TouchSample s{};
    if (self->has_touch() && self->may_read(was_touching) && self->read_touch(s)) {
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
    // Encoder detents arrive from the ISR (one per quadrature cycle); post each.
    int8_t pending = self->encoder_pending_.exchange(0);
    while (pending != 0) {
      const int8_t step = pending > 0 ? 1 : -1;
      pending = static_cast<int8_t>(pending - step);
      EncoderSample e{step};
      events::post(EventType::Encoder, &e, sizeof(e));
    }
    // Solo-pulse timeout: a detent whose partner never showed (fast spin or
    // a stop mid-pair) posts from its leading channel once 200 ms pass quiet.
    if (self->encoder_first_ != 0 && self->encoder_pair_deadline_us_ != 0 &&
        esp_timer_get_time() > self->encoder_pair_deadline_us_) {
      self->encoder_pending_ += self->encoder_first_ > 0 ? -1 : 1;  // A-led = CW
      self->encoder_first_ = 0;
      self->encoder_pair_deadline_us_ = 0;
    }
    // Dozing (screen off, on battery), poll less often so the chip can light-sleep
    // between polls; a touch still wakes the screen within about 50 ms.
    self->wait(EspSystem::dozing_now() ? kDozePollMs : kPollMs);
  }
}

}  // namespace hgp
