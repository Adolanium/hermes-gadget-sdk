#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hg {

// One note of a short local sound: a sine at `hz` for `ms`, then `gap_ms` of silence.
// `level` is the peak sample (the timer alarm's beeps use 9000 of 32767).
struct Tone {
  uint16_t hz;
  uint16_t ms;
  uint16_t gap_ms;
  uint16_t level;
};

// Silence before a short sound's first note. A board's amplifier and DAC take a moment
// to come up when a stream starts (an amplifier enabled with the first audio, a codec
// reopened after a doze, a DAC's soft start), and a note played into that moment is
// lost: on an AMOLED-2.06 the first note of a two-note sound went missing.
constexpr uint16_t kToneLeadMs = 150;

// `lead_ms` of silence, then the notes, as 16-bit mono PCM at `rate`; each note has
// 5 ms ramps so it doesn't click.
std::vector<int16_t> synth_tones(const Tone* tones, size_t count, uint32_t rate, uint16_t lead_ms = 0);

}  // namespace hg
