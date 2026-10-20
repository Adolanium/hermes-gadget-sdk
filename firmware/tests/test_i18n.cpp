// The translation catalogue (hg/i18n.hpp): lookups, placeholders, folding to
// the font, and that translated one-line text fits wherever the English does.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
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

TEST("i18n: drawn text is plain ASCII, the only characters the font has") {
  for (size_t l = 0; l < hg::i18n::kLangCount; ++l) {
    for (Msg m : all_messages()) {
      for (const char* p = hg::i18n::text(static_cast<Lang>(l), m); *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c != '\n' && (c < 0x20 || c > 0x7E)) {
          check::fail(__FILE__, __LINE__, std::string("non-ASCII in \"") + hg::i18n::text(static_cast<Lang>(l), m) + "\"");
          break;
        }
      }
    }
  }
  // Written with its accents, read without them.
  CHECK_EQ(std::string(hg::i18n::translation(Lang::Es, Msg::DeviceInfo)), std::string("Información"));
  CHECK_EQ(std::string(hg::i18n::text(Lang::Es, Msg::DeviceInfo)), std::string("Informacion"));
}

TEST("i18n: folding keeps ASCII and finds the nearest letter for the rest") {
  using hg::i18n::fold_ascii;
  CHECK_EQ(fold_ascii("Ready"), std::string("Ready"));
  CHECK_EQ(fold_ascii("¿Cómo estás? ¡Sí! Añadir"), std::string("?Como estas? !Si! Anadir"));
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
    Msg::WifiSetupUnavailable,
    // hero headlines and one-line captions
    Msg::HiImHermes, Msg::AskMeAnything, Msg::UpShowsLastReply, Msg::SwipeDownForSessions,
    Msg::ButtonShowsSessions, Msg::LowBattery, Msg::Listening, Msg::SpeakNowPause, Msg::LiftFingerToSend,
    Msg::ReleaseToSend, Msg::Thinking, Msg::HermesAsks, Msg::Restarting, Msg::Updating, Msg::SetupNeeded,
    Msg::NoNetwork, Msg::Authenticating, Msg::ConnectingToHermes, Msg::PairingCode, Msg::PairThisDevice,
};
// Headlines of text screens: larger letters after the status indicator.
constexpr Msg kHeadlines[] = {
    Msg::Settings, Msg::WifiSetup, Msg::PairAPhone, Msg::PhoneCode, Msg::Speaking, Msg::Ready,
    Msg::Image, Msg::NWaiting, Msg::NBusy, Msg::AllIdle,
};

// The Prompt screen's answer buttons.
constexpr Msg kButtons[] = {Msg::TapYes, Msg::ButtonYes, Msg::SwipeNo, Msg::ButtonNo};
// Dashboard headings, and the session states shown as a row's value.
constexpr Msg kDashHeadings[] = {Msg::HermesSessions, Msg::NoSessions, Msg::DashStatus};
constexpr Msg kDashValues[] = {Msg::DashApproval, Msg::DashBusy, Msg::DashIdle};

struct Panel {
  int w, h;
  bool round;
};
// Every panel a board or simulator profile uses (the AMOLEDs as their UI area).
constexpr Panel kPanels[] = {
    {240, 135, false}, {320, 170, false}, {240, 240, false}, {240, 240, true}, {320, 240, false},
    {360, 360, true},  {368, 432, false}, {410, 486, false}, {466, 466, true}, {480, 320, false},
    {480, 480, false}, {800, 480, false},
};

void check_fits(Lang lang, Msg m, int cols, const Panel& p) {
  const std::string en = filled(Lang::En, m), t = filled(lang, m);
  if (static_cast<int>(en.size()) <= cols && static_cast<int>(t.size()) > cols)
    check::fail(__FILE__, __LINE__,
                std::string(hg::i18n::info(lang).code) + " \"" + t + "\" (" + std::to_string(t.size()) + ") is cut at " +
                    std::to_string(cols) + " columns on " + std::to_string(p.w) + "x" + std::to_string(p.h) +
                    (p.round ? " round" : "") + ", where \"" + en + "\" fits");
}

}  // namespace

TEST("i18n: one-line text fits wherever the English fits") {
  for (const Panel& p : kPanels) {
    const Columns c = columns(p.w, p.h, p.round);
    CHECK(c.line > 0 && c.headline > 0 && c.button > 0 && c.dash_heading > 0 && c.dash_value > 0);
    for (Lang lang : translations()) {
      for (Msg m : kLines) check_fits(lang, m, c.line, p);
      for (Msg m : kHeadlines) check_fits(lang, m, c.headline, p);
      for (Msg m : kButtons) check_fits(lang, m, c.button, p);
      for (Msg m : kDashHeadings) check_fits(lang, m, c.dash_heading, p);
      for (Msg m : kDashValues) check_fits(lang, m, c.dash_value, p);
    }
  }
}
