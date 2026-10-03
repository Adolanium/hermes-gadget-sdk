// Hermes Gadget Protocol v1 — device side constants and helpers.
// The normative description lives in docs/protocol.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hg/json.hpp"

namespace hg::proto {

constexpr int kVersion = 1;
constexpr const char* kSubprotocol = "hermes-gadget.v1";
// Domain-separation prefix for the auth MAC: HMAC(key, prefix + device_id + "|" + nonce).
constexpr const char* kAuthContext = "hermes-gadget/v1|";
constexpr const char* kDeviceIdPrefix = "hg-";

// Binary frame layout: [channel u8][stream u8][seq u16 LE][payload ...]
constexpr size_t kBinaryHeader = 4;
enum class Channel : uint8_t { Audio = 0x01, Image = 0x02 };

struct BinaryFrame {
  Channel channel;
  uint8_t stream;
  uint16_t seq;
  const uint8_t* payload;
  size_t payload_len;
};

bool parse_binary(const uint8_t* data, size_t len, BinaryFrame& out);
void write_binary_header(uint8_t* dst, Channel channel, uint8_t stream, uint16_t seq);

// Device identity: "hg-" + first 16 hex chars of SHA-256(device key).
std::string device_id_for_key(const uint8_t* key, size_t len);
// Base64 HMAC proving possession of the device key for a server nonce.
std::string auth_mac(const uint8_t* key, size_t key_len, std::string_view device_id, std::string_view nonce);

json::Value message(std::string_view type);

}  // namespace hg::proto
