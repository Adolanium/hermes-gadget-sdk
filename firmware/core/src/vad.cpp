#include "hg/vad.hpp"

#include <cmath>

namespace hg {

float rms(const int16_t* samples, size_t count) {
  if (count == 0) return 0.0f;
  double acc = 0;
  for (size_t i = 0; i < count; ++i) acc += double(samples[i]) * samples[i];
  return static_cast<float>(std::sqrt(acc / double(count)));
}

uint8_t level_percent(float r) {
  // ~-60 dBFS -> 0, 0 dBFS -> 100.
  if (r < 1.0f) return 0;
  float db = 20.0f * std::log10(r / 32768.0f);
  float pct = (db + 60.0f) * (100.0f / 60.0f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return static_cast<uint8_t>(pct);
}

void Vad::reset(const Config& cfg) {
  cfg_ = cfg;
  noise_ = 0;
  elapsed_ms_ = speech_ms_ = silence_ms_ = 0;
  carry_ = 0;
  z1_ = z2_ = 0;
  b0_ = 1;
  b1_ = b2_ = a1_ = a2_ = 0;
  if (cfg.lowpass_hz && cfg.sample_rate > 2 * cfg.lowpass_hz) {
    // Butterworth, by the bilinear transform.
    const float k = std::tan(3.14159265f * static_cast<float>(cfg.lowpass_hz) / static_cast<float>(cfg.sample_rate));
    const float norm = 1.0f / (1.0f + 1.41421356f * k + k * k);
    b0_ = k * k * norm;
    b1_ = 2 * b0_;
    b2_ = b0_;
    a1_ = 2 * (k * k - 1) * norm;
    a2_ = (1 - 1.41421356f * k + k * k) * norm;
  }
}

float Vad::voice_rms(const int16_t* samples, size_t count) {
  double acc = 0;
  for (size_t i = 0; i < count; ++i) {
    // Transposed direct form II.
    const float x = samples[i];
    const float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    acc += double(y) * y;
  }
  return static_cast<float>(std::sqrt(acc / double(count)));
}

Vad::Result Vad::feed(const int16_t* samples, size_t count) {
  if (count == 0 || cfg_.sample_rate == 0) return Result::Continue;
  // Whole milliseconds, with the rest carried: blocks that are not a whole number of
  // milliseconds (320 samples at 24 kHz) would otherwise count short every time.
  const uint64_t scaled = static_cast<uint64_t>(count) * 1000 + carry_;
  const uint32_t ms = static_cast<uint32_t>(scaled / cfg_.sample_rate);
  carry_ = static_cast<uint32_t>(scaled % cfg_.sample_rate);
  float r = voice_rms(samples, count);
  elapsed_ms_ += ms;
  // Track the noise floor quickly downwards, slowly upwards. It starts at the
  // quiet end so speech that begins with the first block is still detected.
  if (noise_ == 0) noise_ = cfg_.min_threshold / cfg_.noise_ratio;
  noise_ = r < noise_ ? noise_ * 0.7f + r * 0.3f : noise_ * 0.995f + r * 0.005f;

  float threshold = noise_ * cfg_.noise_ratio;
  if (threshold < cfg_.min_threshold) threshold = cfg_.min_threshold;
  if (r >= threshold) {
    speech_ms_ += ms;
    silence_ms_ = 0;
  } else if (speech_ms_ > 0) {
    silence_ms_ += ms;
  }
  // Require a little sustained speech before an end-of-speech can trigger, so a
  // single click does not count as an utterance.
  if (spoke() && silence_ms_ >= cfg_.end_silence_ms) return Result::EndOfSpeech;
  if (!spoke() && elapsed_ms_ >= cfg_.no_speech_ms) return Result::NoSpeech;
  return Result::Continue;
}

}  // namespace hg
