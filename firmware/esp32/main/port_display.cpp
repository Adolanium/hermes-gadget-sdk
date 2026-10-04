// SPI ST7789 panel via esp_lcd. The full framebuffer lives in PSRAM; rows are
// copied through a small DMA-capable bounce buffer on flush.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "panel_box3.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.lcd";
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr int kBounceRows = 20;
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;

}  // namespace

bool SpiDisplay::on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SpiDisplay*>(ctx)->done_, &woken);
  return woken == pdTRUE;
}

bool SpiDisplay::begin(const LcdConfig& cfg, i2c_master_bus_handle_t bus) {
  cfg_ = cfg;
  bool ili9341 = false;
  if (cfg.controller == LcdController::Box3) {
    if (!bus) return false;
    if (i2c_master_probe(bus, 0x24, 50) != ESP_OK) {
      if (i2c_master_probe(bus, 0x5d, 50) != ESP_OK && i2c_master_probe(bus, 0x14, 50) != ESP_OK) {
        ESP_LOGE(TAG, "BOX-3 display revision could not be detected");
        return false;
      }
      ili9341 = true;
    }
  }
  controller_name_ = ili9341 ? "ili9342" : "st7789";
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

  spi_bus_config_t bus = {};
  bus.mosi_io_num = cfg.mosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = cfg.sclk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = cfg.width * bounce_rows_ * 2;
  ESP_ERROR_CHECK(spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.dc_gpio_num = static_cast<gpio_num_t>(cfg.dc);
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(cfg.cs);
  io_cfg.pclk_hz = static_cast<uint32_t>(cfg.spi_mhz) * 1000 * 1000;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  io_cfg.spi_mode = 0;
  io_cfg.trans_queue_depth = 4;
  io_cfg.on_color_trans_done = &SpiDisplay::on_trans_done;
  io_cfg.user_ctx = this;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kHost), &io_cfg, &io_));

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(cfg.rst);
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  panel_cfg.flags.reset_active_high = cfg.reset_active_high;
  ili9341_vendor_config_t vendor = {};
  if (ili9341) {
    vendor.init_cmds = kBox3PanelInit;
    vendor.init_cmds_size = sizeof(kBox3PanelInit) / sizeof(kBox3PanelInit[0]);
    panel_cfg.vendor_config = &vendor;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_, &panel_cfg, &panel_));
  } else {
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_, &panel_cfg, &panel_));
  }
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
  ESP_LOGI(TAG, "%s %ux%u ready", controller_name_, cfg.width, cfg.height);
  return true;
}

hg::DisplayInfo SpiDisplay::info() const {
  hg::DisplayInfo di;
  di.width = cfg_.width;
  di.height = cfg_.height;
  di.swap_bytes = true;  // the panel wants big-endian RGB565
  di.has_backlight = cfg_.backlight >= 0;
  return di;
}

void SpiDisplay::flush(uint16_t y0, uint16_t y1) {
  const int w = cfg_.width;
  for (int y = y0; y < y1; y += bounce_rows_) {
    int rows = std::min<int>(bounce_rows_, y1 - y);
    std::memcpy(bounce_, fb_ + static_cast<size_t>(y) * w, static_cast<size_t>(rows) * w * 2);
    esp_lcd_panel_draw_bitmap(panel_, 0, y, w, y + rows, bounce_);
    // The bounce buffer is reused: wait until the DMA transfer has finished.
    xSemaphoreTake(done_, pdMS_TO_TICKS(100));
  }
}

void SpiDisplay::set_backlight(uint8_t percent) {
  if (cfg_.backlight < 0) return;
  uint32_t duty = (1023u * std::min<uint8_t>(percent, 100)) / 100u;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kBlChannel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kBlChannel);
}

}  // namespace hgp
