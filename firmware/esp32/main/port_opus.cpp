// Opus speaker audio: libopus (78/esp-opus, Xiph's BSD-licensed codec built
// fixed-point for ESP-IDF) decoding one packet per binary audio frame.
#include "port.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "opus.h"

namespace hgp {

namespace {
constexpr const char* TAG = "hg.opus";
}

OpusSpeakerDecoder::~OpusSpeakerDecoder() {
  if (decoder_) heap_caps_free(decoder_);
}

bool OpusSpeakerDecoder::begin(uint32_t sample_rate) {
  // Opus decodes at 8, 12, 16, 24 or 48 kHz; the gadget asks for its speaker rate.
  if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 && sample_rate != 24000 &&
      sample_rate != 48000)
    return false;
  if (decoder_ && sample_rate == rate_) return opus_decoder_ctl(decoder_, OPUS_RESET_STATE) == OPUS_OK;
  if (decoder_) heap_caps_free(decoder_);
  // The state (about 18 KB) lives in PSRAM where a board has it; internal RAM otherwise.
  const int size = opus_decoder_get_size(1);
  decoder_ = static_cast<OpusDecoder*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!decoder_) decoder_ = static_cast<OpusDecoder*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
  if (!decoder_ || opus_decoder_init(decoder_, static_cast<opus_int32>(sample_rate), 1) != OPUS_OK) {
    ESP_LOGE(TAG, "decoder for %u Hz unavailable", static_cast<unsigned>(sample_rate));
    if (decoder_) heap_caps_free(decoder_);
    decoder_ = nullptr;
    return false;
  }
  rate_ = sample_rate;
  return true;
}

int OpusSpeakerDecoder::decode(const uint8_t* packet, size_t len, int16_t* pcm, size_t max_samples) {
  if (!decoder_ || len == 0) return -1;
  const int n = opus_decode(decoder_, packet, static_cast<opus_int32>(len), pcm, static_cast<int>(max_samples), 0);
  if (n < 0) {
    ESP_LOGW(TAG, "packet of %u bytes: %s", static_cast<unsigned>(len), opus_strerror(n));
    return -1;
  }
  return n;
}

}  // namespace hgp
