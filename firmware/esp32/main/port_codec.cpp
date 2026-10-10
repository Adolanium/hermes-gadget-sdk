// The shared I2C bus, and audio through codec chips: an ES8311 DAC or AW88298
// speaker amplifier and an ES7210 ADC for the microphones, on one duplex I2S
// bus (esp_codec_dev does the codec register work).
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "esp_codec_dev_defaults.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "speaker_queue.hpp"
#include "ws185.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.codec";
constexpr size_t kMicChunk = 320;             // about 13 ms at 24 kHz
constexpr size_t kSpeakerBuffer = 192 * 1024;  // absorbs WiFi-burst backlog without dropping; PSRAM, 8 MB free
constexpr size_t kSpeakerChunk = 512;         // samples per codec write

}  // namespace

// --------------------------------------------------------------------------
// I2C

namespace i2c {
i2c_master_bus_handle_t bus(const I2cBusConfig& cfg) {
  static i2c_master_bus_handle_t handle = nullptr;
  if (handle || cfg.sda < 0 || cfg.scl < 0) return handle;
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = static_cast<gpio_num_t>(cfg.sda);
  bus_cfg.scl_io_num = static_cast<gpio_num_t>(cfg.scl);
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus on SDA %d / SCL %d failed", cfg.sda, cfg.scl);
    handle = nullptr;
  }
  return handle;
}
}  // namespace i2c

// --------------------------------------------------------------------------
// Codecs

bool CodecAudio::begin(const CodecAudioConfig& cfg, i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  if (cfg.speaker_pa && cfg.pa >= 0) {
    gpio_reset_pin(static_cast<gpio_num_t>(cfg.pa));
    gpio_set_direction(static_cast<gpio_num_t>(cfg.pa), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(cfg.pa), 0);
  }
  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  chan.auto_clear = true;  // silence on underrun instead of repeating the last buffer
  if (i2s_new_channel(&chan, &tx_, &rx_) != ESP_OK) return false;
  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kRate);  // MCLK = 256 x fs
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      cfg.stereo32 ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT,
      cfg.stereo32 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
  std_cfg.gpio_cfg.mclk = static_cast<gpio_num_t>(cfg.mclk);
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(cfg.bclk);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(cfg.ws);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(cfg.dout);
  std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(cfg.din);
  if (i2s_channel_init_std_mode(tx_, &std_cfg) != ESP_OK || i2s_channel_init_std_mode(rx_, &std_cfg) != ESP_OK) {
    ESP_LOGE(TAG, "I2S setup failed");
    return false;
  }
  i2s_channel_enable(tx_);
  i2s_channel_enable(rx_);

  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port = I2S_NUM_1;
  i2s_cfg.rx_handle = rx_;
  i2s_cfg.tx_handle = tx_;
  const audio_codec_data_if_t* data_if = audio_codec_new_i2s_data(&i2s_cfg);
  const audio_codec_gpio_if_t* gpio_if = audio_codec_new_gpio();

  audio_codec_i2c_cfg_t dac_i2c = {};
  dac_i2c.port = I2C_NUM_0;
  dac_i2c.addr = cfg.speaker == SpeakerCodec::Aw88298 ? AW88298_CODEC_DEFAULT_ADDR : ES8311_CODEC_DEFAULT_ADDR;
  dac_i2c.bus_handle = bus;
  es8311_codec_cfg_t dac = {};
  dac.ctrl_if = audio_codec_new_i2c_ctrl(&dac_i2c);
  dac.gpio_if = gpio_if;
  // The ES8311 has an ADC too. A board that wires an analog microphone to it
  // uses one codec handle for both directions instead of an ES7210.
  const bool shared = cfg.mic == MicCodec::Es8311 && cfg.speaker == SpeakerCodec::Es8311;
  dac.codec_mode = shared ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC;
  // With pa_pin, esp_codec_dev turns the amplifier on in esp_codec_dev_open()
  // and leaves it on; muting only writes a DAC register. speaker_pa boards
  // keep it off while idle and drop it as soon as playback is aborted.
  dac.pa_pin = cfg.speaker_pa ? -1 : static_cast<int16_t>(cfg.pa);
  dac.pa_reverted = false;
  dac.master_mode = false;
  dac.use_mclk = cfg.dac_mclk;
  dac.hw_gain.pa_voltage = cfg.amp_supply_v;
  dac.hw_gain.codec_dac_voltage = 3.3;
  esp_codec_dev_cfg_t out_cfg = {};
  out_cfg.dev_type = shared ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT;
  if (cfg.speaker == SpeakerCodec::Aw88298) {
    aw88298_codec_cfg_t amp = {};
    amp.ctrl_if = dac.ctrl_if;
    amp.gpio_if = gpio_if;
    amp.hw_gain.pa_gain = 15;
    out_cfg.codec_if = aw88298_codec_new(&amp);
  } else {
    out_cfg.codec_if = es8311_codec_new(&dac);
  }
  out_cfg.data_if = data_if;
  out_ = out_cfg.codec_if ? esp_codec_dev_new(&out_cfg) : nullptr;

  if (!shared) {
    audio_codec_i2c_cfg_t adc_i2c = {};
    adc_i2c.port = I2C_NUM_0;
    adc_i2c.addr = ES7210_CODEC_DEFAULT_ADDR;
    adc_i2c.bus_handle = bus;
    es7210_codec_cfg_t adc = {};
    adc.ctrl_if = audio_codec_new_i2c_ctrl(&adc_i2c);
    adc.mic_selected = cfg.es7210_mics;
    esp_codec_dev_cfg_t in_cfg = {};
    in_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    in_cfg.codec_if = es7210_codec_new(&adc);
    in_cfg.data_if = data_if;
    in_ = in_cfg.codec_if ? esp_codec_dev_new(&in_cfg) : nullptr;
  }

  // Both stay open at one rate: they share the I2S clocks.
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = kRate;
  fs.channel = cfg.stereo32 ? 2 : 1;
  fs.bits_per_sample = cfg.stereo32 ? 32 : 16;
  if (out_ && esp_codec_dev_open(out_, &fs) != ESP_CODEC_DEV_OK) out_ = nullptr;
  if (shared) in_ = out_;  // one handle, opened once
  else if (in_ && esp_codec_dev_open(in_, &fs) != ESP_CODEC_DEV_OK) in_ = nullptr;
  if (out_) {
    esp_codec_dev_set_out_vol(out_, 70);
    esp_codec_dev_set_out_mute(out_, true);  // unmuted while something plays
  }
  if (in_) esp_codec_dev_set_in_gain(in_, cfg.mic_gain_db);
  ESP_LOGI(TAG, "codecs: speaker %s, microphones %s (%s)", out_ ? "ready" : "missing", in_ ? "ready" : "missing",
           shared ? "es8311 analog" : "es7210");
  return out_ || in_;
}

// --------------------------------------------------------------------------
// Microphone

bool CodecMic::begin(esp_codec_dev_handle_t dev, bool rmnm) {
  if (!dev) return false;
  dev_ = dev;
  rmnm_ = rmnm;
  if (rmnm) {
    raw_ = static_cast<int16_t*>(heap_caps_malloc(kMicChunk * 4 * sizeof(int16_t), MALLOC_CAP_8BIT));
    if (!raw_) return false;
  }
  xTaskCreate(&CodecMic::task, "hg-mic", 4096, this, 6, nullptr);
  return true;
}

bool CodecMic::start(uint32_t sample_rate) {
  if (!dev_) return false;
  if (sample_rate != CodecAudio::kRate) {
    ESP_LOGW(TAG, "microphone runs at %u Hz only", static_cast<unsigned>(CodecAudio::kRate));
    return false;
  }
  capturing_ = true;
  return true;
}

void CodecMic::task(void* arg) {
  auto* self = static_cast<CodecMic*>(arg);
  int16_t pcm[kMicChunk];
  int16_t* raw = self->raw_;
  for (;;) {
    // Read continuously so a capture starts with fresh samples, not a stale DMA backlog.
    void* data = self->rmnm_ ? static_cast<void*>(raw) : static_cast<void*>(pcm);
    const size_t bytes = self->rmnm_ ? kMicChunk * 4 * sizeof(int16_t) : sizeof(pcm);
    if (esp_codec_dev_read(self->dev_, data, bytes) != ESP_CODEC_DEV_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (self->rmnm_) hg::ws185_mono(raw, pcm, kMicChunk);
    if (self->capturing_) events::post(EventType::Mic, pcm, sizeof(pcm));
  }
}

// --------------------------------------------------------------------------
// Speaker

bool CodecSpeaker::begin(esp_codec_dev_handle_t dev, bool stereo32, int pa) {
  if (!dev) return false;
  dev_ = dev;
  uint8_t* storage = static_cast<uint8_t*>(heap_caps_malloc(kSpeakerBuffer + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  static StaticStreamBuffer_t control;
  if (storage) buffer_ = xStreamBufferCreateStatic(kSpeakerBuffer, 1, storage, &control);
  else buffer_ = xStreamBufferCreate(16 * 1024, 1);
  if (!buffer_) return false;
  stereo32_ = stereo32;
  if (pa >= 0) {
    const auto pin = static_cast<gpio_num_t>(pa);
    pa_.emplace([this](void* chunk, size_t bytes) { return xStreamBufferReceive(buffer_, chunk, bytes, 0); },
                [this] { xStreamBufferReset(buffer_); },
                [pin](bool on) { gpio_set_level(pin, on); });
  }
  if (stereo32) {
    stereo_ = static_cast<int32_t*>(heap_caps_malloc(kSpeakerChunk * 2 * sizeof(int32_t), MALLOC_CAP_8BIT));
    if (!stereo_) return false;
  }
  xTaskCreate(&CodecSpeaker::task, "hg-spk", 4096, this, 7, nullptr);
  return true;
}

bool CodecSpeaker::begin(uint32_t sample_rate) {
  if (!dev_) return false;
  if (sample_rate != CodecAudio::kRate) {
    // The host resamples to the rate the device declares, so this is not expected.
    ESP_LOGW(TAG, "speaker runs at %u Hz only", static_cast<unsigned>(CodecAudio::kRate));
    return false;
  }
  abort();
  open_ = true;
  draining_ = false;
  return true;
}

void CodecSpeaker::write(const int16_t* samples, size_t count) {
  if (!open_) return;
  size_t bytes = count * sizeof(int16_t);
  // Whole-frame drop: a nonblocking xStreamBufferSend can partial-write when
  // the ring is nearly full, and a partial frame shifts every later sample pair
  // by one byte => full-scale static. Drop the whole frame instead; never
  // misaligns. Cumulative drop accounting for stream-end logging.
  if (xStreamBufferSpacesAvailable(buffer_) < bytes) {
    drop_bytes_ += bytes;
    static int64_t last_drop_log = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_drop_log > 2000) {
      last_drop_log = now;
      ESP_LOGW(TAG, "playback buffer full, dropped %u B cumulative (ring avail %u)",
               static_cast<unsigned>(drop_bytes_.load()),
               static_cast<unsigned>(xStreamBufferSpacesAvailable(buffer_)));
    }
    return;
  }
  xStreamBufferSend(buffer_, samples, bytes, 0);
}

void CodecSpeaker::end() {
  // Publication order matters: mark draining first. A consumer that observes
  // open_ == false must already see draining_ == true, or it could mute the amp
  // with queued audio still unplayed.
  draining_ = true;
  open_ = false;
}

void CodecSpeaker::abort() {
  open_ = false;
  draining_ = false;
  if (pa_) pa_->abort();
  flush_ = true;
}

bool CodecSpeaker::busy() const {
  return open_ || draining_ || (buffer_ && xStreamBufferBytesAvailable(buffer_) > 0);
}

void CodecSpeaker::set_volume(uint8_t percent) {
  if (dev_) esp_codec_dev_set_out_vol(dev_, percent > 100 ? 100 : percent);
}

void CodecSpeaker::task(void* arg) {
  auto* self = static_cast<CodecSpeaker*>(arg);
  int16_t chunk[kSpeakerChunk];
  int32_t* stereo = self->stereo_;
  auto& pa = self->pa_;
  bool playing = false;
  int64_t stream_t0 = 0;
  uint64_t stream_bytes = 0;
  for (;;) {
    if (self->flush_.exchange(false)) {
      stream_t0 = 0;
      stream_bytes = 0;
      self->drop_bytes_ = 0;
      if (pa) {
        // abort() already emptied the queue and turned the amplifier off.
        esp_codec_dev_set_out_mute(self->dev_, true);
        playing = false;
      } else {
        xStreamBufferReset(self->buffer_);
      }
    }
    uint32_t generation = 0;
    size_t got = pa ? pa->receive(chunk, sizeof(chunk), generation)
                    : xStreamBufferReceive(self->buffer_, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
    if (got == 0) {
      if (playing && !self->open_) {
        // The ring is empty but the last real audio may still sit in the I2S DMA
        // ring (esp_codec_dev_write/i2s_channel_write return once queued, not once
        // played). Push silence exceeding DMA ring depth before muting, else the
        // final phoneme clips. I2S_CHANNEL_DEFAULT_CONFIG gives 6 DMA descriptors
        // x 240 frames, in the codec's actual frame width: mono 16-bit = 2880 B
        // (60 ms at 24 kHz); stereo 32-bit = 11520 B. Silence is written in that
        // same width, sized to exceed the ring with margin (3 x 1 KB for mono16,
        // 3 x 4 KB through the stereo conversion for stereo32).
        int errs = 0, first_rc = 0;
        if (self->stereo32_) {
          // Convert silence through the same width path as real audio: 1KB of
          // int16 mono becomes 4KB of int32 stereo; 3 writes = 12288 B > 11520 B.
          for (int i = 0; i < 3; i++) {
            memset(self->stereo_, 0, kSpeakerChunk * 2 * sizeof(int32_t));
            int rc = esp_codec_dev_write(self->dev_, self->stereo_, kSpeakerChunk * 2 * sizeof(int32_t));
            if (rc != ESP_CODEC_DEV_OK) { errs++; if (!first_rc) first_rc = rc; }
          }
        } else {
          memset(chunk, 0, sizeof(chunk));  // got == 0, so nothing in it is unplayed
          for (int i = 0; i < 3; i++) {
            int rc = esp_codec_dev_write(self->dev_, chunk, sizeof(chunk));
            if (rc != ESP_CODEC_DEV_OK) { errs++; if (!first_rc) first_rc = rc; }
          }
        }
        esp_codec_dev_set_out_mute(self->dev_, true);
        self->draining_ = false;  // busy() stays true until the tail is played and muted
        if (errs) ESP_LOGW(TAG, "spk tail drain: %d/3 writes failed (rc of first=%d), tail may clip", errs, first_rc);
        if (pa) pa->disable();
        int64_t elapsed = stream_t0 ? esp_timer_get_time() / 1000 - stream_t0 : 0;
        ESP_LOGW(TAG, "spk stream end: %lld B played, %u B dropped, in %lld ms (%.2f B/ms, kRate=%u => %.2f)",
                 static_cast<long long>(stream_bytes),
                 static_cast<unsigned>(self->drop_bytes_.exchange(0)),
                 static_cast<long long>(elapsed),
                 elapsed > 0 ? static_cast<double>(stream_bytes) / elapsed : 0.0,
                 static_cast<unsigned>(CodecAudio::kRate),
                 elapsed > 0 ? static_cast<double>(stream_bytes) / elapsed /
                     (CodecAudio::kRate * sizeof(int16_t) / 1000.0) : 0.0);
        stream_t0 = 0;
        stream_bytes = 0;
        playing = false;
      }
      if (pa) vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!playing) {
      esp_codec_dev_set_out_mute(self->dev_, false);
      playing = true;
    }
    // Drop a chunk taken before an abort() rather than replay it.
    if (pa && !pa->enable(generation)) continue;
    if (stream_t0 == 0) stream_t0 = esp_timer_get_time() / 1000;  // the stream-end log's clock starts at the first frame played
    if (self->stereo32_) {
      const size_t frames = got / sizeof(int16_t);
      hg::ws185_stereo32(chunk, stereo, frames);
      esp_codec_dev_write(self->dev_, stereo, static_cast<int>(frames * 2 * sizeof(int32_t)));
      stream_bytes += frames * sizeof(int16_t);  // the stream's own bytes, not the 4x wider I2S frames
    } else {
      size_t wr = got & ~static_cast<size_t>(1);
      esp_codec_dev_write(self->dev_, chunk, static_cast<int>(wr));
      stream_bytes += wr;
    }
  }
}

}  // namespace hgp
