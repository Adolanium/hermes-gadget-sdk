// CrowPanel Advanced P4 speaker path. This remains board-specific so the
// Gadget I2S speaker implementation and behavior on other boards stay upstream.
#include "port.hpp"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "speaker_queue.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.p4audio";
constexpr size_t kSpeakerBuffer = 48 * 1024;
constexpr size_t kSpeakerChunk = 512;

i2s_std_config_t p4_std_config(uint32_t rate, int bclk, int ws, int dout, bool stereo, bool left_align) {
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
  if (stereo) cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
  if (left_align) {
    cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;
    cfg.slot_cfg.left_align = true;
    cfg.slot_cfg.big_endian = false;
    cfg.slot_cfg.bit_order_lsb = false;
    cfg.slot_cfg.bit_shift = true;
  }
  cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(bclk);
  cfg.gpio_cfg.ws = static_cast<gpio_num_t>(ws);
  cfg.gpio_cfg.dout = dout < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(dout);
  cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  return cfg;
}

}  // namespace

bool P4I2sSpeaker::begin(const I2sSpeakerConfig& pins, const P4SpeakerConfig& cfg) {
  if (!cfg.enabled || pins.bclk < 0 || pins.ws < 0 || pins.dout < 0) return false;
  pa_ = cfg.pa;
  pa_active_low_ = cfg.pa_active_low;
  stereo_ = cfg.stereo;
  left_align_ = cfg.left_align;
  if (pa_ >= 0) {
    gpio_reset_pin(static_cast<gpio_num_t>(pa_));
    gpio_set_direction(static_cast<gpio_num_t>(pa_), GPIO_MODE_OUTPUT);
    set_pa(false);
  }

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(
      static_cast<decltype(i2s_chan_config_t{}.id)>(cfg.i2s_port), I2S_ROLE_MASTER);
  chan.auto_clear = true;
  if (i2s_new_channel(&chan, &tx_, nullptr) != ESP_OK) return false;
  const auto std_cfg = p4_std_config(rate_, pins.bclk, pins.ws, pins.dout, stereo_, left_align_);
  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK) return false;

  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(kSpeakerBuffer + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  static StaticStreamBuffer_t control;
  if (storage) buffer_ = xStreamBufferCreateStatic(kSpeakerBuffer, 1, storage, &control);
  else buffer_ = xStreamBufferCreate(16 * 1024, 1);
  if (!buffer_) return false;
  capacity_ = storage ? kSpeakerBuffer : 16 * 1024;
  if (xTaskCreate(&P4I2sSpeaker::task, "hg-p4spk", 4096, this, 7, nullptr) != pdPASS) return false;
  ESP_LOGI(TAG, "P4 speaker ready on I2S%d", cfg.i2s_port);
  return true;
}

void P4I2sSpeaker::set_pa(bool on) {
  if (pa_ < 0) return;
  gpio_set_level(static_cast<gpio_num_t>(pa_), (on != pa_active_low_) ? 1 : 0);
}

bool P4I2sSpeaker::begin(uint32_t sample_rate) {
  if (!tx_) return false;
  const bool pending = open_.load() || draining_.load() || !flush_done_.load() ||
                       (buffer_ && xStreamBufferBytesAvailable(buffer_) > 0);
  if (pending) {
    abort();
    for (int i = 0; i < 5 && !flush_done_.load(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (!flush_done_.load()) {
      ESP_LOGW(TAG, "speaker flush did not finish before new stream");
      return false;
    }
  }
  rate_ = sample_rate;
  open_ = true;
  draining_ = false;
  return true;
}

void P4I2sSpeaker::write(const int16_t* samples, size_t count) {
  if (!open_ || !samples || !buffer_) return;
  const size_t bytes = count * sizeof(int16_t);
  const size_t fit = hg::whole_sample_bytes(bytes, xStreamBufferSpacesAvailable(buffer_));
  const size_t sent = xStreamBufferSend(buffer_, samples, fit, 0);
  if (sent < bytes) ESP_LOGW(TAG, "playback buffer full, dropped %u bytes", static_cast<unsigned>(bytes - sent));
}

void P4I2sSpeaker::end() {
  open_ = false;
  draining_ = true;
}

void P4I2sSpeaker::abort() {
  open_ = false;
  draining_ = false;
  flush_done_ = false;
  flush_ = true;
}

bool P4I2sSpeaker::busy() const {
  return open_ || draining_ || !flush_done_.load() || (buffer_ && xStreamBufferBytesAvailable(buffer_) > 0);
}

void P4I2sSpeaker::task(void* arg) {
  auto* self = static_cast<P4I2sSpeaker*>(arg);
  int16_t chunk[kSpeakerChunk];
  int16_t stereo[kSpeakerChunk];
  bool enabled = false;
  uint32_t applied_rate = self->rate_.load();
  hg::SpeakerUnderruns gaps;
  for (;;) {
    if (self->flush_.exchange(false)) {
      gaps.finish();
      xStreamBufferReset(self->buffer_);
      if (enabled) {
        i2s_channel_disable(self->tx_);
        self->set_pa(false);
        enabled = false;
      }
      self->flush_done_ = true;
    }

    const size_t got = xStreamBufferReceive(self->buffer_, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
    if (got == 0) {
      if (enabled && self->open_) gaps.ran_dry(speaker_now_ms());
      if (self->draining_ && !self->open_) {
        self->draining_ = false;
        if (enabled) {
          log_underruns(TAG, gaps.finish());
          i2s_channel_disable(self->tx_);
          self->set_pa(false);
          enabled = false;
        }
      }
      continue;
    }

    gaps.resumed(speaker_now_ms());
    if (!enabled) {
      const uint32_t want = self->rate_.load();
      if (want != applied_rate) {
        i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(want);
        if (i2s_channel_reconfig_std_clock(self->tx_, &clk) == ESP_OK) applied_rate = want;
      }
      self->set_pa(true);
      const esp_err_t err = i2s_channel_enable(self->tx_);
      if (err != ESP_OK) {
        self->set_pa(false);
        ESP_LOGE(TAG, "I2S channel enable failed: %s", esp_err_to_name(err));
        continue;
      }
      enabled = true;
    }

    const size_t n = got / sizeof(int16_t);
    const int vol = self->volume_.load();
    if (self->stereo_) {
      size_t offset = 0;
      while (offset < n) {
        const size_t consumed = hg::mono_to_stereo(chunk + offset, n - offset, stereo, kSpeakerChunk);
        if (!consumed) break;
        const size_t output_samples = consumed * 2;
        for (size_t i = 0; i < output_samples; ++i)
          stereo[i] = static_cast<int16_t>(static_cast<int32_t>(stereo[i]) * vol / 100);
        size_t written = 0;
        const size_t requested = output_samples * sizeof(int16_t);
        const esp_err_t err = i2s_channel_write(self->tx_, stereo, requested, &written, 200);
        if (err != ESP_OK || written != requested)
          ESP_LOGW(TAG, "I2S short stereo write: requested=%u written=%u err=%s",
                   static_cast<unsigned>(requested), static_cast<unsigned>(written), esp_err_to_name(err));
        offset += consumed;
      }
    } else {
      for (size_t i = 0; i < n; ++i) chunk[i] = static_cast<int16_t>(static_cast<int32_t>(chunk[i]) * vol / 100);
      size_t written = 0;
      const esp_err_t err = i2s_channel_write(self->tx_, chunk, got, &written, 200);
      if (err != ESP_OK || written != got)
        ESP_LOGW(TAG, "I2S short write: requested=%u written=%u err=%s",
                 static_cast<unsigned>(got), static_cast<unsigned>(written), esp_err_to_name(err));
    }
  }
}

}  // namespace hgp
