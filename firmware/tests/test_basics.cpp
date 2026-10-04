#include <cstring>
#include <string>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/crypto.hpp"
#include "hg/json.hpp"
#include "hg/protocol.hpp"
#include "hg/vad.hpp"

using hg::json::Value;

namespace {

std::string sha_hex(const std::string& s) {
  auto d = hg::crypto::sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  return hg::crypto::hex(d.data(), d.size());
}

std::string hmac_hex(const std::string& key, const std::string& msg) {
  auto d = hg::crypto::hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                                   reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  return hg::crypto::hex(d.data(), d.size());
}

}  // namespace

TEST("json: round trip preserves structure and escapes") {
  Value v;
  std::string err;
  CHECK(hg::json::parse(R"({"a":1,"b":[true,false,null],"c":"q\"\\\n\t","d":-2.5,"e":{}})", v, &err));
  CHECK_EQ(v["a"].as_int(), 1);
  CHECK(v["b"].is_array());
  CHECK_EQ(v["b"].size(), size_t(3));
  CHECK(v["b"][0].as_bool());
  CHECK(v["b"][2].is_null());
  CHECK_EQ(v["c"].as_string(), std::string("q\"\\\n\t"));
  CHECK_EQ(v["d"].as_number(), -2.5);
  Value back;
  CHECK(hg::json::parse(v.dump(), back));
  CHECK_EQ(back.dump(), v.dump());
}

TEST("json: unicode escapes decode to utf-8, including surrogate pairs") {
  Value v;
  CHECK(hg::json::parse(R"("\u00e9\u2014\ud83d\ude00")", v));
  CHECK_EQ(v.as_string(), std::string("\xC3\xA9\xE2\x80\x94\xF0\x9F\x98\x80"));
}

TEST("json: malformed input and excessive nesting are rejected") {
  Value v;
  CHECK(!hg::json::parse("{\"a\":}", v));
  CHECK(!hg::json::parse("[1,2", v));
  CHECK(!hg::json::parse("\"unterminated", v));
  CHECK(!hg::json::parse("{} extra", v));
  CHECK(!hg::json::parse("\"\\ud800\"", v));
  std::string deep(40, '[');
  deep += std::string(40, ']');
  CHECK(!hg::json::parse(deep, v));
}

TEST("json: missing keys and wrong types fall back safely") {
  Value v;
  CHECK(hg::json::parse(R"({"n":"x"})", v));
  CHECK(v["missing"].is_null());
  CHECK_EQ(v["n"].as_int(7), int64_t(7));
  CHECK_EQ(v["missing"]["deeper"].as_string(), std::string());
  CHECK_EQ(Value(3).dump(), std::string("3"));
  CHECK_EQ(Value(0.5).dump(), std::string("0.5"));
}

TEST("crypto: sha256 matches FIPS vectors") {
  CHECK_EQ(sha_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(sha_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(sha_hex(std::string(1000, 'a')),
           std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

TEST("crypto: hmac-sha256 matches RFC 4231 vectors") {
  CHECK_EQ(hmac_hex(std::string(20, '\x0b'), "Hi There"),
           std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  CHECK_EQ(hmac_hex("Jefe", "what do ya want for nothing?"),
           std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
  CHECK_EQ(hmac_hex(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First"),
           std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

TEST("crypto: base64 round trips every length") {
  for (size_t n = 0; n < 40; ++n) {
    std::vector<uint8_t> data(n);
    for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint8_t>(i * 37 + 11);
    std::string enc = hg::crypto::base64_encode(data.data(), data.size());
    std::vector<uint8_t> dec;
    CHECK(hg::crypto::base64_decode(enc, dec));
    CHECK(dec == data);
  }
  std::vector<uint8_t> out;
  CHECK(!hg::crypto::base64_decode("ab$d", out));
}

TEST("protocol: identity and auth mac match the host implementation") {
  // Vectors shared with tests/test_protocol_vectors.py (computed with Python hashlib/hmac).
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
  std::string id = hg::proto::device_id_for_key(key, sizeof(key));
  CHECK_EQ(id, std::string("hg-630dcd2966c43366"));
  CHECK_EQ(hg::proto::auth_mac(key, sizeof(key), id, "bm9uY2Utbm9uY2Utbm9uY2U="),
           std::string("AMUEF53Phk8+1vHw7R8PgiDwlPd8rXUx+cgyYv1YJX0="));
  CHECK_EQ(hg::proto::ota_mac(key, sizeof(key), id, "AAECAwQFBgcICQoLDA0ODw==",
                              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 1234),
           std::string("ieymn+y1CMEJ5uX8yGz8VrXxT2KKjSOnzJC5qagTTwk="));
}

TEST("protocol: binary frame header round trips") {
  uint8_t buf[8] = {0};
  hg::proto::write_binary_header(buf, hg::proto::Channel::Audio, 7, 0x1234);
  hg::proto::BinaryFrame f;
  CHECK(hg::proto::parse_binary(buf, sizeof(buf), f));
  CHECK(f.channel == hg::proto::Channel::Audio);
  CHECK_EQ(f.stream, uint8_t(7));
  CHECK_EQ(f.seq, uint16_t(0x1234));
  CHECK_EQ(f.payload_len, size_t(4));
  uint8_t firmware[4] = {0x03, 2, 0, 0};
  CHECK(hg::proto::parse_binary(firmware, sizeof(firmware), f));
  CHECK(f.channel == hg::proto::Channel::Firmware);
  uint8_t bad[4] = {0x09, 0, 0, 0};
  CHECK(!hg::proto::parse_binary(bad, sizeof(bad), f));
}

TEST("canvas: word wrap keeps words whole and splits overlong ones") {
  auto lines = hg::wrap_text("the quick brown fox jumps", 10);
  CHECK_EQ(lines.size(), size_t(3));
  CHECK_EQ(lines[0], std::string("the quick"));
  CHECK_EQ(lines[1], std::string("brown fox"));
  CHECK_EQ(lines[2], std::string("jumps"));
  auto split = hg::wrap_text("abcdefghijklmno", 6);
  CHECK_EQ(split.size(), size_t(3));
  CHECK_EQ(split[2], std::string("mno"));
  auto paras = hg::wrap_text("a\n\nb", 10);
  CHECK_EQ(paras.size(), size_t(3));
  CHECK_EQ(paras[1], std::string());
}

TEST("canvas: drawing respects the row clip") {
  std::vector<uint16_t> px(10 * 10, 0);
  hg::Canvas c(px.data(), 10, 10, false);
  c.set_clip_rows(2, 4);
  c.fill_rect(0, 0, 10, 10, 0xFFFF);
  int lit = 0;
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 10; ++x)
      if (px[static_cast<size_t>(y * 10 + x)]) {
        ++lit;
        CHECK(y >= 2 && y < 4);
      }
  CHECK_EQ(lit, 20);
}

TEST("vad: speech followed by silence ends the utterance") {
  hg::Vad vad;
  hg::Vad::Config cfg;
  vad.reset(cfg);
  std::vector<int16_t> quiet(320, 20), loud(320);
  for (size_t i = 0; i < loud.size(); ++i) loud[i] = static_cast<int16_t>((i % 2) ? 6000 : -6000);
  for (int i = 0; i < 10; ++i) CHECK(vad.feed(quiet.data(), quiet.size()) == hg::Vad::Result::Continue);
  for (int i = 0; i < 25; ++i) vad.feed(loud.data(), loud.size());
  CHECK(vad.heard_speech());
  hg::Vad::Result r = hg::Vad::Result::Continue;
  for (int i = 0; i < 60 && r == hg::Vad::Result::Continue; ++i) r = vad.feed(quiet.data(), quiet.size());
  CHECK(r == hg::Vad::Result::EndOfSpeech);
}

TEST("vad: silence alone gives up") {
  hg::Vad vad;
  hg::Vad::Config cfg;
  cfg.no_speech_ms = 1000;
  vad.reset(cfg);
  std::vector<int16_t> quiet(320, 10);
  hg::Vad::Result r = hg::Vad::Result::Continue;
  for (int i = 0; i < 100 && r == hg::Vad::Result::Continue; ++i) r = vad.feed(quiet.data(), quiet.size());
  CHECK(r == hg::Vad::Result::NoSpeech);
}
