#include "hg/mascot.hpp"

#include "hg/crypto.hpp"

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

bool parse(std::string_view base64, Face& out) {
  const size_t n = bits_bytes(), total = n + kAnchorBytes;
  if (base64.size() != (total + 2) / 3 * 4) return false;
  std::unique_ptr<uint8_t, Face::Free> data(static_cast<uint8_t*>(std::malloc(total)));
  size_t len = 0;
  if (!data || !crypto::base64_decode(base64, data.get(), total, len) || len != total) return false;
  const uint8_t* a = data.get() + n;
  int16_t v[10];
  for (size_t i = 0; i < 10; ++i) {
    v[i] = static_cast<int16_t>(a[2 * i] | (a[2 * i + 1] << 8));
    if (v[i] < 0 || v[i] > 1000) return false;  // off the bitmap: the effects and blink repaints would miss
  }
  if (v[6] > v[7] || v[8] > v[9]) return false;  // the eye and mouth rows, top first
  out.anchors = {{v[0], v[1]}, {v[2], v[3]}, {v[4], v[5]}, v[6], v[7], v[8], v[9]};
  out.bits = std::move(data);
  return true;
}

Bitmap pick(int max_size, Frame frame, const Face* face) {
  const Bitmap* b = pick(max_size, frame);
  if (!face) return *b;
  return {b->size, frame, face->bits.get() + offset(b->size, frame)};
}

const Anchors& anchors(const Face* face) { return face ? face->anchors : anchors(); }

}  // namespace hg::mascot
