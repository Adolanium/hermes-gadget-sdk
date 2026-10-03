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
  };

  enum class Result : uint8_t { Continue, EndOfSpeech, NoSpeech };

  void reset(const Config& cfg);
  Result feed(const int16_t* samples, size_t count);
  bool heard_speech() const { return speech_ms_ > 0; }

 private:
  Config cfg_;
  float noise_ = 0;
  uint32_t elapsed_ms_ = 0;
  uint32_t speech_ms_ = 0;
  uint32_t silence_ms_ = 0;
};

}  // namespace hg
