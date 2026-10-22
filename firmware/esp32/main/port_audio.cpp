// I2S microphone (INMP441-style) and I2S amplifier (MAX98357A-style) on two
// separate I2S controllers, ESP-IDF 5.x standard-mode driver.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "speaker_queue.hpp"

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

uint32_t speaker_now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void log_underruns(const char* tag, const hg::SpeakerUnderruns::Totals& t) {
  ESP_LOGI(tag, "playback ended: %u underruns, %u ms in total, longest %u ms", static_cast<unsigned>(t.count),
           static_cast<unsigned>(t.total_ms), static_cast<unsigned>(t.longest_ms));
}

// --------------------------------------------------------------------------
// Microphone

bool I2sMic::begin(const I2sMicConfig& cfg) {
  // The PDM2PCM converter only exists on I2S0 (ESP32-S3); plain I2S stays on I2S1,
  // which leaves I2S0 free for the speaker on boards that have one.
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(cfg.pdm ? I2S_NUM_0 : I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan, nullptr, &rx_) != ESP_OK) return false;
  if (cfg.pdm) {
    // PDM mic (e.g. Waveshare Knob-Touch-LCD-1.8): sck = PDM clock, sd = data.
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(rate_),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = static_cast<gpio_num_t>(cfg.sck),
                     .din = static_cast<gpio_num_t>(cfg.sd),
                     .invert_flags = {.clk_inv = false}},
    };
    if (i2s_channel_init_pdm_rx_mode(rx_, &pdm_cfg) != ESP_OK) return false;
    pdm_ = true;
  } else {
    // INMP441 outputs 24-bit samples in a 32-bit left slot.
    i2s_std_config_t std_cfg = std_config(rate_, I2S_DATA_BIT_WIDTH_32BIT, cfg.sck, cfg.ws, -1, cfg.sd);
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    if (i2s_channel_init_std_mode(rx_, &std_cfg) != ESP_OK) return false;
  }
  xTaskCreate(&I2sMic::task, "hg-mic", 4096, this, 6, nullptr);
  ESP_LOGI(TAG, "microphone ready (%s)", pdm_ ? "pdm" : "i2s");
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
  int16_t pcm[kMicChunk];
  for (;;) {
    if (!self->capturing_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    size_t got = 0;
    if (self->pdm_) {
      // PDM decimation yields 16-bit samples straight out of the driver.
      // The onboard MEMS runs quiet: ~x4 digital gain puts speech in a
      // comfortable range for the server-side VAD/STT.
      i2s_channel_read(self->rx_, pcm, sizeof(pcm), &got, pdMS_TO_TICKS(100));
      if (got == 0) continue;
      size_t n = got / sizeof(int16_t);
      for (size_t i = 0; i < n; ++i) {
        pcm[i] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, pcm[i] * 4)));
      }
      if (self->capturing_) events::post(EventType::Mic, pcm, n * sizeof(int16_t));
      continue;
    }
    int32_t raw[kMicChunk];
    // A read that spans DMA buffers can return ESP_ERR_TIMEOUT with part of the data in `got`
    // (seen on ESP-IDF 6.0.1): keep what arrived instead of dropping the whole read.
    i2s_channel_read(self->rx_, raw, sizeof(raw), &got, pdMS_TO_TICKS(100));
    if (got == 0) continue;
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
  // A stream still open, draining, queued or being reset is aborted first. The speaker task
  // empties the queue for that abort() on its next pass, and audio written before then would
  // go with the old: wait for that reset, at most 6 ticks, so write() stays non-blocking. The
  // P4 speaker waits in begin() too, and as there, an idle speaker starts at once.
  if (busy() || flush_) abort();
  for (int i = 0; flush_ && i < 6; ++i) vTaskDelay(1);
  rate_ = sample_rate;  // applied by the writer task before it next enables the channel
  open_ = true;
  draining_ = false;
  return true;
}

void I2sSpeaker::write(const int16_t* samples, size_t count) {
  if (!open_) return;
  size_t bytes = count * sizeof(int16_t);
  // The app task is the only writer, so the room can only grow before the send.
  size_t fit = hg::whole_sample_bytes(bytes, xStreamBufferSpacesAvailable(buffer_));
  size_t sent = xStreamBufferSend(buffer_, samples, fit, 0);
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
  hg::SpeakerUnderruns gaps;
  for (;;) {
    if (self->flush_) {
      gaps.finish();  // an aborted stream isn't reported
      xStreamBufferReset(self->buffer_);
      if (enabled) {
        i2s_channel_disable(self->tx_);
        enabled = false;
      }
      self->flush_ = false;  // only after the reset: a write() waiting for it is not wiped
    }
    size_t got = xStreamBufferReceive(self->buffer_, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
    if (self->flush_) continue;  // received while an abort() waits for its reset: old audio
    if (got == 0) {
      if (enabled && self->open_) gaps.ran_dry(speaker_now_ms());
      if (self->draining_ && !self->open_) {
        self->draining_ = false;  // everything queued has been written out
        if (enabled) {
          log_underruns(TAG, gaps.finish());
          i2s_channel_disable(self->tx_);
          enabled = false;
        }
      }
      continue;
    }
    gaps.resumed(speaker_now_ms());
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
