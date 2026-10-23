#include "hg/mascot.hpp"

namespace hg::mascot {
namespace {

constexpr int kSizes[] = {64, 96, 144, 192};  // the mascot's, as mascot_data.cpp lists them
constexpr size_t kAnchorBytes = 10 * 2;

size_t frame_bytes(int size) { return static_cast<size_t>((size + 7) / 8) * static_cast<size_t>(size); }

size_t bits_bytes() {
  size_t n = 0;
  for (int s : kSizes) n += 3 * frame_bytes(s);
  return n;
}

size_t offset(int size, Frame frame) {
  size_t off = 0;
  for (int s : kSizes) {
    if (s == size) return off + static_cast<size_t>(frame) * frame_bytes(s);
    off += 3 * frame_bytes(s);
  }
  return off;
}

}  // namespace

bool parse(std::vector<uint8_t> data, Face& out) {
  const size_t n = bits_bytes();
  if (data.size() != n + kAnchorBytes) return false;
  int16_t v[10];
  for (size_t i = 0; i < 10; ++i) v[i] = static_cast<int16_t>(data[n + 2 * i] | (data[n + 2 * i + 1] << 8));
  out.anchors = {{v[0], v[1]}, {v[2], v[3]}, {v[4], v[5]}, v[6], v[7], v[8], v[9]};
  data.resize(n);
  out.bits = std::move(data);
  return true;
}

Bitmap pick(int max_size, Frame frame, const Face* face) {
  const Bitmap* b = pick(max_size, frame);
  if (!face) return *b;
  return {b->size, frame, face->bits.data() + offset(b->size, frame)};
}

const Anchors& anchors(const Face* face) { return face ? face->anchors : anchors(); }

}  // namespace hg::mascot
