#include "hg/protocol.hpp"

#include "hg/crypto.hpp"

namespace hg::proto {

bool parse_binary(const uint8_t* data, size_t len, BinaryFrame& out) {
  if (len < kBinaryHeader) return false;
  uint8_t ch = data[0];
  if (ch != static_cast<uint8_t>(Channel::Audio) && ch != static_cast<uint8_t>(Channel::Image)) return false;
  out.channel = static_cast<Channel>(ch);
  out.stream = data[1];
  out.seq = static_cast<uint16_t>(data[2] | (data[3] << 8));
  out.payload = data + kBinaryHeader;
  out.payload_len = len - kBinaryHeader;
  return true;
}

void write_binary_header(uint8_t* dst, Channel channel, uint8_t stream, uint16_t seq) {
  dst[0] = static_cast<uint8_t>(channel);
  dst[1] = stream;
  dst[2] = static_cast<uint8_t>(seq & 0xFF);
  dst[3] = static_cast<uint8_t>(seq >> 8);
}

std::string device_id_for_key(const uint8_t* key, size_t len) {
  crypto::Digest d = crypto::sha256(key, len);
  return std::string(kDeviceIdPrefix) + crypto::hex(d.data(), 8);
}

std::string auth_mac(const uint8_t* key, size_t key_len, std::string_view device_id, std::string_view nonce) {
  std::string msg = kAuthContext;
  msg.append(device_id.data(), device_id.size());
  msg.push_back('|');
  msg.append(nonce.data(), nonce.size());
  crypto::Digest mac =
      crypto::hmac_sha256(key, key_len, reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  return crypto::base64_encode(mac.data(), mac.size());
}

json::Value message(std::string_view type) {
  json::Value v = json::Value::object();
  v.set("type", type);
  return v;
}

}  // namespace hg::proto
