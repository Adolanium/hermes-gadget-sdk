#include "hg/i18n.hpp"

#include <array>
#include <deque>

namespace hg::i18n {

// Translation tables, one per language other than English (i18n_<code>.cpp).
extern const Entry kSpanish[];
extern const size_t kSpanishCount;

namespace {

const char* const kEnglish[kMessageCount] = {
#define HG_I18N_TEXT(id, english) english,
    HG_I18N_MESSAGES(HG_I18N_TEXT)
#undef HG_I18N_TEXT
};

const LangInfo kLangs[kLangCount] = {
    {"en", "English"},
    {"es", "Español"},
};

struct Table {
  const Entry* entries;
  size_t count;
};

Table table_for(Lang lang) {
  switch (lang) {
    case Lang::Es: return {kSpanish, kSpanishCount};
    default: return {nullptr, 0};
  }
}

// Message id -> translation as written, built once per language.
const std::array<const char*, kMessageCount>& written(Lang lang) {
  static std::array<std::array<const char*, kMessageCount>, kLangCount> tables{};
  static std::array<bool, kLangCount> built{};
  const size_t l = static_cast<size_t>(lang);
  if (!built[l]) {
    const Table t = table_for(lang);
    for (size_t i = 0; i < t.count; ++i) {
      const size_t id = static_cast<size_t>(t.entries[i].id);
      if (id < kMessageCount) tables[l][id] = t.entries[i].text;
    }
    built[l] = true;
  }
  return tables[l];
}

bool is_ascii(const char* s) {
  for (; *s; ++s)
    if (static_cast<unsigned char>(*s) >= 0x80) return false;
  return true;
}

// Message id -> text as drawn: the translation folded to ASCII, else English.
const std::array<const char*, kMessageCount>& drawn(Lang lang) {
  static std::array<std::array<const char*, kMessageCount>, kLangCount> tables{};
  static std::array<bool, kLangCount> built{};
  static std::deque<std::string> folded;  // owns the folded copies; a deque never moves them
  const size_t l = static_cast<size_t>(lang);
  if (!built[l]) {
    const auto& w = written(lang);
    for (size_t i = 0; i < kMessageCount; ++i) {
      const char* s = w[i] ? w[i] : kEnglish[i];
      if (!is_ascii(s)) {
        folded.push_back(fold_ascii(s));
        s = folded.back().c_str();
      }
      tables[l][i] = s;
    }
    built[l] = true;
  }
  return tables[l];
}

// ASCII stand-ins for U+00A0..U+00FF; "" drops the character.
const char* const kLatin1[96] = {
    " ", "!", "c", "L", "", "Y", "|", "S", "", "(c)", "a", "<<", "-", "-", "(R)", "-",  // A0-AF
    "o", "+-", "2", "3", "'", "u", "P", ".", "", "1", "o", ">>", "1/4", "1/2", "3/4", "?",  // B0-BF
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",  // C0-CF
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",  // D0-DF
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  // E0-EF
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",  // F0-FF
};

}  // namespace

const LangInfo& info(Lang lang) {
  const size_t l = static_cast<size_t>(lang);
  return kLangs[l < kLangCount ? l : 0];
}

std::optional<Lang> parse(std::string_view code) {
  for (size_t i = 0; i < kLangCount; ++i)
    if (code == kLangs[i].code) return static_cast<Lang>(i);
  return std::nullopt;
}

Lang next(Lang lang) { return static_cast<Lang>((static_cast<size_t>(lang) + 1) % kLangCount); }

const char* english(Msg id) {
  const size_t i = static_cast<size_t>(id);
  return i < kMessageCount ? kEnglish[i] : "";
}

const char* translation(Lang lang, Msg id) {
  const size_t i = static_cast<size_t>(id);
  if (i >= kMessageCount || static_cast<size_t>(lang) >= kLangCount) return nullptr;
  if (lang == Lang::En) return kEnglish[i];
  return written(lang)[i];
}

const char* text(Lang lang, Msg id) {
  const size_t i = static_cast<size_t>(id);
  if (i >= kMessageCount) return "";
  if (static_cast<size_t>(lang) >= kLangCount) lang = Lang::En;
  return drawn(lang)[i];
}

std::string format(std::string_view text, std::initializer_list<std::string_view> args) {
  std::string out;
  out.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{' && i + 2 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9' && text[i + 2] == '}') {
      const size_t n = static_cast<size_t>(text[i + 1] - '0');
      if (n < args.size()) {
        out += *(args.begin() + n);
        i += 2;
        continue;
      }
    }
    out += text[i];
  }
  return out;
}

std::string fold_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
      continue;
    }
    // Decode one UTF-8 sequence; a malformed byte stands for itself as '?'.
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    uint32_t cp = len == 4 ? c & 0x07 : len == 3 ? c & 0x0F : c & 0x1F;
    bool ok = len > 1 && i + len <= s.size();
    for (size_t k = 1; ok && k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) ok = false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out += '?';
      ++i;
      continue;
    }
    i += len;
    if (cp >= 0xA0 && cp <= 0xFF) out += kLatin1[cp - 0xA0];
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0x2026) out += "...";
    else out += '?';
  }
  return out;
}

std::string control_name(Lang lang, std::string_view label) {
  static constexpr Msg kWords[] = {Msg::LabelSwipeDown, Msg::LabelHoldScreen, Msg::LabelScreen, Msg::LabelKnob};
  for (Msg m : kWords)
    if (label == english(m)) return text(lang, m);
  return std::string(label);
}

}  // namespace hg::i18n
