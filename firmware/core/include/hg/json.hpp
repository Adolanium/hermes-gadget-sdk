// Minimal JSON value, parser and serializer for the device protocol.
//
// Protocol messages are small (a few hundred bytes), so the representation
// favours simplicity over memory density. No exceptions, no RTTI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hg::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value {
 public:
  using Member = std::pair<std::string, Value>;

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool b) : type_(Type::Bool), bool_(b) {}
  Value(int n) : type_(Type::Number), num_(n) {}
  Value(unsigned n) : type_(Type::Number), num_(n) {}
  Value(long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
  Value(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
  Value(unsigned long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
  Value(unsigned long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
  Value(double n) : type_(Type::Number), num_(n) {}
  Value(const char* s) : type_(Type::String), str_(s ? s : "") {}
  Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
  Value(std::string_view s) : type_(Type::String), str_(s) {}

  static Value array() { Value v; v.type_ = Type::Array; return v; }
  static Value object() { Value v; v.type_ = Type::Object; return v; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
  double as_number(double fallback = 0) const { return is_number() ? num_ : fallback; }
  int64_t as_int(int64_t fallback = 0) const;
  // Empty string when the value is not a string.
  const std::string& as_string() const;
  std::string_view str_or(std::string_view fallback) const {
    return is_string() ? std::string_view(str_) : fallback;
  }

  // Object access. A missing key (or a non-object) yields a shared null value.
  const Value& operator[](std::string_view key) const;
  bool has(std::string_view key) const;
  Value& set(std::string_view key, Value v);
  const std::vector<Member>& members() const { return obj_; }

  // Array access. Out of range yields a shared null value.
  const Value& operator[](size_t index) const;
  const Value& at(size_t index) const { return (*this)[index]; }
  Value& push(Value v);
  const std::vector<Value>& elements() const { return arr_; }
  size_t size() const;

  std::string dump() const;
  void dump_to(std::string& out) const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<Member> obj_;
};

// Parses `text` into `out`. Returns false (and sets `error` when given) on
// malformed input or nesting deeper than `max_depth`.
bool parse(std::string_view text, Value& out, std::string* error = nullptr, int max_depth = 32);

// Appends `s` to `out` as a quoted, escaped JSON string.
void quote(std::string_view s, std::string& out);

}  // namespace hg::json
