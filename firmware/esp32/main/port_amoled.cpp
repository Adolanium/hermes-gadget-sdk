// QSPI AMOLED panel with a CO5300 controller (e.g. the round 466x466 1.75"
// modules), driven directly through esp_lcd panel IO in quad mode.
//
// Framing on the QSPI link: commands go out as (0x02 << 24) | (cmd << 8) with
// their parameters; pixels as (0x32 << 24) | (RAMWR << 8). The controller only
// accepts windows that start on an even row/column and span an even count.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.amoled";
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr int kBounceRows = 16;  // even, so every chunk keeps the window even

constexpr uint32_t command_word(uint8_t cmd) { return (0x02u << 24) | (static_cast<uint32_t>(cmd) << 8); }
constexpr uint32_t kRamWrite = (0x32u << 24) | (0x2Cu << 8);

struct InitCommand {
  uint8_t cmd;
  uint8_t data[4];
  uint8_t len;
  uint16_t delay_ms;
};

// Panel bring-up for CO5300 1.75" modules: vendor page settings, RGB565,
// tearing line on, full brightness, the 466x466 window (column offset 6),
// then sleep out and display on.
constexpr InitCommand kInit466[] = {
    {0x36, {0x00}, 1, 0},  // memory access control: no rotation
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0xFE, {0x20}, 1, 0},
    {0x19, {0x10}, 1, 0},
    {0x1C, {0xA0}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, {}, 0, 600},  // sleep out
    {0x29, {}, 0, 0},    // display on
};

// Panel bring-up for the rectangular 368x448 1.8" module (V2: CO5300 + CST820),
// following Waveshare's own board example: no MADCTL write and no page-2
// registers, only the vendor page, RGB565, tearing, brightness and the window
// (column offset 16 is applied per flush through gap_x). The window set here is
// the full 368x448 glass; every flush rewrites it.
constexpr InitCommand kInit368[] = {
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, {}, 0, 100},  // sleep out
    {0x29, {}, 0, 0},    // display on
};

}  // namespace

bool AmoledDisplay::on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<AmoledDisplay*>(ctx)->done_, &woken);
  if (woken) portYIELD_FROM_ISR();  // the SPI panel IO ignores this callback's return value
  return false;
}

void AmoledDisplay::command(uint8_t cmd, const uint8_t* data, size_t len) {
  esp_lcd_panel_io_tx_param(io_, static_cast<int>(command_word(cmd)), len ? data : nullptr, len);
}

bool AmoledDisplay::begin(const AmoledConfig& cfg) {
  cfg_ = cfg;
  const size_t px = static_cast<size_t>(cfg.width) * cfg.height;
  fb_ = static_cast<uint16_t*>(heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  bounce_ = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(cfg.width) * kBounceRows * 2, MALLOC_CAP_DMA));
  if (!fb_ || !bounce_) {
    ESP_LOGE(TAG, "not enough memory for a %ux%u framebuffer (is PSRAM enabled?)", cfg.width, cfg.height);
    return false;
  }
  std::memset(fb_, 0, px * 2);
  done_ = xSemaphoreCreateBinary();

  spi_bus_config_t bus = {};
  bus.sclk_io_num = cfg.sclk;
  bus.data0_io_num = cfg.d0;
  bus.data1_io_num = cfg.d1;
  bus.data2_io_num = cfg.d2;
  bus.data3_io_num = cfg.d3;
  bus.data4_io_num = -1;
  bus.data5_io_num = -1;
  bus.data6_io_num = -1;
  bus.data7_io_num = -1;
  bus.max_transfer_sz = cfg.width * kBounceRows * 2 + 16;
  bus.flags = SPICOMMON_BUSFLAG_QUAD;
  if (!esp_ok(spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus_initialize")) return false;

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(cfg.cs);
  io_cfg.dc_gpio_num = GPIO_NUM_NC;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = static_cast<uint32_t>(cfg.qspi_mhz) * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 32;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = 1;
  io_cfg.on_color_trans_done = &AmoledDisplay::on_trans_done;
  io_cfg.user_ctx = this;
  if (!esp_ok(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kHost), &io_cfg, &io_), TAG, "esp_lcd_new_panel_io_spi")) return false;

  if (cfg.rst >= 0) {
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << cfg.rst;
    rst.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 1);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  const InitCommand* init = kInit466;
  size_t init_len = sizeof(kInit466) / sizeof(kInit466[0]);
  if (cfg.panel == AmoledPanel::Co5300_368) {
    init = kInit368;
    init_len = sizeof(kInit368) / sizeof(kInit368[0]);
  }
  for (size_t i = 0; i < init_len; ++i) {
    command(init[i].cmd, init[i].data, init[i].len);
    if (init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init[i].delay_ms));
  }
  // Each band's DMA transfer must finish before the bounce buffer is refilled.
  bands_.emplace(
      cfg.height, kBounceRows, 100,
      [this](int y, int rows) {
        const int w = cfg_.width;
        std::memcpy(bounce_, fb_ + static_cast<size_t>(y) * w, static_cast<size_t>(rows) * w * 2);
        const int x0 = cfg_.gap_x, x1 = cfg_.gap_x + w - 1;
        const int ya = y + cfg_.gap_y, yb = y + rows - 1 + cfg_.gap_y;
        const uint8_t cols[4] = {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1 >> 8),
                                 static_cast<uint8_t>(x1)};
        const uint8_t lines[4] = {static_cast<uint8_t>(ya >> 8), static_cast<uint8_t>(ya), static_cast<uint8_t>(yb >> 8),
                                  static_cast<uint8_t>(yb)};
        command(0x2A, cols, 4);
        command(0x2B, lines, 4);
        return esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), bounce_, static_cast<size_t>(rows) * w * 2) ==
               ESP_OK;
      },
      [this](uint32_t ms) { return xSemaphoreTake(done_, pdMS_TO_TICKS(ms)) == pdTRUE; });
  ESP_LOGI(TAG, "CO5300 %ux%u ready", cfg.width, cfg.height);
  return true;
}

hg::DisplayInfo AmoledDisplay::info() const {
  hg::DisplayInfo di;
  di.width = cfg_.width;
  di.height = cfg_.height;
  di.swap_bytes = true;  // big-endian RGB565 on the wire
  di.has_backlight = true;  // brightness command 0x51
  di.round = cfg_.round;
  di.corner_inset = cfg_.corner_inset;
  return di;
}

void AmoledDisplay::flush(uint16_t y0, uint16_t y1) {
  // Even start and even span: widen the dirty rows by at most one on each side.
  const int top = y0 & ~1;
  const int bottom = std::min<int>(cfg_.height, (y1 + 1) & ~1);
  report_band_event(TAG, bands_->flush(top, bottom));
}

void AmoledDisplay::set_backlight(uint8_t percent) {
  const uint8_t level = static_cast<uint8_t>(255u * std::min<uint8_t>(percent, 100) / 100u);
  // Zero is screen sleep: turn the panel's output off too (display off keeps
  // its memory, so drawing continues and waking needs no redraw delay).
  if (level && !panel_on_) command(0x29, nullptr, 0);  // display on
  command(0x51, &level, 1);
  if (!level && panel_on_) command(0x28, nullptr, 0);  // display off
  panel_on_ = level != 0;
}

}  // namespace hgp
