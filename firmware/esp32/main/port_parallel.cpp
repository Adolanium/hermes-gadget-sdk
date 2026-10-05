// I80 (8-bit parallel) ST7789 panel via esp_lcd.
//
// LCD modules that put the controller on a parallel bus rather than SPI —
// LilyGO's T-Display-S3 is one — use the chip's LCD_CAM unit in i80 mode: an
// 8-bit data bus, a write strobe and a pixel clock, with DC selecting between
// commands and pixel data. Everything above this (framebuffer, bounce buffer,
// backlight) matches SpiDisplay; only the bus differs.
//
// The framebuffer lives in PSRAM, and rows reach the panel through a small
// DMA-capable bounce buffer on flush.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.lcd.i80";
constexpr int kBounceRows = 20;
static portMUX_TYPE s_aw9364_mux = portMUX_INITIALIZER_UNLOCKED;

// Park the write strobe idle once the driver owns the bus: the panel samples it
// as "no write", and leaving it driven low looks like a permanent write cycle.
void park_wr(int wr) {
  if (wr >= 0) gpio_set_level(static_cast<gpio_num_t>(wr), 1);
}

// Raises the panel's peripheral rail. Boards without one pass -1.
bool power_panel(int pin) {
  if (pin < 0) return true;
  gpio_reset_pin(static_cast<gpio_num_t>(pin));
  gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
  return gpio_set_level(static_cast<gpio_num_t>(pin), 1) == ESP_OK;
}

// Drives the T-Display-S3 AW9364 backlight pulse counter. Brightness is 0..16.
void set_aw9364(int pin, uint8_t value, uint8_t& level) {
  constexpr uint8_t steps = 16;
  value = std::min<uint8_t>(value, steps);
  gpio_set_direction(static_cast<gpio_num_t>(pin), GPIO_MODE_OUTPUT);
  if (value == 0) {
    gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    vTaskDelay(pdMS_TO_TICKS(3));
    level = 0;
    return;
  }
  if (level == 0) {
    gpio_set_level(static_cast<gpio_num_t>(pin), 1);
    level = steps;
    esp_rom_delay_us(30);
  }
  const int from = steps - level;
  const int to = steps - value;
  const int pulses = (steps + to - from) % steps;
  // AW9364 accepts TLO up to 500 us. Disable interrupts across the pulse train
  // so scheduler/ISR latency cannot corrupt the pulse count.
  portENTER_CRITICAL(&s_aw9364_mux);
  for (int i = 0; i < pulses; ++i) {
    gpio_set_level(static_cast<gpio_num_t>(pin), 0);
    gpio_set_level(static_cast<gpio_num_t>(pin), 1);
  }
  portEXIT_CRITICAL(&s_aw9364_mux);
  level = value;
}

}  // namespace

bool ParallelDisplay::on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<ParallelDisplay*>(ctx)->done_, &woken);
  return woken == pdTRUE;
}

bool ParallelDisplay::begin(const LcdConfig& cfg, int power_pin) {
  cfg_ = cfg;
  if (!power_panel(power_pin)) {
    ESP_LOGE(TAG, "could not raise the panel power pin (GPIO %d)", power_pin);
    return false;
  }
  const size_t px = static_cast<size_t>(cfg.width) * cfg.height;
  fb_ = static_cast<uint16_t*>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!fb_) fb_ = static_cast<uint16_t*>(heap_caps_malloc(px * 2, MALLOC_CAP_8BIT));
  bounce_rows_ = kBounceRows;
  bounce_ = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(cfg.width) * bounce_rows_ * 2, MALLOC_CAP_DMA));
  if (!fb_ || !bounce_) {
    ESP_LOGE(TAG, "not enough memory for a %ux%u framebuffer", cfg.width, cfg.height);
    return false;
  }
  std::memset(fb_, 0, px * 2);
  done_ = xSemaphoreCreateBinary();

  // GPIO9 is the panel's active-low RD input; park it high before bus setup.
  gpio_reset_pin(GPIO_NUM_9);
  gpio_set_direction(GPIO_NUM_9, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_9, 1);

  esp_lcd_i80_bus_config_t bus = {};
  bus.clk_src = LCD_CLK_SRC_PLL160M;
  bus.dc_gpio_num = static_cast<gpio_num_t>(cfg.dc);
  bus.wr_gpio_num = static_cast<gpio_num_t>(cfg.bus.wr);
  bus.bus_width = 8;
  for (int i = 0; i < 8; ++i) bus.data_gpio_nums[i] = static_cast<gpio_num_t>(cfg.bus.data[i]);
  bus.max_transfer_bytes = static_cast<size_t>(cfg.width) * bounce_rows_ * 2;
  ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus, &i80_));
  park_wr(cfg.bus.wr);

  esp_lcd_panel_io_i80_config_t io_cfg = {};
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(cfg.cs);
  io_cfg.pclk_hz = static_cast<uint32_t>(cfg.bus.pclk_mhz) * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  io_cfg.dc_levels.dc_idle_level = 0;
  io_cfg.dc_levels.dc_cmd_level = 0;
  io_cfg.dc_levels.dc_dummy_level = 0;
  io_cfg.dc_levels.dc_data_level = 1;
  io_cfg.on_color_trans_done = &ParallelDisplay::on_trans_done;
  io_cfg.user_ctx = this;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_, &io_cfg, &io_));

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(cfg.rst);
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_, &panel_cfg, &panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, cfg.invert));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, cfg.swap_xy));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, cfg.mirror_x, cfg.mirror_y));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_, cfg.gap_x, cfg.gap_y));

  // Match LilyGO's ST7789V setup: this panel needs its vendor power/gamma
  // registers after the generic esp_lcd reset/init sequence.
  static constexpr uint8_t kVendorInit[][15] = {
      {0x11},
      {0x3A, 0x05},
      {0xB2, 0x0B, 0x0B, 0x00, 0x33, 0x33},
      {0xB7, 0x75},
      {0xBB, 0x28},
      {0xC0, 0x2C},
      {0xC2, 0x01},
      {0xC3, 0x1F},
      {0xC6, 0x13},
      {0xD0, 0xA7},
      {0xD0, 0xA4, 0xA1},
      {0xD6, 0xA1},
      {0xE0, 0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32},
      {0xE1, 0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37},
  };
  static constexpr uint8_t kVendorInitLen[] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 1, 2, 1, 14, 14};
  for (size_t i = 0; i < std::size(kVendorInit); ++i) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_, kVendorInit[i][0],
                                             kVendorInitLen[i] ? &kVendorInit[i][1] : nullptr,
                                             kVendorInitLen[i]));
    if (i == 0) vTaskDelay(pdMS_TO_TICKS(120));  // sleep-out settling time from the panel spec
  }
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

  if (cfg.backlight >= 0) set_backlight(100);
  ESP_LOGI(TAG, "ST7789 %ux%u ready on the i80 bus (gap %d,%d)", cfg.width, cfg.height, cfg.gap_x, cfg.gap_y);
  return true;
}

hg::DisplayInfo ParallelDisplay::info() const {
  hg::DisplayInfo di;
  di.width = cfg_.width;
  di.height = cfg_.height;
  di.swap_bytes = true;  // the panel wants big-endian RGB565
  di.has_backlight = cfg_.backlight >= 0;
  return di;
}

void ParallelDisplay::flush(uint16_t y0, uint16_t y1) {
  const int w = cfg_.width;
  for (int y = y0; y < y1; y += bounce_rows_) {
    int rows = std::min<int>(bounce_rows_, y1 - y);
    std::memcpy(bounce_, fb_ + static_cast<size_t>(y) * w, static_cast<size_t>(rows) * w * 2);
    esp_lcd_panel_draw_bitmap(panel_, 0, y, w, y + rows, bounce_);
    // The bounce buffer is reused: wait until the transfer has finished.
    xSemaphoreTake(done_, pdMS_TO_TICKS(100));
  }
}

void ParallelDisplay::set_backlight(uint8_t percent) {
  if (cfg_.backlight < 0) return;
  percent = std::min<uint8_t>(percent, 100);
  const uint8_t level = percent == 0 ? 0 : std::max<uint8_t>(1, static_cast<uint8_t>(percent * 16 / 100));
  set_aw9364(cfg_.backlight, level, backlight_level_);
}

}  // namespace hgp