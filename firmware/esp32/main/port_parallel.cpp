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
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

namespace hgp {
namespace {

const char* TAG = "hg.lcd.i80";
constexpr int kBounceRows = 20;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;

// Some LCD modules need the write strobe held high while nothing is written.
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

  esp_lcd_i80_bus_config_t bus = {};
  bus.clk_src = LCD_CLK_SRC_PLL160M;
  bus.dc_gpio_num = cfg.dc;
  bus.wr_gpio_num = cfg.bus.wr;
  bus.bus_width = 8;
  for (int i = 0; i < 8; ++i) bus.data_gpio_nums[i] = cfg.bus.data[i];
  bus.max_transfer_bytes = static_cast<int>(static_cast<size_t>(cfg.width) * bounce_rows_ * 2);
  bus.psram_trans_align = 0;
  bus.sram_trans_align = 0;
  ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus, &i80_));
  park_wr(cfg.bus.wr);

  esp_lcd_panel_io_i80_config_t io_cfg = {};
  io_cfg.cs_gpio_num = cfg.cs;
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
  panel_cfg.reset_gpio_num = cfg.rst;
  panel_cfg.color_space = ESP_LCD_COLOR_SPACE_RGB;
  panel_cfg.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_, &panel_cfg, &panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, cfg.invert));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, cfg.swap_xy));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, cfg.mirror_x, cfg.mirror_y));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_, cfg.gap_x, cfg.gap_y));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

  if (cfg.backlight >= 0) {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch = {};
    ch.gpio_num = cfg.backlight;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = kBlChannel;
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    set_backlight(100);
  }
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
  uint32_t duty = (1023u * std::min<uint8_t>(percent, 100)) / 100u;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kBlChannel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kBlChannel);
}

}  // namespace hgp