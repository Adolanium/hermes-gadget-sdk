#include <cstring>
#include <string>
#include <vector>

#include "check.hpp"
#include "tag_scanner.hpp"

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// Feeds `image` in pieces of `chunk` bytes; returns the number of feeds that reported completion.
int feed_in_chunks(hg::TagScanner& scanner, const std::vector<uint8_t>& image, size_t chunk) {
  int completions = 0;
  for (size_t at = 0; at < image.size(); at += chunk) {
    size_t n = std::min(chunk, image.size() - at);
    if (scanner.feed(image.data() + at, n)) ++completions;
  }
  return completions;
}

}  // namespace

TEST("TagScanner: finds the tag whatever the chunk boundaries") {
  std::string body = "\xE9\x05junk HGBOAR not it ... HGBOARD=esp32s3-breadboard";
  body.push_back('\0');
  body += " trailing bytes HGBOARD=other";
  std::vector<uint8_t> image = bytes(body);
  for (size_t chunk : {1u, 2u, 3u, 7u, 16u, 4096u}) {
    hg::TagScanner scanner("HGBOARD=");
    CHECK_EQ(feed_in_chunks(scanner, image, chunk), 1);
    CHECK(scanner.found());
    CHECK_EQ(scanner.value(), std::string("esp32s3-breadboard"));  // the first tag wins; a later one is ignored
  }
}

TEST("TagScanner: a nameless copy of the tag is skipped") {
  // Images built from main before this fix (never released) store the bare search text ahead of the real tag.
  std::string body = "HGBOARD=";
  body.push_back('\0');
  body += "code HGBOARD=esp32s3-touch-amoled-1.75c";
  body.push_back('\0');
  for (size_t chunk : {1u, 5u, 4096u}) {
    hg::TagScanner scanner("HGBOARD=");
    CHECK_EQ(feed_in_chunks(scanner, bytes(body), chunk), 1);
    CHECK_EQ(scanner.value(), std::string("esp32s3-touch-amoled-1.75c"));
  }
}

TEST("TagScanner: an image without the tag is never found") {
  hg::TagScanner scanner("HGBOARD=");
  std::vector<uint8_t> image = bytes("HGBOARD HGBOAR= HG BOARD=nope");
  CHECK_EQ(feed_in_chunks(scanner, image, 5), 0);
  CHECK(!scanner.found());
}

TEST("TagScanner: a value that never ends is cut at the limit") {
  hg::TagScanner scanner("HGBOARD=", 8);
  std::vector<uint8_t> image = bytes("HGBOARD=abcdefghijklmnop");
  CHECK_EQ(feed_in_chunks(scanner, image, 4), 1);
  CHECK_EQ(scanner.value(), std::string("abcdefgh"));
}
