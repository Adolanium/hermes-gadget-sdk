// I2S microphone (INMP441-style) and I2S amplifier (MAX98357A-style) on two
// separate I2S controllers, ESP-IDF 5.x standard-mode driver.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace hgp {
namespace {

const char* TAG = "hg.audio";
constexpr size_t kMicChunk = 320;           // 20 ms at 16 kHz
constexpr size_t kSpeakerBuffer = 48 * 1024;  // ~1.5 s at 16 kHz; the server paces 0.5 s ahead
constexpr size_t kSpeakerChunk = 512;       // samples per I2S write

i2s_std_config_t std_config(uint32_t rate, i2s_data_bit_width_t bits, int bclk, int ws, int dout, int din) {
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, I2S_SLOT_MODE_MONO);
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(bclk);
  cfg.gpio_cfg.ws = static_cast<gpio_num_t>(ws);
  cfg.gpio_cfg.dout = dout < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(dout);
  cfg.gpio_cfg.din = din < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(din);
  return cfg;
}

}  // namespace

// --------------------------------------------------------------------------
// Microphone

bool I2sMic::begin(const I2sMicConfig& cfg) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan, nullptr, &rx_) != ESP_OK) return false;
  // INMP441 outputs 24-bit samples in a 32-bit left slot.
  i2s_std_config_t std_cfg = std_config(rate_, I2S_DATA_BIT_WIDTH_32BIT, cfg.sck, cfg.ws, -1, cfg.sd);
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  if (i2s_channel_init_std_mode(rx_, &std_cfg) != ESP_OK) return false;
  xTaskCreate(&I2sMic::task, "hg-mic", 4096, this, 6, nullptr);
  ESP_LOGI(TAG, "microphone ready");
  return true;
}

bool I2sMic::start(uint32_t sample_rate) {
  if (!rx_) return false;
  if (sample_rate != rate_) {
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    if (i2s_channel_reconfig_std_clock(rx_, &clk) != ESP_OK) return false;
    rate_ = sample_rate;
  }
  if (i2s_channel_enable(rx_) != ESP_OK) return false;
  capturing_ = true;
  return true;
}

void I2sMic::stop() {
  if (!capturing_) return;
  capturing_ = false;
  i2s_channel_disable(rx_);
}

void I2sMic::task(void* arg) {
  auto* self = static_cast<I2sMic*>(arg);
  int32_t raw[kMicChunk];
  int16_t pcm[kMicChunk];
  for (;;) {
    if (!self->capturing_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    size_t got = 0;
    if (i2s_channel_read(self->rx_, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) continue;
    size_t n = got / sizeof(int32_t);
    for (size_t i = 0; i < n; ++i) {
      // 24-bit sample left-aligned in 32 bits; keep the top 16 with a little gain.
      int32_t s = raw[i] >> 13;
      pcm[i] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, s)));
    }
    if (self->capturing_) events::post(EventType::Mic, pcm, n * sizeof(int16_t));
  }
}

// --------------------------------------------------------------------------
// Speaker

bool I2sSpeaker::begin(const I2sSpeakerConfig& cfg) {
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // silence on underrun instead of repeating the last buffer
  if (i2s_new_channel(&chan, &tx_, nullptr) != ESP_OK) return false;
  i2s_std_config_t std_cfg = std_config(rate_, I2S_DATA_BIT_WIDTH_16BIT, cfg.bclk, cfg.ws, cfg.dout, -1);
  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK) return false;
  uint8_t* storage = static_cast<uint8_t*>(heap_caps_malloc(kSpeakerBuffer + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  static StaticStreamBuffer_t control;
  if (storage) buffer_ = xStreamBufferCreateStatic(kSpeakerBuffer, 1, storage, &control);
  else buffer_ = xStreamBufferCreate(16 * 1024, 1);
  if (!buffer_) return false;
  xTaskCreate(&I2sSpeaker::task, "hg-spk", 4096, this, 7, nullptr);
  ESP_LOGI(TAG, "speaker ready");
  return true;
}

bool I2sSpeaker::begin(uint32_t sample_rate) {
  if (!tx_) return false;
  abort();
  rate_ = sample_rate;  // applied by the writer task before it next enables the channel
  open_ = true;
  draining_ = false;
  return true;
}

void I2sSpeaker::write(const int16_t* samples, size_t count) {
  if (!open_) return;
  size_t bytes = count * sizeof(int16_t);
  size_t sent = xStreamBufferSend(buffer_, samples, bytes, 0);
  if (sent < bytes) ESP_LOGW(TAG, "playback buffer full, dropped %u bytes", static_cast<unsigned>(bytes - sent));
}

void I2sSpeaker::end() {
  open_ = false;
  draining_ = true;
}

void I2sSpeaker::abort() {
  open_ = false;
  draining_ = false;
  flush_ = true;
}

bool I2sSpeaker::busy() const {
  return open_ || draining_ || (buffer_ && xStreamBufferBytesAvailable(buffer_) > 0);
}

void I2sSpeaker::task(void* arg) {
  // Owns every channel state change (enable, disable, clock) so the app task never races it.
  auto* self = static_cast<I2sSpeaker*>(arg);
  int16_t chunk[kSpeakerChunk];
  bool enabled = false;
  uint32_t applied_rate = self->rate_.load();
  for (;;) {
    if (self->flush_.exchange(false)) {
      xStreamBufferReset(self->buffer_);
      if (enabled) {
        i2s_channel_disable(self->tx_);
        enabled = false;
      }
    }
    size_t got = xStreamBufferReceive(self->buffer_, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
    if (got == 0) {
      if (self->draining_ && !self->open_) {
        self->draining_ = false;  // everything queued has been written out
        if (enabled) {
          i2s_channel_disable(self->tx_);
          enabled = false;
        }
      }
      continue;
    }
    if (!enabled) {
      uint32_t want = self->rate_.load();
      if (want != applied_rate) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(want);
        if (i2s_channel_reconfig_std_clock(self->tx_, &clk) == ESP_OK) applied_rate = want;
      }
      i2s_channel_enable(self->tx_);
      enabled = true;
    }
    size_t n = got / sizeof(int16_t);
    int vol = self->volume_.load();
    for (size_t i = 0; i < n; ++i) chunk[i] = static_cast<int16_t>(chunk[i] * vol / 100);
    size_t written = 0;
    i2s_channel_write(self->tx_, chunk, n * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
  }
}

}  // namespace hgp
