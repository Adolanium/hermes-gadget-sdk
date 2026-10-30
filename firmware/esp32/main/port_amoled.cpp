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

// Panel bring-up for the round 466x466 1.32" module, following Waveshare's
// esp32_s3_touch_amoled_1_32 BSP (commit b9a73081): the short table, RGB565,
// tearing, brightness, the 466 window (column offset 6) and a 100 ms sleep-out.
// The 1.75" table writes page-2 registers and waits 600 ms, so this glass does
// not use it. Every flush rewrites the window.
constexpr InitCommand kInit132[] = {
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, {}, 0, 100},  // sleep out
    {0x29, {}, 0, 0},    // display on
};

// Panel bring-up for the round 466x466 1.43C module, following Waveshare's
// factory component at commit 8cea11dd. That table is the short page, tearing,
// brightness control and a 120 ms sleep-out. It leaves COLMOD and the window to
// the CO5300 driver, writes brightness 0x00, then calls esp_lcd_panel_mirror
// (true, true) and esp_lcd_panel_set_gap(panel, 8, 0). This driver sends the
// whole sequence itself, so the table below also writes RGB565, the 466 window
// at column 8 (every flush rewrites it from gap_x), MADCTL 0xC0 and full
// brightness. The factory image raises brightness to its 100% default only
// after the first frame.
constexpr InitCommand kInit143c[] = {
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0x35, {0x00}, 1, 10},  // tearing effect line on
    {0x53, {0x20}, 1, 10},  // brightness control on
    {0x51, {0xFF}, 1, 10},  // brightness
    {0x63, {0xFF}, 1, 10},
    {0x2A, {0x00, 0x08, 0x01, 0xD9}, 4, 0},  // column 8 .. 473
    {0x2B, {0x00, 0x00, 0x01, 0xD1}, 4, 0},  // row 0 .. 465
    {0x11, {}, 0, 120},  // sleep out
    {0x36, {0xC0}, 1, 0},  // mirror X and Y (180 degrees)
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

// Panel bring-up for the 410x502 2.06" watch module, following Waveshare's
// esp32_s3_touch_amoled_2_06 BSP 2.0.0 (commit 9daffb7): the same short table as
// the 1.8" module with the watch's window (column offset 22, applied per flush
// through gap_x). Every flush rewrites the window.
constexpr InitCommand kInit410[] = {
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x11, {}, 0, 120},  // sleep out
    {0x29, {}, 0, 0},    // display on
};

// Panel bring-up for the 480x480 2.16" development board (CO5300, QSPI),
// following Waveshare's esp32_s3_touch_amoled_2_16 BSP (v2.0.1): the vendor
// page, RGB565, tearing, brightness, the full 480x480 window at no column
// offset (0x0000..0x01DF both axes) and MADCTL 0xA0. This is a square panel,
// so there are no round margins to blank.
constexpr InitCommand kInit480[] = {
    {0x11, {}, 0, 600},  // sleep out first (per the BSP table order)
    {0xFE, {0x20}, 1, 0},
    {0x19, {0x10}, 1, 0},
    {0x1C, {0xA0}, 1, 0},
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x00, 0x01, 0xDF}, 4, 0},  // full 480-wide window
    {0x2B, {0x00, 0x00, 0x01, 0xDF}, 4, 0},  // full 480-tall window
    {0x36, {0xA0}, 1, 0},  // memory access control
    {0x29, {}, 0, 600},    // display on
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
  if (cfg.panel == AmoledPanel::Co5300_143c) {
    init = kInit143c;
    init_len = sizeof(kInit143c) / sizeof(kInit143c[0]);
  } else if (cfg.panel == AmoledPanel::Co5300_132) {
    init = kInit132;
    init_len = sizeof(kInit132) / sizeof(kInit132[0]);
  } else if (cfg.panel == AmoledPanel::Co5300_368) {
    init = kInit368;
    init_len = sizeof(kInit368) / sizeof(kInit368[0]);
  } else if (cfg.panel == AmoledPanel::Co5300_410) {
    init = kInit410;
    init_len = sizeof(kInit410) / sizeof(kInit410[0]);
  } else if (cfg.panel == AmoledPanel::Co5300_480) {
    init = kInit480;
    init_len = sizeof(kInit480) / sizeof(kInit480[0]);
  }
  for (size_t i = 0; i < init_len; ++i) {
    command(init[i].cmd, init[i].data, init[i].len);
    if (init[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(init[i].delay_ms));
  }
  // A timed-out margin transfer may still own bounce_. Leave it untouched and
  // keep this display out of the HAL instead of starting normal drawing.
  if (!blank_margins()) return false;
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

// A window shorter than the glass sits gap_y rows down, centred (432 rows on the 1.8"s 448, 486 on
// the 2.06"s 502).
// Flushes never write the rows above and below it, and the panel's memory starts out as noise,
// so they are blanked once here.
bool AmoledDisplay::blank_margins() {
  if (cfg_.gap_y <= 0) return true;
  const int w = cfg_.width, rows = std::min<int>(cfg_.gap_y, kBounceRows);
  std::memset(bounce_, 0, static_cast<size_t>(w) * rows * 2);
  const int x0 = cfg_.gap_x, x1 = cfg_.gap_x + w - 1;
  const uint8_t cols[4] = {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1 >> 8),
                           static_cast<uint8_t>(x1)};
  for (int top : {0, cfg_.gap_y + cfg_.height}) {
    for (int y = top; y < top + cfg_.gap_y; y += rows) {
      const int n = std::min(rows, top + cfg_.gap_y - y), yb = y + n - 1;
      const uint8_t lines[4] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y), static_cast<uint8_t>(yb >> 8),
                                static_cast<uint8_t>(yb)};
      command(0x2A, cols, 4);
      command(0x2B, lines, 4);
      if (esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), bounce_, static_cast<size_t>(n) * w * 2) != ESP_OK ||
          xSemaphoreTake(done_, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "couldn't blank rows %d-%d outside the window", y, yb);
        return false;
      }
    }
  }
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
  di.emissive = true;
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
