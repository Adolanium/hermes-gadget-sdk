// Portable SHA-256, HMAC-SHA256, Base64 and hex helpers.
//
// Device authentication needs only these primitives. Keeping them in the core
// (instead of calling a platform crypto library) makes the auth path identical
// on hardware and in the simulator, and testable against published vectors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hg::crypto {

using Digest = std::array<uint8_t, 32>;

class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t len);
  void update(std::string_view s) { update(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
  Digest finish();

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t buf_len_ = 0;
  uint64_t total_ = 0;
};

Digest sha256(const uint8_t* data, size_t len);
Digest hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len);

std::string base64_encode(const uint8_t* data, size_t len);
// Returns false on malformed input (padding optional).
bool base64_decode(std::string_view in, std::vector<uint8_t>& out);

std::string hex(const uint8_t* data, size_t len);

}  // namespace hg::crypto
