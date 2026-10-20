// Elecrow CrowPanel Advanced 10.1-inch ESP32-P4: 1024x600 MIPI-DSI panel with
// an EK79007 controller, and GT911 capacitive touch on I2C0.
//
// The DSI bus uses dedicated SoC peripheral pins (the MIPI-DSI host maps them
// itself), so esp_lcd_new_dsi_bus() takes no pin numbers. The PHY runs from an
// internal LDO channel (2500 mV), which must be acquired before the bus is
// created or the panel never receives a signal.
//
// Hardware facts from Elecrow V1.1 at pinned commit 6c7c851f3105349dc24b12ad058655ba6afa98ee; no vendor code is copied.
//   panel            1024x600, RGB565, 2 data lanes @ 900 Mbps, DPI clock 51 MHz
//   DSI PHY power    internal LDO channel 3 @ 2500 mV
//   LCD reset        NC (not bonded) — the EK79007 driver skips the pulse
//   backlight        GPIO31, active-high PWM
//   touch GT911      I2C0 SDA GPIO45, SCL GPIO46, INT GPIO42, RST GPIO40
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstring>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
// Both LCD headers guard their contents on SOC_MIPI_DSI_SUPPORTED, so the
// capability header has to come first or the DSI types silently disappear.
#include "soc/soc_caps.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.dsi";

constexpr int kBacklightChannelNum = 0;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;

// Every flush call is wrapped in a spin that ends either on a registered
// callback or after this many microseconds. A finished copy and a DMA that
// never completed are otherwise indistinguishable.
constexpr uint32_t kCopySpinUs = 40000;
// One flush is normally 512 KB: ~24 us on the P4's own bus. A poll interval of
// 100 us keeps the app task free between iterations.
constexpr uint32_t kCopyPollUs = 100;

// The LDO handle is held for the process lifetime: releasing it would cut the
// PHY rail out from under a running display.
esp_ldo_channel_handle_t g_dsi_phy_ldo = nullptr;

// Set from the DMA2D completion callback, read by flush(). An atomic rather
// than a semaphore: the copy is normally already done by the time it is read,
// and the DSI host keeps scanning out regardless.
std::atomic<bool> g_dsi_copy_done{false};

}  // namespace

// Runs in the DMA2D completion ISR. Only one DSI panel exists on a board, so a
// single flag is enough; flush() reads it with acquire ordering.
bool DsiDisplay::on_color_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  g_dsi_copy_done.store(true, std::memory_order_release);
  return false;
}

bool DsiDisplay::begin(const LcdConfig& cfg, const DsiPanelConfig& dsi) {
  cfg_ = cfg;
  dsi_ = dsi;
  use_dma2d_ = dsi_.use_dma2d;

  // 1. Power the DSI PHY. Without this the bus configures cleanly and the panel
  //    stays dark, which is the single most misleading failure on this board.
  esp_ldo_channel_config_t ldo = {};
  ldo.chan_id = dsi_.ldo_channel;
  ldo.voltage_mv = dsi_.ldo_voltage_mv;
  const esp_err_t ldo_err = esp_ldo_acquire_channel(&ldo, &g_dsi_phy_ldo);
  if (ldo_err != ESP_OK) {
    ESP_LOGE(TAG, "DSI PHY LDO channel %d (%d mV) unavailable: %s", dsi_.ldo_channel, dsi_.ldo_voltage_mv,
             esp_err_to_name(ldo_err));
    return false;
  }
  ESP_LOGI(TAG, "DSI PHY powered from internal LDO channel %d at %d mV", dsi_.ldo_channel, dsi_.ldo_voltage_mv);

  // 2. The DSI bus. Lanes and bit rate come from the board's DsiPanelConfig.
  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = dsi_.lanes;
  bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus_cfg.lane_bit_rate_mbps = static_cast<float>(dsi_.lane_bit_rate_mbps);
  esp_err_t err = esp_lcd_new_dsi_bus(&bus_cfg, &bus_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "MIPI-DSI bus (2 lanes) failed: %s", esp_err_to_name(err));
    return false;
  }

  // 3. The DBI command interface, used only for the EK79007's init sequence.
  esp_lcd_dbi_io_config_t dbi_cfg = {};
  dbi_cfg.virtual_channel = 0;
  dbi_cfg.lcd_cmd_bits = 8;
  dbi_cfg.lcd_param_bits = 8;
  err = esp_lcd_new_panel_io_dbi(bus_, &dbi_cfg, &io_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "MIPI-DBI panel IO failed: %s", esp_err_to_name(err));
    return false;
  }

  // 4. DPI timing. One framebuffer: the app owns a second one and each flush
  //    is a copy into this scanout buffer.
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.virtual_channel = 0;
  dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi.dpi_clock_freq_mhz = static_cast<float>(dsi_.dpi_clock_mhz);
  dpi.in_color_format = LCD_COLOR_FMT_RGB565;
  dpi.out_color_format = LCD_COLOR_FMT_RGB565;
  dpi.num_fbs = 1;
  dpi.video_timing.h_size = cfg.width;
  dpi.video_timing.v_size = cfg.height;
  dpi.video_timing.hsync_pulse_width = dsi_.hsync_pulse_width;
  dpi.video_timing.hsync_back_porch = dsi_.hsync_back_porch;
  dpi.video_timing.hsync_front_porch = dsi_.hsync_front_porch;
  dpi.video_timing.vsync_pulse_width = dsi_.vsync_pulse_width;
  dpi.video_timing.vsync_back_porch = dsi_.vsync_back_porch;
  dpi.video_timing.vsync_front_porch = dsi_.vsync_front_porch;

  // 5. The EK79007 panel driver. reset_gpio_num is GPIO_NUM_NC: the V1.1 board
  //    does not bond an LCD reset line.
  ek79007_vendor_config_t vendor = {};
  vendor.mipi_config.dsi_bus = bus_;
  vendor.mipi_config.dpi_config = &dpi;
  vendor.mipi_config.lane_num = dsi_.lanes;

  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = GPIO_NUM_NC;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  panel_cfg.vendor_config = &vendor;
  err = esp_lcd_new_panel_ek79007(io_, &panel_cfg, &panel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "EK79007 panel creation failed: %s", esp_err_to_name(err));
    return false;
  }

  // The scanout buffer belongs to the DPI driver, not to us; we copy into it.
  void* scanout = nullptr;
  err = esp_lcd_dpi_panel_get_frame_buffer(panel_, 1, &scanout);
  if (err != ESP_OK || !scanout) {
    ESP_LOGE(TAG, "scanout framebuffer unavailable: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "EK79007 scanout framebuffer at %p", scanout);

  // 6. Our own full-frame framebuffer, the one the core's canvas draws into.
  //    PSRAM first (32 MB present); it is only read by the copy, never by DMA.
  const size_t bytes = static_cast<size_t>(cfg.width) * cfg.height * sizeof(uint16_t);
  fb_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!fb_) fb_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  if (!fb_) {
    ESP_LOGE(TAG, "no memory for the %ux%u framebuffer", cfg.width, cfg.height);
    return false;
  }
  std::memset(fb_, 0, bytes);

  if (use_dma2d_) {
    err = esp_lcd_dpi_panel_enable_dma2d(panel_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "DMA2D unavailable (%s); falling back to the CPU copy path", esp_err_to_name(err));
      use_dma2d_ = false;
    }
  }

  if (use_dma2d_) {
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = &DsiDisplay::on_color_done;
    err = esp_lcd_dpi_panel_register_event_callbacks(panel_, &cbs, this);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "DMA2D completion callbacks unavailable (%s); falling back to the CPU copy path",
               esp_err_to_name(err));
      use_dma2d_ = false;
    }
  }

  err = esp_lcd_panel_reset(panel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "panel reset failed: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_lcd_panel_init(panel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
    return false;
  }
  // A DPI panel is scanning out from the moment it is initialised; it has no
  // disp_on_off operation, and calling it fails with ESP_ERR_NOT_SUPPORTED.
  // Accepting that specific code keeps a genuinely missing operation from
  // masking a real init failure.
  err = esp_lcd_panel_disp_on_off(panel_, true);
  if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGE(TAG, "panel display-on failed: %s", esp_err_to_name(err));
    return false;
  }

  // 7. Backlight. GPIO31 active-high is a hardware fact from the pinned Elecrow
  //    V1.1 config.h and schematic (see docs/hardware.md). Start dark; the core raises it.
  if (cfg.backlight >= 0) {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = kBacklightTimer;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) == ESP_OK) {
      ledc_channel_config_t channel = {};
      channel.gpio_num = static_cast<gpio_num_t>(cfg.backlight);
      channel.speed_mode = LEDC_LOW_SPEED_MODE;
      channel.channel = static_cast<ledc_channel_t>(kBacklightChannelNum);
      channel.timer_sel = kBacklightTimer;
      channel.duty = 0;
      channel.flags.output_invert = cfg.backlight_invert;
      if (ledc_channel_config(&channel) != ESP_OK) ESP_LOGW(TAG, "backlight PWM on GPIO%d failed", cfg.backlight);
    }
  }

  ESP_LOGI(TAG, "ek79007 %ux%u DSI ready (lanes %d @ %d Mbps, DPI %d MHz, %s copy)", cfg.width, cfg.height, dsi_.lanes,
           dsi_.lane_bit_rate_mbps, dsi_.dpi_clock_mhz, use_dma2d_ ? "DMA2D" : "CPU");
  return true;
}

hg::DisplayInfo DsiDisplay::info() const {
  hg::DisplayInfo di;
  di.width = cfg_.width;
  di.height = cfg_.height;
  di.swap_bytes = false;  // RGB565 little-endian; the DPI controller takes it natively
  di.has_backlight = cfg_.backlight >= 0;
  di.round = cfg_.round;
  return di;
}

bool DsiDisplay::wait_copy(uint32_t timeout_us) {
  if (!use_dma2d_) return true;  // the CPU path is complete on return
  const uint64_t deadline = esp_timer_get_time() + timeout_us;
  while (!g_dsi_copy_done.load(std::memory_order_acquire)) {
    if (esp_timer_get_time() >= deadline) return false;
    esp_rom_delay_us(kCopyPollUs);
  }
  g_dsi_copy_done.store(false, std::memory_order_release);
  return true;
}

void DsiDisplay::flush(uint16_t y0, uint16_t y1) {
  if (!panel_ || y0 >= y1) return;
  // esp_lcd_panel_draw_bitmap() treats color_data as the first pixel of the
  // sub-window [x0,y0)-(x1,y1), NOT the base of the full framebuffer. Passing
  // fb_ would blit the top of the frame into every band, so the bottom band
  // re-drew the top-bar rows (status text repeated down the screen). Offset
  // the source by the starting row.
  const esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, 0, y0, cfg_.width, y1, &fb_[static_cast<size_t>(y0) * cfg_.width]);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "flush of rows %u..%u failed: %s", y0, y1, esp_err_to_name(err));
    return;
  }
  ++flushes_;
  if (!wait_copy(kCopySpinUs)) ESP_LOGW(TAG, "rows %u..%u: copy did not complete in %u us", y0, y1, kCopySpinUs);
}

void DsiDisplay::set_backlight(uint8_t percent) {
  if (cfg_.backlight < 0) return;
  const uint32_t duty = (1023u * std::min<uint8_t>(percent, 100)) / 100u;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(kBacklightChannelNum), duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(kBacklightChannelNum));
}

}  // namespace hgp
