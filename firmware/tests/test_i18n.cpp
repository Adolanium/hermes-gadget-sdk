// The translation catalogue (hg/i18n.hpp): lookups, placeholders, folding to
// the font, and that translated one-line text fits wherever the English does.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "hg/canvas.hpp"
#include "hg/font.hpp"
#include "hg/i18n.hpp"
#include "hg/ui.hpp"

using hg::i18n::Lang;
using hg::i18n::Msg;

namespace {

std::vector<Lang> translations() {
  std::vector<Lang> out;
  for (size_t i = 1; i < hg::i18n::kLangCount; ++i) out.push_back(static_cast<Lang>(i));
  return out;
}

std::vector<Msg> all_messages() {
  std::vector<Msg> out;
  for (size_t i = 0; i < hg::i18n::kMessageCount; ++i) out.push_back(static_cast<Msg>(i));
  return out;
}

// The placeholders a text uses: {0} and {2} gives "02".
std::string placeholders(std::string_view s) {
  std::set<char> found;
  for (size_t i = 0; i + 2 < s.size(); ++i)
    if (s[i] == '{' && s[i + 1] >= '0' && s[i + 1] <= '9' && s[i + 2] == '}') found.insert(s[i + 1]);
  return std::string(found.begin(), found.end());
}

}  // namespace

TEST("i18n: every language has a code, a name and parses back") {
  CHECK(hg::i18n::parse("en") == Lang::En);
  CHECK(hg::i18n::parse("es") == Lang::Es);
  CHECK(!hg::i18n::parse("xx"));
  CHECK(!hg::i18n::parse(""));
  CHECK(!hg::i18n::parse("EN"));
  std::set<std::string> codes;
  for (size_t i = 0; i < hg::i18n::kLangCount; ++i) {
    const auto lang = static_cast<Lang>(i);
    const auto& info = hg::i18n::info(lang);
    CHECK(info.code && info.name && *info.name);
    CHECK(hg::i18n::parse(info.code) == lang);
    codes.insert(info.code);
  }
  CHECK_EQ(codes.size(), hg::i18n::kLangCount);
  // The settings menu steps through every language and comes back.
  Lang l = Lang::En;
  for (size_t i = 0; i < hg::i18n::kLangCount; ++i) l = hg::i18n::next(l);
  CHECK(l == Lang::En);
}

TEST("i18n: English is the catalogue's own text, and other languages fall back to it") {
  CHECK_EQ(std::string(hg::i18n::text(Lang::En, Msg::Ready)), std::string("Ready"));
  CHECK_EQ(std::string(hg::i18n::english(Msg::HiImHermes)), std::string("Hi, I'm Hermes"));
  CHECK_EQ(std::string(hg::i18n::text(Lang::Es, Msg::Ready)), std::string("Listo"));
  for (Msg m : all_messages()) {
    // Every message reads as something, in every language.
    for (size_t l = 0; l < hg::i18n::kLangCount; ++l) CHECK(*hg::i18n::text(static_cast<Lang>(l), m));
    CHECK_EQ(std::string(hg::i18n::translation(Lang::En, m)), std::string(hg::i18n::english(m)));
  }
  // An unknown language reads as English rather than failing.
  CHECK_EQ(std::string(hg::i18n::text(static_cast<Lang>(200), Msg::Ready)), std::string("Ready"));
}

TEST("i18n: each translation is complete and keeps the English placeholders") {
  for (Lang lang : translations()) {
    for (Msg m : all_messages()) {
      const char* t = hg::i18n::translation(lang, m);
      if (!t) {
        check::fail(__FILE__, __LINE__,
                    std::string(hg::i18n::info(lang).code) + " has no text for \"" + hg::i18n::english(m) + "\"");
        continue;
      }
      if (placeholders(t) != placeholders(hg::i18n::english(m)))
        check::fail(__FILE__, __LINE__, std::string(hg::i18n::info(lang).code) + " \"" + t +
                                            "\" does not use the placeholders of \"" + hg::i18n::english(m) + "\"");
    }
  }
}

TEST("i18n: drawn text is in the font: ASCII and Latin-1, as written") {
  for (size_t l = 0; l < hg::i18n::kLangCount; ++l) {
    for (Msg m : all_messages()) {
      const std::string_view t = hg::i18n::text(static_cast<Lang>(l), m);
      for (size_t i = 0; i < t.size();) {
        const char32_t cp = hg::font::next(t, i);
        if (cp != '\n' && !hg::font::drawable(cp)) {
          check::fail(__FILE__, __LINE__, std::string("not in the font: \"") + std::string(t) + "\"");
          break;
        }
      }
    }
  }
  // Written with its accents and drawn with them.
  CHECK_EQ(std::string(hg::i18n::text(Lang::Es, Msg::DeviceInfo)), std::string("Información"));
  CHECK_EQ(std::string(hg::i18n::text(Lang::Es, Msg::ToneFinished)), std::string("Tono terminado. ¿Lo oíste?"));
}

TEST("i18n: weekday and month names are at most 3 columns in every language") {
  // The clock face budgets three columns per name (Msg::ClockDate).
  const Msg names[] = {
      Msg::WeekdaySun, Msg::WeekdayMon, Msg::WeekdayTue, Msg::WeekdayWed,
      Msg::WeekdayThu, Msg::WeekdayFri, Msg::WeekdaySat,
      Msg::MonthJan,   Msg::MonthFeb,   Msg::MonthMar,   Msg::MonthApr,
      Msg::MonthMay,   Msg::MonthJun,   Msg::MonthJul,   Msg::MonthAug,
      Msg::MonthSep,   Msg::MonthOct,   Msg::MonthNov,   Msg::MonthDec,
  };
  for (size_t l = 0; l < hg::i18n::kLangCount; ++l) {
    const auto lang = static_cast<Lang>(l);
    for (Msg m : names) {
      const std::string_view t = hg::i18n::text(lang, m);
      if (hg::font::length(t) > 3)
        check::fail(__FILE__, __LINE__,
                    std::string(hg::i18n::info(lang).code) + " \"" + std::string(t) + "\" is longer than 3 columns");
    }
  }
}

TEST("i18n: folding to the font keeps Latin-1 and replaces the rest") {
  using hg::i18n::fold_to_font;
  CHECK_EQ(fold_to_font("¿Cómo estás? ¡Sí! Ñandú"), std::string("¿Cómo estás? ¡Sí! Ñandú"));
  CHECK_EQ(fold_to_font("“quoted” – it’s…"), std::string("\"quoted\" - it's..."));
  CHECK_EQ(fold_to_font("日本 🙂"), std::string("?? ?"));
  CHECK_EQ(fold_to_font("a\xC3"), std::string("a?"));
  CHECK_EQ(fold_to_font("line\nbreak"), std::string("line\nbreak"));
}

TEST("i18n: folding keeps ASCII and finds the nearest letter for the rest") {
  using hg::i18n::fold_ascii;
  CHECK_EQ(fold_ascii("Ready"), std::string("Ready"));
  CHECK_EQ(fold_ascii("¿Cómo estás? ¡Sí! Añadir"), std::string("Como estas? Si! Anadir"));
  CHECK_EQ(fold_ascii("Ça va, garçon, über"), std::string("Ca va, garcon, uber"));
  CHECK_EQ(fold_ascii("Straße ÆØÅ"), std::string("Strasse AEOA"));
  CHECK_EQ(fold_ascii("“quoted” – it’s…"), std::string("\"quoted\" - it's..."));
  CHECK_EQ(fold_ascii("日本"), std::string("??"));       // no stand-in: one '?' per character
  CHECK_EQ(fold_ascii("a\xC3"), std::string("a?"));       // a sequence cut short
  CHECK_EQ(fold_ascii("\xA9z"), std::string("?z"));       // a stray continuation byte
  CHECK_EQ(fold_ascii("line\nbreak"), std::string("line\nbreak"));
}

TEST("i18n: format fills placeholders in any order and keeps the ones it has no value for") {
  using hg::i18n::format;
  CHECK_EQ(format("{0} to change", {"BOOT"}), std::string("BOOT to change"));
  CHECK_EQ(format("{1}, then {0}", {"a", "b"}), std::string("b, then a"));
  CHECK_EQ(format("{0}{0}", {"x"}), std::string("xx"));
  CHECK_EQ(format("{2} and {0}", {"a"}), std::string("{2} and a"));
  CHECK_EQ(format("no braces { here }", {"a"}), std::string("no braces { here }"));
  CHECK_EQ(format("", {}), std::string());
}

TEST("i18n: control names in words are translated, printed button names are kept") {
  using hg::i18n::control_name;
  CHECK_EQ(control_name(Lang::En, "Swipe down"), std::string("Swipe down"));
  CHECK_EQ(control_name(Lang::Es, "Swipe down"), std::string("Desliza abajo"));
  CHECK_EQ(control_name(Lang::Es, "BOOT"), std::string("BOOT"));
  CHECK_EQ(control_name(Lang::Es, "PWR"), std::string("PWR"));
}

namespace {

struct BlankDisplay : hg::Display {
  hg::DisplayInfo d;
  std::vector<uint16_t> fb;
  BlankDisplay(int w, int h, bool round) : fb(static_cast<size_t>(w * h)) {
    d.width = static_cast<uint16_t>(w);
    d.height = static_cast<uint16_t>(h);
    d.round = round;
  }
  hg::DisplayInfo info() const override { return d; }
  uint16_t* framebuffer() override { return fb.data(); }
  void flush(uint16_t, uint16_t) override {}
};

struct Columns {
  int line = 0;          // bottom-bar hints and hero captions (Ui: hero_cols)
  int name = 0;          // a hero headline in a name panel (Ui: name_cols)
  int headline = 0;      // the header band of a text screen, at the larger scale
  int button = 0;        // one of the Prompt screen's two answer buttons
  int dash_heading = 0;  // a dashboard heading row
  int dash_value = 0;    // a dashboard row's value: at most half the row
};

// What ui.cpp fits on one line of each kind on this panel.
Columns columns(int w, int h, bool round) {
  BlankDisplay display(w, h, round);
  hg::Ui ui(display);
  const hg::UiLayout& l = ui.layout();
  const int s = l.scale;
  const int r = l.header_h / 2 - 2 * s;
  const int tx = 4 * s + r + r + 4 * s;
  Columns c;
  c.line = l.hero_cols;
  c.name = l.name_cols;
  c.headline = (ui.area().width - tx - 2 * s) / (hg::font::kCellWidth * (s + 1));
  const int cell = hg::font::kCellWidth * s, aw = ui.area().width, margin = 4 * s;
  const int bw = (aw - 2 * margin - 4 * s) / 2;
  c.button = (bw - 2 * s) / cell;
  // A long dashboard scrolls and gives up 2s on the right; take that worst case.
  const int right = aw - margin - 2 * s;
  const int dot = std::max(2, hg::font::kGlyphHeight * s / 3);
  c.dash_heading = (right - margin) / cell;
  c.dash_value = (right - (margin + 2 * dot + 3 * s)) / cell / 2;
  return c;
}

// Text with its placeholders filled as the device fills them, the longest way:
// TALK is a button's printed name; CANCEL is a phrase on touch boards.
std::string filled(Lang lang, Msg m) {
  const std::string talk = "BOOT", cancel = hg::i18n::control_name(lang, "Swipe down");
  std::string a = "", b = "";
  switch (m) {
    case Msg::SettingsHintButtons: a = talk, b = "CANCEL"; break;  // only on boards with buttons
    case Msg::ButtonYes: a = talk; break;
    case Msg::ButtonNo: a = "CANCEL"; break;  // a No button needs a CANCEL button
    case Msg::ButtonToClose:
    case Msg::ButtonToCancel:
    case Msg::ButtonToDiscard:
    case Msg::ButtonToStop:
    case Msg::ButtonShowsSessions: a = cancel; break;
    case Msg::ReleaseToSend:
    case Msg::TapButtonToTalk:
    case Msg::HoldButtonToTalk:
    case Msg::ButtonToReply:
    case Msg::HoldButtonWhileSpeaking: a = talk; break;
    case Msg::RetryIn:
    case Msg::Attempt:
    case Msg::NWaiting:
    case Msg::NBusy: a = "30"; break;
    case Msg::NewSessionIn: a = "2"; break;
    case Msg::PairingCode: a = "ABCD1234"; break;
    case Msg::PhoneCode: a = "123456"; break;
    case Msg::ProfileGreeting: a = "Equipo de trabajo"; break;  // a long profile name
    case Msg::TimerLeft: a = "24:00:00"; break;  // the longest timer: 86400 s
    default: break;
  }
  return hg::i18n::format(hg::i18n::text(lang, m), {a, b});
}

// Messages the UI draws on a single line, cutting them with "..".
constexpr Msg kLines[] = {
    // bottom-bar hints
    Msg::SettingsHintTouch, Msg::SettingsHintButtons, Msg::SwipeDownToClose, Msg::ButtonToClose,
    Msg::SwipeDownToCancel, Msg::ButtonToCancel, Msg::KeepPowerOn, Msg::SeeDocs, Msg::ConfigureWifiSerial,
    Msg::RetryIn, Msg::Attempt, Msg::WaitingForApproval, Msg::ButtonToDiscard, Msg::HermesWaiting,
    Msg::ButtonToStop, Msg::HoldToReply, Msg::ButtonToReply, Msg::TapScreenToTalk, Msg::HoldScreenToTalk,
    Msg::TapButtonToTalk, Msg::HoldButtonToTalk, Msg::PairedWithHermes, Msg::ApprovePairingFirst,
    Msg::NotConnected, Msg::HoldScreenWhileSpeaking, Msg::HoldButtonWhileSpeaking, Msg::DidntHearAnything,
    Msg::Cancelled, Msg::NewSessionIn, Msg::NewSession, Msg::MicUnavailable, Msg::AnsweredYes, Msg::AnsweredNo,
    Msg::PhonePaired, Msg::PhonePairingFailed, Msg::PhonePairingClosed, Msg::BluetoothUnavailable,
    Msg::WifiSetupUnavailable, Msg::ProfileListHint, Msg::ProfileCantSwitch, Msg::ProfileRefusedBusy,
    Msg::ProfileRefusedUnknown, Msg::ProfileRefused,
    // hero headlines and one-line captions
    Msg::HiImHermes, Msg::AskMeAnything, Msg::UpShowsLastReply, Msg::SwipeDownForSessions,
    Msg::ButtonShowsSessions, Msg::LowBattery, Msg::Listening, Msg::SpeakNowPause, Msg::LiftFingerToSend,
    Msg::ReleaseToSend, Msg::Thinking, Msg::HermesAsks, Msg::Restarting, Msg::Updating, Msg::SetupNeeded,
    Msg::NoNetwork, Msg::Authenticating, Msg::ConnectingToHermes, Msg::PairingCode, Msg::PairThisDevice,
    Msg::ProfileTapName, Msg::ProfileInSettings,
    Msg::TimerLeft, Msg::TapScreenToStop,
};
// A profile's greeting, in the name panel a touchscreen taps.
constexpr Msg kNameLines[] = {Msg::ProfileGreeting};
// Headlines of text screens: larger letters after the status indicator.
constexpr Msg kHeadlines[] = {
    Msg::Settings, Msg::WifiSetup, Msg::PairAPhone, Msg::PhoneCode, Msg::Speaking, Msg::Ready,
    Msg::Image, Msg::NWaiting, Msg::NBusy, Msg::AllIdle, Msg::ProfileListTitle,
};

// The Prompt screen's answer buttons.
constexpr Msg kButtons[] = {Msg::TapYes, Msg::ButtonYes, Msg::SwipeNo, Msg::ButtonNo};
// Dashboard headings, and the session states shown as a row's value.
constexpr Msg kDashHeadings[] = {Msg::HermesSessions, Msg::NoSessions, Msg::DashStatus};
constexpr Msg kDashValues[] = {Msg::DashApproval, Msg::DashBusy, Msg::DashIdle, Msg::ProfileListCurrent};

struct Panel {
  int w, h;
  bool round;
};
// Every panel a board or simulator profile uses (the AMOLEDs as their UI area).
constexpr Panel kPanels[] = {
    {240, 135, false}, {320, 170, false}, {240, 240, false}, {240, 240, true}, {320, 240, false},
    {360, 360, true},  {368, 432, false}, {410, 486, false}, {466, 466, true}, {480, 320, false},
    {480, 480, false}, {800, 480, false},
    {480, 480, true},
};

void check_fits(Lang lang, Msg m, int cols, const Panel& p) {
  const std::string en = filled(Lang::En, m), t = filled(lang, m);
  const int en_cols = static_cast<int>(hg::font::length(en)), t_cols = static_cast<int>(hg::font::length(t));
  if (en_cols <= cols && t_cols > cols)
    check::fail(__FILE__, __LINE__,
                std::string(hg::i18n::info(lang).code) + " \"" + t + "\" (" + std::to_string(t_cols) + ") is cut at " +
                    std::to_string(cols) + " columns on " + std::to_string(p.w) + "x" + std::to_string(p.h) +
                    (p.round ? " round" : "") + ", where \"" + en + "\" fits");
}

}  // namespace

TEST("i18n: one-line text fits wherever the English fits") {
  for (const Panel& p : kPanels) {
    const Columns c = columns(p.w, p.h, p.round);
    CHECK(c.line > 0 && c.name > 0 && c.headline > 0 && c.button > 0 && c.dash_heading > 0 && c.dash_value > 0);
    for (Lang lang : translations()) {
      for (Msg m : kLines) check_fits(lang, m, c.line, p);
      for (Msg m : kNameLines) check_fits(lang, m, c.name, p);
      for (Msg m : kHeadlines) check_fits(lang, m, c.headline, p);
      for (Msg m : kButtons) check_fits(lang, m, c.button, p);
      for (Msg m : kDashHeadings) check_fits(lang, m, c.dash_heading, p);
      for (Msg m : kDashValues) check_fits(lang, m, c.dash_value, p);
    }
  }
}

// The timer's lines on the 128x128 panel (the AIPI Lite, the smallest board with a speaker), which the
// panel list above does not run every message on.
TEST("i18n: the timer's one-line texts fit the 128x128 panel in every language") {
  const Panel p{128, 128, false};
  const Columns c = columns(p.w, p.h, p.round);
  for (Lang lang : translations()) {
    check_fits(lang, Msg::TimerLeft, c.line, p);
    check_fits(lang, Msg::TapScreenToStop, c.line, p);
  }
}

// ---------------------------------------------------------------------------
// The font's Latin-1 half and UTF-8 text (hg/font.hpp)

TEST("font: every Latin-1 code point has its own glyph") {
  const std::string question(hg::font::glyph('?'));
  std::set<std::string> seen;
  for (char32_t cp = 0xA1; cp <= 0xFF; ++cp) {
    CHECK(hg::font::drawable(cp));
    const std::string g(hg::font::glyph(cp));
    CHECK_EQ(g.size(), size_t(hg::font::kGlyphWidth * hg::font::kGlyphHeight));
    if (cp == 0xAD) continue;  // a soft hyphen draws as a short dash
    if (g == question) check::fail(__FILE__, __LINE__, "U+" + std::to_string(cp) + " draws as '?'");
    if (!seen.insert(g).second) check::fail(__FILE__, __LINE__, "U+" + std::to_string(cp) + " repeats a glyph");
  }
  // An accented letter is not its base letter, and the accents differ from each other.
  CHECK(std::string(hg::font::glyph(U'á')) != hg::font::glyph(U'a'));
  CHECK(std::string(hg::font::glyph(U'á')) != hg::font::glyph(U'à'));
  CHECK(std::string(hg::font::glyph(U'ñ')) != hg::font::glyph(U'n'));
  CHECK(std::string(hg::font::glyph(U'Ñ')) != hg::font::glyph(U'N'));
  // Outside ASCII and Latin-1: the '?' glyph.
  CHECK_EQ(std::string(hg::font::glyph(0x20AC)), question);  // €
  CHECK_EQ(std::string(hg::font::glyph(0x7F)), question);
  CHECK(!hg::font::drawable(0x0101));
}

TEST("font: UTF-8 reads one code point at a time and survives malformed input") {
  const std::string_view s = "añ€😀";
  size_t i = 0;
  CHECK(hg::font::next(s, i) == U'a' && i == 1);
  CHECK(hg::font::next(s, i) == U'ñ' && i == 3);
  CHECK(hg::font::next(s, i) == 0x20AC && i == 6);
  CHECK(hg::font::next(s, i) == 0x1F600 && i == 10);
  CHECK_EQ(hg::font::length(s), size_t(4));
  CHECK_EQ(hg::font::prefix(s, 2), size_t(3));
  CHECK_EQ(hg::font::prefix(s, 9), s.size());
  // Truncated, overlong-lead and stray bytes each read as U+FFFD and advance one byte.
  for (std::string_view bad : {std::string_view("\xC3"), std::string_view("\xE2\x82"), std::string_view("\x80"),
                               std::string_view("\xC0\xAF"), std::string_view("\xFF")}) {
    size_t j = 0;
    CHECK(hg::font::next(bad, j) == 0xFFFD && j == 1);
    CHECK_EQ(hg::font::length(bad), bad.size());
  }
}

TEST("font: widths, wrapping and cuts count letters, not bytes") {
  CHECK_EQ(hg::Canvas::text_width("ñandú", 1), hg::Canvas::text_width("nandu", 1));
  const auto lines = hg::wrap_text("canción para el niño pequeño", 10);
  CHECK_EQ(lines.size(), size_t(4));
  if (lines.size() == 4) {
    CHECK_EQ(lines[0], std::string("canción"));
    CHECK_EQ(lines[1], std::string("para el"));
    CHECK_EQ(lines[2], std::string("niño"));
    CHECK_EQ(lines[3], std::string("pequeño"));  // 7 letters in 9 bytes: fits a 10-column line
  }
  // A word longer than the line splits between letters, never inside one.
  const auto split = hg::wrap_text("ñññññññ", 3);
  CHECK_EQ(split.size(), size_t(3));
  for (const auto& l : split) CHECK(hg::font::length(l) <= 3 && hg::font::prefix(l, 99) == l.size());
}
