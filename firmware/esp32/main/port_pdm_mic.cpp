// PDM microphone (ESP32-P4 CrowPanel Advanced 10.1"): a reader task posts 20 ms
// PCM16 chunks while capturing, mirroring I2sMic. The board's mic is a PDM
// device on I2S0 (clk GPIO24, data GPIO26); the speaker uses standard I2S on
// I2S1. PDM RX decimates the 1-bit stream to 16-bit PCM in hardware.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cmath>
#include <cstring>

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.pdmmic";
constexpr size_t kMicChunk = 320;  // 20 ms at 16 kHz

}  // namespace

bool PdmMic::begin(const I2sMicConfig& cfg) {
  // PDM mic lives on I2S0; the speaker takes I2S1 (see board config).
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 6;
  chan.dma_frame_num = 256;
  chan.auto_clear_after_cb = true;
  chan.auto_clear_before_cb = true;
  if (i2s_new_channel(&chan, nullptr, &rx_) != ESP_OK) return false;

  i2s_pdm_rx_config_t pdm_cfg = {};
  pdm_cfg.clk_cfg.sample_rate_hz = rate_;
  pdm_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
  pdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  pdm_cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_8S;
  pdm_cfg.clk_cfg.bclk_div = 8;
  pdm_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  pdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  pdm_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
  pdm_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
  pdm_cfg.slot_cfg.hp_en = true;              // high-pass removes DC offset
  pdm_cfg.slot_cfg.hp_cut_off_freq_hz = 35.5;
  pdm_cfg.slot_cfg.amplify_num = 1;
  pdm_cfg.gpio_cfg.clk = static_cast<gpio_num_t>(cfg.sck);
  pdm_cfg.gpio_cfg.din = static_cast<gpio_num_t>(cfg.sd);
  pdm_cfg.gpio_cfg.invert_flags.clk_inv = false;

  if (i2s_channel_init_pdm_rx_mode(rx_, &pdm_cfg) != ESP_OK) return false;
  const BaseType_t task_result = xTaskCreate(&PdmMic::task, "hg-pdmmic", 4096, this, 6, nullptr);
  ESP_LOGI(TAG, "[MICDBG-audio-01] reader task create=%s", task_result == pdPASS ? "ok" : "failed");
  if (task_result != pdPASS) return false;
  ESP_LOGI(TAG, "pdm microphone ready (clk %d, data %d)", cfg.sck, cfg.sd);
  return true;
}

bool PdmMic::start(uint32_t sample_rate) {
  if (!rx_) return false;
  if (sample_rate != rate_) {
    i2s_pdm_rx_clk_config_t clk = {};
    clk.sample_rate_hz = sample_rate;
    clk.clk_src = I2S_CLK_SRC_DEFAULT;
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    clk.dn_sample_mode = I2S_PDM_DSR_8S;
    clk.bclk_div = 8;
    if (i2s_channel_reconfig_pdm_rx_clock(rx_, &clk) != ESP_OK) return false;
    rate_ = sample_rate;
  }
  const esp_err_t enable_err = i2s_channel_enable(rx_);
  ESP_LOGI(TAG, "[MICDBG-audio-01] channel enable=%s rate=%u", esp_err_to_name(enable_err), static_cast<unsigned>(sample_rate));
  if (enable_err != ESP_OK) return false;
  capturing_ = true;
  return true;
}

void PdmMic::stop() {
  if (!capturing_) return;
  capturing_ = false;
  i2s_channel_disable(rx_);
}

void PdmMic::task(void* arg) {
  auto* self = static_cast<PdmMic*>(arg);
  int16_t pcm[kMicChunk];
  TickType_t report_at = xTaskGetTickCount();
  uint32_t blocks = 0, read_errors = 0, empty_reads = 0, posted = 0, post_failures = 0;
  uint32_t interval_peak = 0, interval_last_peak = 0;
  uint64_t interval_squares = 0;
  size_t interval_samples = 0;
  int last_error = 0;
  bool capture_reported = false;
  for (;;) {
    if (!self->capturing_.load()) {
      capture_reported = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (!capture_reported) {
      ESP_LOGI(TAG, "[MICDBG-audio-01] capture loop entered");
      capture_reported = true;
      report_at = xTaskGetTickCount();
    }
    size_t got = 0;
    const esp_err_t read_err = i2s_channel_read(self->rx_, pcm, sizeof(pcm), &got, 100);  // timeout_ms, not FreeRTOS ticks
    if (read_err != ESP_OK) {
      ++read_errors;
      last_error = static_cast<int>(read_err);
    } else if (got == 0) {
      ++empty_reads;
    } else {
      const size_t n = got / sizeof(int16_t);
      uint64_t block_squares = 0;
      uint32_t block_peak = 0;
      for (size_t i = 0; i < n; ++i) {
        const int32_t raw = pcm[i];
        const uint32_t magnitude = static_cast<uint32_t>(raw < 0 ? -raw : raw);
        block_peak = std::max(block_peak, magnitude);
        block_squares += static_cast<uint64_t>(raw * raw);

      }
      ++blocks;
      interval_last_peak = block_peak;
      interval_peak = std::max(interval_peak, block_peak);
      interval_squares += block_squares;
      interval_samples += n;
      if (self->capturing_.load()) {
        if (events::post(EventType::Mic, pcm, n * sizeof(int16_t))) ++posted;
        else ++post_failures;
      }
    }
    if (xTaskGetTickCount() - report_at >= pdMS_TO_TICKS(1000)) {
      const uint32_t rms_value = interval_samples
          ? static_cast<uint32_t>(std::sqrt(static_cast<double>(interval_squares) / interval_samples)) : 0;
      ESP_LOGI(TAG, "[MICDBG-audio-01] blocks=%u read_err=%u empty=%u posted=%u post_fail=%u peak=%u last_peak=%u rms=%u err=%d",
               static_cast<unsigned>(blocks), static_cast<unsigned>(read_errors), static_cast<unsigned>(empty_reads),
               static_cast<unsigned>(posted), static_cast<unsigned>(post_failures), static_cast<unsigned>(interval_peak),
               static_cast<unsigned>(interval_last_peak), static_cast<unsigned>(rms_value), last_error);
      blocks = read_errors = empty_reads = posted = post_failures = interval_peak = interval_last_peak = 0;
      interval_squares = 0;
      interval_samples = 0;
      last_error = 0;
      report_at = xTaskGetTickCount();
    }
  }
}

}  // namespace hgp
