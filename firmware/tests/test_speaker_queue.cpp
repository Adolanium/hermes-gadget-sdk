#include "check.hpp"
#include "speaker_queue.hpp"

TEST("whole_sample_bytes: a nearly full buffer takes whole samples only") {
  CHECK_EQ(hg::whole_sample_bytes(1280, 511), 510u);  // #73: 511 bytes of room used to take 511
  CHECK_EQ(hg::whole_sample_bytes(1280, 1), 0u);
  CHECK_EQ(hg::whole_sample_bytes(1280, 0), 0u);
}

TEST("whole_sample_bytes: a write that fits is queued whole") {
  CHECK_EQ(hg::whole_sample_bytes(1280, 1281), 1280u);
  CHECK_EQ(hg::whole_sample_bytes(1280, 49151), 1280u);
}


TEST("mono-to-stereo expansion preserves all samples across bounded writes") {
  constexpr size_t input_count = 512;
  int16_t input[input_count];
  int16_t stereo[512];
  for (size_t i = 0; i < input_count; ++i) input[i] = static_cast<int16_t>(static_cast<int>(i) - 256);
  size_t offset = 0;
  size_t writes = 0;
  while (offset < input_count) {
    const size_t consumed = hg::mono_to_stereo(input + offset, input_count - offset,
                                                stereo, 512);
    CHECK(consumed > 0);
    if (consumed == 0) break;
    for (size_t i = 0; i < consumed; ++i) {
      CHECK_EQ(stereo[2 * i], input[offset + i]);
      CHECK_EQ(stereo[2 * i + 1], input[offset + i]);
    }
    offset += consumed;
    ++writes;
  }
  CHECK_EQ(offset, input_count);
  CHECK_EQ(writes, size_t(2));
}
