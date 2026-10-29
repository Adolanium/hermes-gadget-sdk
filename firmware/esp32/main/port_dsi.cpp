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
#include "esp_lcd_jd9365.h"
#include "esp_idf_version.h"
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

// JD9365 800x800 panel init sequence (Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C).
// The values and their order follow Waveshare's board support package
// waveshare/esp32_p4_wifi6_touch_lcd_xc 2.0.0 (the CONFIG_BSP_LCD_TYPE_800_800_3_4_INCH
// table in esp32_p4_wifi6_touch_lcd_xc.c), Apache License 2.0; see NOTICE. Unchanged.
static const jd9365_lcd_init_cmd_t kJd9365InitCmds[] = {
    {0xE0, (uint8_t[]){0x00}, 1, 0},

    {0xE1, (uint8_t[]){0x93}, 1, 0},
    {0xE2, (uint8_t[]){0x65}, 1, 0},
    {0xE3, (uint8_t[]){0xF8}, 1, 0},
    {0x80, (uint8_t[]){0x01}, 1, 0},

    {0xE0, (uint8_t[]){0x01}, 1, 0},

    {0x00, (uint8_t[]){0x00}, 1, 0},
    {0x01, (uint8_t[]){0x41}, 1, 0},
    {0x03, (uint8_t[]){0x10}, 1, 0},
    {0x04, (uint8_t[]){0x44}, 1, 0},

    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0xD0}, 1, 0},
    {0x19, (uint8_t[]){0x00}, 1, 0},
    {0x1A, (uint8_t[]){0x00}, 1, 0},
    {0x1B, (uint8_t[]){0xD0}, 1, 0},
    {0x1C, (uint8_t[]){0x00}, 1, 0},

    {0x24, (uint8_t[]){0xFE}, 1, 0},
    {0x35, (uint8_t[]){0x26}, 1, 0},

    {0x37, (uint8_t[]){0x09}, 1, 0},

    {0x38, (uint8_t[]){0x04}, 1, 0},
    {0x39, (uint8_t[]){0x08}, 1, 0},
    {0x3A, (uint8_t[]){0x0A}, 1, 0},
    {0x3C, (uint8_t[]){0x78}, 1, 0},
    {0x3D, (uint8_t[]){0xFF}, 1, 0},
    {0x3E, (uint8_t[]){0xFF}, 1, 0},
    {0x3F, (uint8_t[]){0xFF}, 1, 0},

    {0x40, (uint8_t[]){0x00}, 1, 0},
    {0x41, (uint8_t[]){0x64}, 1, 0},
    {0x42, (uint8_t[]){0xC7}, 1, 0},
    {0x43, (uint8_t[]){0x18}, 1, 0},
    {0x44, (uint8_t[]){0x0B}, 1, 0},
    {0x45, (uint8_t[]){0x14}, 1, 0},

    {0x55, (uint8_t[]){0x02}, 1, 0},
    {0x57, (uint8_t[]){0x49}, 1, 0},
    {0x59, (uint8_t[]){0x0A}, 1, 0},
    {0x5A, (uint8_t[]){0x1B}, 1, 0},
    {0x5B, (uint8_t[]){0x19}, 1, 0},

    {0x5D, (uint8_t[]){0x7F}, 1, 0},
    {0x5E, (uint8_t[]){0x56}, 1, 0},
    {0x5F, (uint8_t[]){0x43}, 1, 0},
    {0x60, (uint8_t[]){0x37}, 1, 0},
    {0x61, (uint8_t[]){0x33}, 1, 0},
    {0x62, (uint8_t[]){0x25}, 1, 0},
    {0x63, (uint8_t[]){0x2A}, 1, 0},
    {0x64, (uint8_t[]){0x16}, 1, 0},
    {0x65, (uint8_t[]){0x30}, 1, 0},
    {0x66, (uint8_t[]){0x2F}, 1, 0},
    {0x67, (uint8_t[]){0x32}, 1, 0},
    {0x68, (uint8_t[]){0x53}, 1, 0},
    {0x69, (uint8_t[]){0x43}, 1, 0},
    {0x6A, (uint8_t[]){0x4C}, 1, 0},
    {0x6B, (uint8_t[]){0x40}, 1, 0},
    {0x6C, (uint8_t[]){0x3D}, 1, 0},
    {0x6D, (uint8_t[]){0x31}, 1, 0},
    {0x6E, (uint8_t[]){0x20}, 1, 0},
    {0x6F, (uint8_t[]){0x0F}, 1, 0},

    {0x70, (uint8_t[]){0x7F}, 1, 0},
    {0x71, (uint8_t[]){0x56}, 1, 0},
    {0x72, (uint8_t[]){0x43}, 1, 0},
    {0x73, (uint8_t[]){0x37}, 1, 0},
    {0x74, (uint8_t[]){0x33}, 1, 0},
    {0x75, (uint8_t[]){0x25}, 1, 0},
    {0x76, (uint8_t[]){0x2A}, 1, 0},
    {0x77, (uint8_t[]){0x16}, 1, 0},
    {0x78, (uint8_t[]){0x30}, 1, 0},
    {0x79, (uint8_t[]){0x2F}, 1, 0},
    {0x7A, (uint8_t[]){0x32}, 1, 0},
    {0x7B, (uint8_t[]){0x53}, 1, 0},
    {0x7C, (uint8_t[]){0x43}, 1, 0},
    {0x7D, (uint8_t[]){0x4C}, 1, 0},
    {0x7E, (uint8_t[]){0x40}, 1, 0},
    {0x7F, (uint8_t[]){0x3D}, 1, 0},
    {0x80, (uint8_t[]){0x31}, 1, 0},
    {0x81, (uint8_t[]){0x20}, 1, 0},
    {0x82, (uint8_t[]){0x0F}, 1, 0},

    {0xE0, (uint8_t[]){0x02}, 1, 0},
    {0x00, (uint8_t[]){0x5F}, 1, 0},
    {0x01, (uint8_t[]){0x5F}, 1, 0},
    {0x02, (uint8_t[]){0x5E}, 1, 0},
    {0x03, (uint8_t[]){0x5E}, 1, 0},
    {0x04, (uint8_t[]){0x50}, 1, 0},
    {0x05, (uint8_t[]){0x48}, 1, 0},
    {0x06, (uint8_t[]){0x48}, 1, 0},
    {0x07, (uint8_t[]){0x4A}, 1, 0},
    {0x08, (uint8_t[]){0x4A}, 1, 0},
    {0x09, (uint8_t[]){0x44}, 1, 0},
    {0x0A, (uint8_t[]){0x44}, 1, 0},
    {0x0B, (uint8_t[]){0x46}, 1, 0},
    {0x0C, (uint8_t[]){0x46}, 1, 0},
    {0x0D, (uint8_t[]){0x5F}, 1, 0},
    {0x0E, (uint8_t[]){0x5F}, 1, 0},
    {0x0F, (uint8_t[]){0x57}, 1, 0},
    {0x10, (uint8_t[]){0x57}, 1, 0},
    {0x11, (uint8_t[]){0x77}, 1, 0},
    {0x12, (uint8_t[]){0x77}, 1, 0},
    {0x13, (uint8_t[]){0x40}, 1, 0},
    {0x14, (uint8_t[]){0x42}, 1, 0},
    {0x15, (uint8_t[]){0x5F}, 1, 0},

    {0x16, (uint8_t[]){0x5F}, 1, 0},
    {0x17, (uint8_t[]){0x5F}, 1, 0},
    {0x18, (uint8_t[]){0x5E}, 1, 0},
    {0x19, (uint8_t[]){0x5E}, 1, 0},
    {0x1A, (uint8_t[]){0x50}, 1, 0},
    {0x1B, (uint8_t[]){0x49}, 1, 0},
    {0x1C, (uint8_t[]){0x49}, 1, 0},
    {0x1D, (uint8_t[]){0x4B}, 1, 0},
    {0x1E, (uint8_t[]){0x4B}, 1, 0},
    {0x1F, (uint8_t[]){0x45}, 1, 0},
    {0x20, (uint8_t[]){0x45}, 1, 0},
    {0x21, (uint8_t[]){0x47}, 1, 0},
    {0x22, (uint8_t[]){0x47}, 1, 0},
    {0x23, (uint8_t[]){0x5F}, 1, 0},
    {0x24, (uint8_t[]){0x5F}, 1, 0},
    {0x25, (uint8_t[]){0x57}, 1, 0},
    {0x26, (uint8_t[]){0x57}, 1, 0},
    {0x27, (uint8_t[]){0x77}, 1, 0},
    {0x28, (uint8_t[]){0x77}, 1, 0},
    {0x29, (uint8_t[]){0x41}, 1, 0},
    {0x2A, (uint8_t[]){0x43}, 1, 0},
    {0x2B, (uint8_t[]){0x5F}, 1, 0},

    {0x2C, (uint8_t[]){0x1E}, 1, 0},
    {0x2D, (uint8_t[]){0x1E}, 1, 0},
    {0x2E, (uint8_t[]){0x1F}, 1, 0},
    {0x2F, (uint8_t[]){0x1F}, 1, 0},
    {0x30, (uint8_t[]){0x10}, 1, 0},
    {0x31, (uint8_t[]){0x07}, 1, 0},
    {0x32, (uint8_t[]){0x07}, 1, 0},
    {0x33, (uint8_t[]){0x05}, 1, 0},
    {0x34, (uint8_t[]){0x05}, 1, 0},
    {0x35, (uint8_t[]){0x0B}, 1, 0},
    {0x36, (uint8_t[]){0x0B}, 1, 0},
    {0x37, (uint8_t[]){0x09}, 1, 0},
    {0x38, (uint8_t[]){0x09}, 1, 0},
    {0x39, (uint8_t[]){0x1F}, 1, 0},
    {0x3A, (uint8_t[]){0x1F}, 1, 0},
    {0x3B, (uint8_t[]){0x17}, 1, 0},
    {0x3C, (uint8_t[]){0x17}, 1, 0},
    {0x3D, (uint8_t[]){0x17}, 1, 0},
    {0x3E, (uint8_t[]){0x17}, 1, 0},
    {0x3F, (uint8_t[]){0x03}, 1, 0},
    {0x40, (uint8_t[]){0x01}, 1, 0},
    {0x41, (uint8_t[]){0x1F}, 1, 0},

    {0x42, (uint8_t[]){0x1E}, 1, 0},
    {0x43, (uint8_t[]){0x1E}, 1, 0},
    {0x44, (uint8_t[]){0x1F}, 1, 0},
    {0x45, (uint8_t[]){0x1F}, 1, 0},
    {0x46, (uint8_t[]){0x10}, 1, 0},
    {0x47, (uint8_t[]){0x06}, 1, 0},
    {0x48, (uint8_t[]){0x06}, 1, 0},
    {0x49, (uint8_t[]){0x04}, 1, 0},
    {0x4A, (uint8_t[]){0x04}, 1, 0},
    {0x4B, (uint8_t[]){0x0A}, 1, 0},
    {0x4C, (uint8_t[]){0x0A}, 1, 0},
    {0x4D, (uint8_t[]){0x08}, 1, 0},
    {0x4E, (uint8_t[]){0x08}, 1, 0},
    {0x4F, (uint8_t[]){0x1F}, 1, 0},
    {0x50, (uint8_t[]){0x1F}, 1, 0},
    {0x51, (uint8_t[]){0x17}, 1, 0},
    {0x52, (uint8_t[]){0x17}, 1, 0},
    {0x53, (uint8_t[]){0x17}, 1, 0},
    {0x54, (uint8_t[]){0x17}, 1, 0},
    {0x55, (uint8_t[]){0x02}, 1, 0},
    {0x56, (uint8_t[]){0x00}, 1, 0},
    {0x57, (uint8_t[]){0x1F}, 1, 0},

    {0xE0, (uint8_t[]){0x02}, 1, 0},
    {0x58, (uint8_t[]){0x40}, 1, 0},
    {0x59, (uint8_t[]){0x00}, 1, 0},
    {0x5A, (uint8_t[]){0x00}, 1, 0},
    {0x5B, (uint8_t[]){0x30}, 1, 0},
    {0x5C, (uint8_t[]){0x01}, 1, 0},
    {0x5D, (uint8_t[]){0x30}, 1, 0},
    {0x5E, (uint8_t[]){0x01}, 1, 0},
    {0x5F, (uint8_t[]){0x02}, 1, 0},
    {0x60, (uint8_t[]){0x30}, 1, 0},
    {0x61, (uint8_t[]){0x03}, 1, 0},
    {0x62, (uint8_t[]){0x04}, 1, 0},
    {0x63, (uint8_t[]){0x04}, 1, 0},
    {0x64, (uint8_t[]){0xA6}, 1, 0},
    {0x65, (uint8_t[]){0x43}, 1, 0},
    {0x66, (uint8_t[]){0x30}, 1, 0},
    {0x67, (uint8_t[]){0x73}, 1, 0},
    {0x68, (uint8_t[]){0x05}, 1, 0},
    {0x69, (uint8_t[]){0x04}, 1, 0},
    {0x6A, (uint8_t[]){0x7F}, 1, 0},
    {0x6B, (uint8_t[]){0x08}, 1, 0},
    {0x6C, (uint8_t[]){0x00}, 1, 0},
    {0x6D, (uint8_t[]){0x04}, 1, 0},
    {0x6E, (uint8_t[]){0x04}, 1, 0},
    {0x6F, (uint8_t[]){0x88}, 1, 0},

    {0x75, (uint8_t[]){0xD9}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x33}, 1, 0},
    {0x78, (uint8_t[]){0x43}, 1, 0},

    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},

    {0x29, (uint8_t[]){0x00}, 1, 20},
    {0x35, (uint8_t[]){0x00}, 1, 0},
};


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
  // DsiPanelConfig::phy_clk_auto leaves phy_clk_src at zero so ESP-IDF picks the source
  // for the chip revision (XTAL on P4 rev 3.x, PLL_F20M before). The boards validated
  // before it keep MIPI_DSI_PHY_CLK_SRC_DEFAULT.
  if (!dsi_.phy_clk_auto) bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
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

  // 4. DPI timing. The driver allocates its scanout framebuffers; the app's
  //    framebuffer is copied into the active scanout buffer on each flush.
  esp_lcd_dpi_panel_config_t dpi = {};
  dpi.virtual_channel = 0;
  dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi.dpi_clock_freq_mhz = static_cast<float>(dsi_.dpi_clock_mhz);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  dpi.in_color_format = LCD_COLOR_FMT_RGB565;
#else
  dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
#endif
  dpi.num_fbs = 3;
  dpi.video_timing.h_size = cfg.width;
  dpi.video_timing.v_size = cfg.height;
  dpi.video_timing.hsync_pulse_width = dsi_.hsync_pulse_width;
  dpi.video_timing.hsync_back_porch = dsi_.hsync_back_porch;
  dpi.video_timing.hsync_front_porch = dsi_.hsync_front_porch;
  dpi.video_timing.vsync_pulse_width = dsi_.vsync_pulse_width;
  dpi.video_timing.vsync_back_porch = dsi_.vsync_back_porch;
  dpi.video_timing.vsync_front_porch = dsi_.vsync_front_porch;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
  dpi.flags.use_dma2d = use_dma2d_ ? 1 : 0;
#endif

  // 5. The DSI panel driver. EK79007 (CrowPanel) or JD9365 (Waveshare 3.4C).
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num = (cfg.rst >= 0) ? static_cast<gpio_num_t>(cfg.rst) : GPIO_NUM_NC;
  panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_cfg.bits_per_pixel = 16;
  if (dsi_.controller == DsiController::Jd9365) {
    jd9365_vendor_config_t vendor = {};
    vendor.init_cmds = kJd9365InitCmds;
    vendor.init_cmds_size = sizeof(kJd9365InitCmds) / sizeof(kJd9365InitCmds[0]);
    vendor.mipi_config.dsi_bus = bus_;
    vendor.mipi_config.dpi_config = &dpi;
    vendor.mipi_config.lane_num = dsi_.lanes;
    panel_cfg.vendor_config = &vendor;
    err = esp_lcd_new_panel_jd9365(io_, &panel_cfg, &panel_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "JD9365 panel creation failed: %s", esp_err_to_name(err));
      return false;
    }
    controller_name_ = "jd9365-dsi";
  } else {
    ek79007_vendor_config_t vendor = {};
    vendor.mipi_config.dsi_bus = bus_;
    vendor.mipi_config.dpi_config = &dpi;
    vendor.mipi_config.lane_num = dsi_.lanes;
    panel_cfg.vendor_config = &vendor;
    err = esp_lcd_new_panel_ek79007(io_, &panel_cfg, &panel_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "EK79007 panel creation failed: %s", esp_err_to_name(err));
      return false;
    }
    controller_name_ = "ek79007-dsi";
  }

  // The scanout buffer belongs to the DPI driver, not to us; we copy into it.
  void* scanout = nullptr;
  err = esp_lcd_dpi_panel_get_frame_buffer(panel_, 1, &scanout);
  if (err != ESP_OK || !scanout) {
    ESP_LOGE(TAG, "scanout framebuffer unavailable: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "DPI scanout framebuffer at %p", scanout);

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
  if (cfg.mirror_x || cfg.mirror_y) {
    transform_fb_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!transform_fb_) transform_fb_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    if (!transform_fb_) {
      ESP_LOGE(TAG, "no memory for mirrored %ux%u scanout buffer", cfg.width, cfg.height);
      return false;
    }
  }

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

  // 7. Backlight. Start dark; the core raises it.
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

  ESP_LOGI(TAG, "%s %ux%u DSI ready (lanes %d @ %d Mbps, DPI %d MHz, %s copy)", controller_name_, cfg.width, cfg.height, dsi_.lanes,
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
  // The EK79007's MADCTL mirror command can be ineffective on this panel. When
  // board orientation flags are set, transform the source rows explicitly so
  // the pixels arrive at the DPI scanout buffer in the intended orientation.
  const uint16_t* source = &fb_[static_cast<size_t>(y0) * cfg_.width];
  if (transform_fb_) {
    // Mirrored vertically, the changed rows [y0, y1) land at [height - y1, height - y0).
    if (cfg_.mirror_y) {
      const uint16_t top = cfg_.height - y1;
      y1 = cfg_.height - y0;
      y0 = top;
    }
    for (uint16_t out_y = y0; out_y < y1; ++out_y) {
      const uint16_t in_y = cfg_.mirror_y ? cfg_.height - 1 - out_y : out_y;
      const uint16_t* src_row = &fb_[static_cast<size_t>(in_y) * cfg_.width];
      uint16_t* dst_row = &transform_fb_[static_cast<size_t>(out_y) * cfg_.width];
      if (cfg_.mirror_x) {
        for (uint16_t out_x = 0; out_x < cfg_.width; ++out_x) {
          dst_row[out_x] = src_row[cfg_.width - 1 - out_x];
        }
      } else {
        std::copy_n(src_row, cfg_.width, dst_row);
      }
    }
    source = &transform_fb_[static_cast<size_t>(y0) * cfg_.width];
  }
  // The source pointer is the first pixel of the sub-window [0,y0)-(width,y1).
  const esp_err_t err = esp_lcd_panel_draw_bitmap(panel_, 0, y0, cfg_.width, y1, source);
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
