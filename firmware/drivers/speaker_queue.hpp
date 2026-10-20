// How much of a PCM16 write fits in the speaker's stream buffer. A FreeRTOS
// stream buffer keeps one byte free, so its room is odd whenever it holds whole
// samples. Queueing all of that room would split a sample, shift every later
// sample by a byte, and play the rest of the reply as noise.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace hg {
inline size_t whole_sample_bytes(size_t want, size_t room) {
  return std::min(want, room) & ~static_cast<size_t>(1);
}

// Expand as many mono PCM16 samples as fit in the bounded stereo scratch buffer.
// The caller repeats until all input samples have been consumed.
inline size_t mono_to_stereo(const int16_t* input, size_t input_samples,
                             int16_t* output, size_t output_samples) {
  if (!input || !output) return 0;
  const size_t pairs = std::min(input_samples, output_samples / 2);
  for (size_t i = 0; i < pairs; ++i) {
    output[2 * i] = input[i];
    output[2 * i + 1] = input[i];
  }
  return pairs;
}
}  // namespace hg
