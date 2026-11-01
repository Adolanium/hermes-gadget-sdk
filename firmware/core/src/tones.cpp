#include "hg/tones.hpp"

#include <algorithm>
#include <cmath>

namespace hg {

std::vector<int16_t> synth_tones(const Tone* tones, size_t count, uint32_t rate, uint16_t lead_ms) {
  constexpr double kPi = 3.14159265358979323846;
  const size_t lead = static_cast<size_t>(rate) * lead_ms / 1000;
  size_t total = lead;
  for (size_t t = 0; t < count; ++t) total += static_cast<size_t>(rate) * (tones[t].ms + tones[t].gap_ms) / 1000;
  std::vector<int16_t> pcm(total, 0);
  const size_t ramp = std::max<size_t>(1, static_cast<size_t>(rate) * 5 / 1000);
  size_t at = lead;
  for (size_t t = 0; t < count; ++t) {
    const Tone& tone = tones[t];
    const size_t n = static_cast<size_t>(rate) * tone.ms / 1000;
    const double step = 2.0 * kPi * tone.hz / static_cast<double>(rate);
    const size_t edge = std::min(ramp, n / 2);
    for (size_t i = 0; i < n; ++i) {
      double gain = 1.0;
      if (edge && i < edge) gain = static_cast<double>(i) / static_cast<double>(edge);
      if (edge && n - i < edge) gain = static_cast<double>(n - i) / static_cast<double>(edge);
      pcm[at + i] = static_cast<int16_t>(tone.level * gain * std::sin(step * static_cast<double>(i)));
    }
    at += static_cast<size_t>(rate) * (tone.ms + tone.gap_ms) / 1000;
  }
  return pcm;
}

}  // namespace hg
