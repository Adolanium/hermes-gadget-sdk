// Elecrow CrowPanel 2.1-inch RGB display and PCF8574 setup.
// RGB wiring and power sequencing follow Elecrow's official Arduino example.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"
#include "crowpanel_st7701_init.hpp"
#include "freertos/task.h"

namespace hgp {
namespace {
const char* TAG = "hg.lcd.rgb";
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_1;
uint8_t expander_output = 0xff;

// The panel data lanes are electrically arranged in BGR order while the SDK
// canvas emits native RGB565. Match the vendor sketch's explicit field swap.
uint16_t swap_rgb565_red_blue(uint16_t pixel) {
  return static_cast<uint16_t>((pixel & 0x07e0u) | ((pixel & 0x001fu) << 11) | ((pixel & 0xf800u) >> 11));
}

bool expander_write(i2c_master_dev_handle_t dev, uint8_t value) {
  expander_output = value;
  return i2c_master_transmit(dev, &expander_output, 1, 50) == ESP_OK;
}

bool expander_set(i2c_master_dev_handle_t dev, uint8_t bit, bool high) {
  const uint8_t value = high ? static_cast<uint8_t>(expander_output | bit)
                             : static_cast<uint8_t>(expander_output & ~bit);
  return expander_write(dev, value);
}

}  // namespace

bool RgbDisplay::on_color_done(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<RgbDisplay*>(ctx)->done_, &woken);
  return woken == pdTRUE;
}

bool RgbDisplay::begin(const LcdConfig& cfg, i2c_master_bus_handle_t i2c_bus) {
  cfg_ = cfg;
  if (!i2c_bus || cfg.rgb.i2c_expander < 0) return false;

  i2c_device_config_t expander_cfg = {};
  expander_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  expander_cfg.device_address = cfg.rgb.i2c_expander;
  expander_cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(i2c_bus, &expander_cfg, &expander_) != ESP_OK) return false;

  // A PCF8574 powers the panel and controls panel/touch reset signals.
  // P3 powers LCD; P4 resets LCD; P0 resets touch; P2 enables touch IRQ.
  if (!expander_set(expander_, 1u << 3, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!expander_set(expander_, 1u << 4, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!expander_set(expander_, 1u << 4, false)) return false;
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!expander_set(expander_, 1u << 4, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!expander_set(expander_, 1u << 0, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!expander_set(expander_, 1u << 0, false)) return false;
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!expander_set(expander_, 1u << 0, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(120));
  if (!expander_set(expander_, 1u << 2, true)) return false;
  vTaskDelay(pdMS_TO_TICKS(120));
  // Release every unused PCF8574 line to its pulled-up input state.
  if (!expander_set(expander_, 0xff, true)) return false;

  fb_ = static_cast<uint16_t*>(heap_caps_malloc(
      static_cast<size_t>(cfg.width) * cfg.height * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!fb_) {
    ESP_LOGE(TAG, "unable to allocate %ux%u application framebuffer in PSRAM", cfg.width, cfg.height);
    return false;
  }
  std::memset(fb_, 0, static_cast<size_t>(cfg.width) * cfg.height * sizeof(uint16_t));
  // Rows are packed here with red and blue swapped; allocated once, not per flush.
  staging_ = static_cast<uint16_t*>(heap_caps_malloc(
      static_cast<size_t>(cfg.width) * cfg.height * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!staging_) {
    ESP_LOGE(TAG, "unable to allocate the %ux%u flush buffer in PSRAM", cfg.width, cfg.height);
    return false;
  }
  done_ = xSemaphoreCreateBinary();
  if (!done_) return false;

  spi_line_config_t lines = {};
  lines.cs_io_type = IO_TYPE_GPIO;
  lines.cs_gpio_num = static_cast<gpio_num_t>(cfg.rgb.cmd_cs);
  lines.scl_io_type = IO_TYPE_GPIO;
  lines.scl_gpio_num = static_cast<gpio_num_t>(cfg.rgb.cmd_sclk);
  lines.sda_io_type = IO_TYPE_GPIO;
  lines.sda_gpio_num = static_cast<gpio_num_t>(cfg.rgb.cmd_sda);
  lines.io_expander = nullptr;
  esp_lcd_panel_io_3wire_spi_config_t io_cfg = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(lines, 0);
  esp_err_t io_err = esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io_);
  if (io_err != ESP_OK) {
    ESP_LOGE(TAG, "3-wire ST7701 panel IO init failed: %s", esp_err_to_name(io_err));
    return false;
  }

  esp_lcd_rgb_panel_config_t rgb_cfg = {};
  rgb_cfg.clk_src = LCD_CLK_SRC_PLL160M;
  rgb_cfg.data_width = 16;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  // IDF 6 replaced the bpp/alignment fields with explicit color formats; the
  // PSRAM alignment is now handled by the driver.
  rgb_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
  rgb_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
#else
  rgb_cfg.bits_per_pixel = 16;
  rgb_cfg.psram_trans_align = 64;
#endif
  rgb_cfg.de_gpio_num = static_cast<gpio_num_t>(cfg.rgb.de);
  rgb_cfg.vsync_gpio_num = static_cast<gpio_num_t>(cfg.rgb.vsync);
  rgb_cfg.hsync_gpio_num = static_cast<gpio_num_t>(cfg.rgb.hsync);
  rgb_cfg.pclk_gpio_num = static_cast<gpio_num_t>(cfg.rgb.pclk);
  // No DISP line is wired. Left at zero, disp_gpio_num would drive GPIO 0, the
  // BOOT key, high; NC makes the ST7701 driver send DISPON over the 3-wire IO.
  rgb_cfg.disp_gpio_num = GPIO_NUM_NC;
  for (int i = 0; i < 16; ++i) rgb_cfg.data_gpio_nums[i] = static_cast<gpio_num_t>(cfg.rgb.data[i]);
  rgb_cfg.timings.pclk_hz = cfg.rgb.pclk_hz;
  rgb_cfg.timings.h_res = cfg.width;
  rgb_cfg.timings.v_res = cfg.height;
  rgb_cfg.timings.hsync_pulse_width = 4;
  rgb_cfg.timings.hsync_back_porch = 20;
  rgb_cfg.timings.hsync_front_porch = 10;
  rgb_cfg.timings.vsync_pulse_width = 4;
  rgb_cfg.timings.vsync_back_porch = 20;
  rgb_cfg.timings.vsync_front_porch = 10;
  rgb_cfg.timings.flags.hsync_idle_low = false;
  rgb_cfg.timings.flags.vsync_idle_low = false;
  rgb_cfg.timings.flags.de_idle_high = false;
  rgb_cfg.timings.flags.pclk_active_neg = false;
  rgb_cfg.timings.flags.pclk_idle_high = false;
  // Allocate the RGB scanout framebuffer in PSRAM alongside the app framebuffer.
  rgb_cfg.num_fbs = 1;
  rgb_cfg.flags.fb_in_psram = true;
  rgb_cfg.bounce_buffer_size_px = cfg.width * 20;

  st7701_vendor_config_t vendor = {};
  vendor.rgb_config = &rgb_cfg;
  vendor.init_cmds = kCrowPanelInitCommands;
  vendor.init_cmds_size = sizeof(kCrowPanelInitCommands) / sizeof(kCrowPanelInitCommands[0]);
  vendor.flags.auto_del_panel_io = 0;
  vendor.flags.mirror_by_cmd = 1;
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = GPIO_NUM_NC;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_cfg.bits_per_pixel = 16;
  panel_cfg.vendor_config = &vendor;
  if (!esp_ok(esp_lcd_new_panel_st7701(io_, &panel_cfg, &panel_), TAG, "esp_lcd_new_panel_st7701")) return false;
  esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
  callbacks.on_color_trans_done = &RgbDisplay::on_color_done;
  if (!esp_ok(esp_lcd_rgb_panel_register_event_callbacks(panel_, &callbacks, this), TAG, "esp_lcd_rgb_panel_register_event_callbacks")) return false;
  void* scanout_fb = nullptr;
  if (!esp_ok(esp_lcd_rgb_panel_get_frame_buffer(panel_, 1, &scanout_fb), TAG, "esp_lcd_rgb_panel_get_frame_buffer")) return false;
  ESP_LOGI(TAG, "RGB scanout framebuffer allocated at %p", scanout_fb);
  if (!esp_ok(esp_lcd_panel_reset(panel_), TAG, "esp_lcd_panel_reset")) return false;
  if (!esp_ok(esp_lcd_panel_init(panel_), TAG, "esp_lcd_panel_init")) return false;
  if (!esp_ok(esp_lcd_panel_disp_on_off(panel_, true), TAG, "esp_lcd_panel_disp_on_off")) return false;

  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_1;
  timer.freq_hz = 5000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  if (!esp_ok(ledc_timer_config(&timer), TAG, "ledc_timer_config")) return false;
  ledc_channel_config_t channel = {};
  channel.gpio_num = cfg.backlight;
  channel.speed_mode = LEDC_LOW_SPEED_MODE;
  channel.channel = kBacklightChannel;
  channel.timer_sel = LEDC_TIMER_1;
  channel.duty = 0;
  channel.flags.output_invert = cfg.backlight_invert;
  if (!esp_ok(ledc_channel_config(&channel), TAG, "ledc_channel_config")) return false;
  set_backlight(80);
  ESP_LOGI(TAG, "ST7701 %ux%u RGB panel initialized at %lu Hz", cfg.width, cfg.height,
           static_cast<unsigned long>(cfg.rgb.pclk_hz));
  return true;
}

hg::DisplayInfo RgbDisplay::info() const {
  hg::DisplayInfo info;
  info.width = cfg_.width;
  info.height = cfg_.height;
  info.swap_bytes = false;
  info.has_backlight = true;
  info.round = cfg_.round;
  return info;
}

void RgbDisplay::flush(uint16_t y0, uint16_t y1) {
  if (y0 >= y1 || !panel_) return;
  uint16_t* packed = staging_;
  for (uint16_t row = y0; row < y1; ++row) {
    const size_t src = static_cast<size_t>(row) * cfg_.width;
    const size_t dst = static_cast<size_t>(row - y0) * cfg_.width;
    for (uint16_t col = 0; col < cfg_.width; ++col) {
      const uint16_t pixel = fb_[src + col];
      packed[dst + col] = swap_rgb565_red_blue(pixel);
    }
  }
  // The RGB driver copies from `packed` into its own scanout buffer, so a
  // failure or a late completion here costs one frame, never a reboot.
  esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, 0, y0, cfg_.width, y1, packed);
  if (err != ESP_OK) {
    if (!draw_failed_) ESP_LOGE(TAG, "RGB flush failed: %s (reported once)", esp_err_to_name(err));
    draw_failed_ = true;
    return;
  }
  if (xSemaphoreTake(done_, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "RGB flush completion timed out for y=%u..%u", y0, y1);
  }
}

void RgbDisplay::set_backlight(uint8_t percent) {
  const uint32_t duty = (1023u * std::min<uint8_t>(percent, 100)) / 100u;
  if (ledc_set_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel, duty) == ESP_OK)
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kBacklightChannel);
}

}  // namespace hgp
