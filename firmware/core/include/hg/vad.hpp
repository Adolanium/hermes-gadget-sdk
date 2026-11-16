// Energy-based voice activity detection for hands-free (tap) talk mode.
//
// Deliberately simple: an adaptive noise floor and a speech threshold above
// it. Good enough to end an utterance after a pause; boards with a DSP wake
// word / VAD engine can replace it behind the same interface later.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hg {

// RMS of a PCM16 block in [0, 32768].
float rms(const int16_t* samples, size_t count);
// Maps RMS to a perceptual 0..100 meter value (log scale).
uint8_t level_percent(float rms_value);

class Vad {
 public:
  struct Config {
    uint32_t sample_rate = 16000;
    uint32_t end_silence_ms = 900;   // silence after speech that ends the utterance
    uint32_t no_speech_ms = 6000;    // give up if nobody speaks at all
    float min_threshold = 500.0f;    // absolute RMS floor for "speech"
    float noise_ratio = 3.0f;        // speech must exceed noise floor * ratio
    uint32_t lowpass_hz = 3400;      // levels are measured below this (0: the whole band)
  };

  enum class Result : uint8_t { Continue, EndOfSpeech, NoSpeech };

  void reset(const Config& cfg);
  Result feed(const int16_t* samples, size_t count);
  bool heard_speech() const { return speech_ms_ > 0; }
  // Enough speech to be an utterance rather than a click (what EndOfSpeech requires).
  static constexpr uint32_t kMinSpeechMs = 200;
  bool spoke() const { return speech_ms_ >= kMinSpeechMs; }
  // How much audio has been fed since reset().
  uint32_t heard_ms() const { return elapsed_ms_; }

 private:
  // The level of a block as speech sees it: through a second-order low-pass, so a whistle from
  // the board's electronics above the voice band (a StopWatch records one at 10.4 kHz, as loud
  // as speech) is neither speech nor noise.
  float voice_rms(const int16_t* samples, size_t count);
  Config cfg_;
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;  // the low-pass, and its state
  float z1_ = 0, z2_ = 0;
  float noise_ = 0;
  uint32_t elapsed_ms_ = 0;
  uint32_t speech_ms_ = 0;
  uint32_t carry_ = 0;  // samples * 1000 not yet a whole millisecond
  uint32_t silence_ms_ = 0;
};

}  // namespace hg
