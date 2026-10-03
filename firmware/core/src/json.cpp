#include "hg/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hg::json {
namespace {

const Value& null_value() {
  static const Value v;
  return v;
}

const std::string& empty_string() {
  static const std::string s;
  return s;
}

void append_utf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  Parser(std::string_view text, int max_depth) : s_(text), max_depth_(max_depth) {}

  bool run(Value& out, std::string* error) {
    skip_ws();
    if (!value(out, 0)) {
      if (error) *error = err_ + " at offset " + std::to_string(pos_);
      return false;
    }
    skip_ws();
    if (pos_ != s_.size()) {
      if (error) *error = "trailing characters at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  bool fail(const char* msg) {
    if (err_.empty()) err_ = msg;
    return false;
  }

  void skip_ws() {
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++pos_;
    }
  }

  bool literal(std::string_view word) {
    if (s_.substr(pos_, word.size()) != word) return fail("invalid literal");
    pos_ += word.size();
    return true;
  }

  bool value(Value& out, int depth) {
    if (depth > max_depth_) return fail("nesting too deep");
    if (pos_ >= s_.size()) return fail("unexpected end of input");
    switch (s_[pos_]) {
      case '{': return object(out, depth);
      case '[': return array(out, depth);
      case '"': {
        std::string str;
        if (!string(str)) return false;
        out = Value(std::move(str));
        return true;
      }
      case 't': out = Value(true); return literal("true");
      case 'f': out = Value(false); return literal("false");
      case 'n': out = Value(); return literal("null");
      default: return number(out);
    }
  }

  bool number(Value& out) {
    size_t start = pos_;
    if (s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size() || s_[pos_] < '0' || s_[pos_] > '9') return fail("invalid number");
    while (pos_ < s_.size()) {
      char c = s_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        ++pos_;
      } else {
        break;
      }
    }
    std::string buf(s_.substr(start, pos_ - start));
    char* end = nullptr;
    double d = std::strtod(buf.c_str(), &end);
    if (end != buf.c_str() + buf.size()) return fail("invalid number");
    out = Value(d);
    return true;
  }

  bool hex4(uint32_t& cp) {
    if (pos_ + 4 > s_.size()) return fail("truncated unicode escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
      else return fail("invalid unicode escape");
    }
    return true;
  }

  bool string(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) break;
      char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t low;
            if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
              return fail("unpaired surrogate");
            }
            pos_ += 2;
            if (!hex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return fail("invalid surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          append_utf8(cp, out);
          break;
        }
        default: return fail("invalid escape");
      }
    }
    return fail("unterminated string");
  }

  bool array(Value& out, int depth) {
    ++pos_;
    out = Value::array();
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      Value item;
      skip_ws();
      if (!value(item, depth + 1)) return false;
      out.push(std::move(item));
      skip_ws();
      if (pos_ >= s_.size()) return fail("unterminated array");
      char c = s_[pos_++];
      if (c == ']') return true;
      if (c != ',') return fail("expected ',' or ']'");
    }
  }

  bool object(Value& out, int depth) {
    ++pos_;
    out = Value::object();
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected object key");
      std::string key;
      if (!string(key)) return false;
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_++] != ':') return fail("expected ':'");
      skip_ws();
      Value item;
      if (!value(item, depth + 1)) return false;
      out.set(key, std::move(item));
      skip_ws();
      if (pos_ >= s_.size()) return fail("unterminated object");
      char c = s_[pos_++];
      if (c == '}') return true;
      if (c != ',') return fail("expected ',' or '}'");
    }
  }

  std::string_view s_;
  size_t pos_ = 0;
  int max_depth_;
  std::string err_;
};

}  // namespace

int64_t Value::as_int(int64_t fallback) const {
  if (!is_number() || !std::isfinite(num_)) return fallback;
  return static_cast<int64_t>(num_);
}

const std::string& Value::as_string() const { return is_string() ? str_ : empty_string(); }

const Value& Value::operator[](std::string_view key) const {
  if (!is_object()) return null_value();
  for (const auto& m : obj_) {
    if (m.first == key) return m.second;
  }
  return null_value();
}

bool Value::has(std::string_view key) const {
  if (!is_object()) return false;
  for (const auto& m : obj_) {
    if (m.first == key) return true;
  }
  return false;
}

Value& Value::set(std::string_view key, Value v) {
  if (!is_object()) *this = object();
  for (auto& m : obj_) {
    if (m.first == key) {
      m.second = std::move(v);
      return *this;
    }
  }
  obj_.emplace_back(std::string(key), std::move(v));
  return *this;
}

const Value& Value::operator[](size_t index) const {
  if (!is_array() || index >= arr_.size()) return null_value();
  return arr_[index];
}

Value& Value::push(Value v) {
  if (!is_array()) *this = array();
  arr_.push_back(std::move(v));
  return *this;
}

size_t Value::size() const {
  if (is_array()) return arr_.size();
  if (is_object()) return obj_.size();
  return 0;
}

void quote(std::string_view s, std::string& out) {
  static const char* kHex = "0123456789abcdef";
  out.push_back('"');
  for (char ch : s) {
    unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void Value::dump_to(std::string& out) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      if (!std::isfinite(num_)) {
        out += "null";
        break;
      }
      char buf[32];
      double integral;
      if (std::modf(num_, &integral) == 0.0 && std::fabs(num_) < 9.007199254740992e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
      } else {
        std::snprintf(buf, sizeof(buf), "%.15g", num_);
      }
      out += buf;
      break;
    }
    case Type::String: quote(str_, out); break;
    case Type::Array: {
      out.push_back('[');
      for (size_t i = 0; i < arr_.size(); ++i) {
        if (i) out.push_back(',');
        arr_[i].dump_to(out);
      }
      out.push_back(']');
      break;
    }
    case Type::Object: {
      out.push_back('{');
      for (size_t i = 0; i < obj_.size(); ++i) {
        if (i) out.push_back(',');
        quote(obj_[i].first, out);
        out.push_back(':');
        obj_[i].second.dump_to(out);
      }
      out.push_back('}');
      break;
    }
  }
}

std::string Value::dump() const {
  std::string out;
  dump_to(out);
  return out;
}

bool parse(std::string_view text, Value& out, std::string* error, int max_depth) {
  Parser p(text, max_depth);
  return p.run(out, error);
}

}  // namespace hg::json
