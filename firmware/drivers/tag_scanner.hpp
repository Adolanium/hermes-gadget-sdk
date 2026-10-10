#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hg {

// Finds "<tag><value>\0" in a byte stream that arrives in chunks of any size,
// and keeps the value. The firmware image carries "HGBOARD=<board>\0" somewhere
// in its data; the device streams the image straight to flash, so this is how
// it learns which board the image was built for before selecting it.
class TagScanner {
 public:
  explicit TagScanner(std::string_view tag, size_t max_value = 64) : tag_(tag), max_value_(max_value) {}

  // Returns true when this call completed the value.
  bool feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      if (done_) return false;
      char c = static_cast<char>(data[i]);
      if (matched_ < tag_.size()) {
        if (c == tag_[matched_]) {
          ++matched_;
        } else {
          matched_ = c == tag_[0] ? 1 : 0;
        }
        continue;
      }
      if (c == '\0' && value_.empty()) {
        // "<tag>\0" names nothing (a copy of the bare tag); keep looking.
        matched_ = 0;
        continue;
      }
      if (c == '\0' || value_.size() >= max_value_) {
        done_ = true;
        return true;
      }
      value_.push_back(c);
    }
    return false;
  }

  bool found() const { return done_; }
  const std::string& value() const { return value_; }

 private:
  std::string_view tag_;
  size_t max_value_;
  size_t matched_ = 0;
  std::string value_;
  bool done_ = false;
};

}  // namespace hg
