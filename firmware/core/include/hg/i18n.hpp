// The device's own text in each language it speaks.
//
// Every string the device shows by itself is listed once below, with its
// English text. A translation is a table in src/i18n_<code>.cpp that maps
// message ids to text; a message it leaves out is shown in English. Text with
// {0}, {1}... is filled in by format(), so a translation can put the pieces in
// its own order. Translations are written in UTF-8 with their accents; until
// text() keeps what the font draws (ASCII and Latin-1) and folds the rest (fold_to_font).
//
// Replies, cards and questions come from Hermes in the user's language and are
// not translated here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace hg::i18n {

// clang-format off
#define HG_I18N_MESSAGES(X) \
  /* Settings menu */ \
  X(Settings,               "Settings") \
  X(SettingsHintTouch,      "Tap: change | Swipe: next") \
  X(SettingsHintButtons,    "{0}: change | {1}: next") \
  X(SpeakerVolume,          "Speaker volume") \
  X(ChangesSaved,           "Changes are saved.") \
  X(NoSpeakerDriver,        "No speaker driver is active.") \
  X(ScreenBrightness,       "Screen brightness") \
  X(BrightnessUnavailable,  "Brightness control is unavailable.") \
  X(TalkMode,               "Talk mode") \
  X(NoMicDriver,            "No microphone driver is active.") \
  X(TalkModeHold,           "Hold to record; release to send.") \
  X(TalkModeTap,            "Tap to record; pause or tap to send.") \
  X(MicCheck,               "Microphone check") \
  X(MicCheckLive,           "Speak now. Level: {0}%\nLocal only. Nothing is sent.") \
  X(MicCheckIntro,          "Start to check the microphone level. No recording is saved or sent.") \
  X(SpeakerCheck,           "Speaker check") \
  X(SpeakerCheckIntro,      "Start to play a short tone at the current volume.") \
  X(DisplayCheck,           "Display check") \
  X(DisplayCheckIntro,      "Start to show red, green, blue, white and black bands.") \
  X(InputCheck,             "Input check") \
  X(InputCheckIntro,        "Start to check the controls. Cancel leaves this check.") \
  X(DeviceInfo,             "Device information") \
  X(DeviceInfoBody,         "{0}\nFirmware {1}\n{2}\nMicrophone: {3}\nSpeaker: {4}") \
  X(Available,              "available") \
  X(Unavailable,            "unavailable") \
  X(BatteryAndPower,        "Battery and power") \
  X(PowerUnavailable,       "Power readings unavailable.") \
  X(NoBattery,              "No battery detected.") \
  X(PercentVoltage,         "{0}% (voltage estimate)") \
  X(PercentVoltageCharger,  "{0}% (voltage estimate, high on a charger)") \
  X(PercentGauge,           "{0}% (gauge estimate)") \
  X(Charging,               "Charging") \
  X(NotCharging,            "Not charging") \
  X(UsbPower,               "USB power") \
  X(NoUsbPower,             "No USB power") \
  X(LowBatteryConnectUsb,   "Low battery: connect USB.") \
  X(BatteryMustBeFitted,    "Battery must be fitted; charging affects estimate.") \
  X(Indicator,              "Indicator: {0}") \
  X(IndicatorIcon,          "Icon") \
  X(IndicatorPercent,       "Percent") \
  X(IndicatorBoth,          "Both") \
  X(IndicatorOff,           "Off") \
  X(TapToChange,            "Tap to change") \
  X(ButtonToChange,         "{0} to change") \
  X(ScreenTimeout,          "Screen timeout") \
  X(TimeoutSeconds,         "{0} seconds\nDims halfway; first input wakes.") \
  X(TimeoutAlwaysOn,        "Always on\nSelect to enable automatic dimming and screen sleep.") \
  X(FaceDownSleep,          "Face-down sleep") \
  X(FaceDownSleepOn,        "On\nFace down, the screen sleeps and Hermes stays quiet.") \
  X(FaceDownSleepOff,       "Off\nSelect to sleep when the gadget lies face down.") \
  X(ClockFace,              "Clock face") \
  X(ClockAfterSeconds,      "{0} seconds\nA dim clock appears after this much rest.") \
  X(ClockNoTime,            "{0} seconds\nThe clock starts once the gadget knows the time.") \
  X(ClockOff,               "Off\nSelect to show a clock after a rest.") \
  X(PowerOff,               "Power off") \
  X(PowerOffIntro,          "Shut down the board. Use its PWR key to turn it on again.") \
  X(WifiSetup,              "Wi-Fi setup") \
  X(WifiSetupIntro,         "Start a temporary network to configure this device with your phone.") \
  X(PhoneApp,               "Phone app") \
  X(NoPhonePaired,          "No phone paired.\nSelect to pair one over Bluetooth.") \
  X(PhoneConnected,         "Phone connected.") \
  X(PhonePairedOffline,     "Phone paired, not connected.") \
  X(SelectToPairAnother,    "Select to pair another.") \
  X(Language,               "Language") \
  X(LanguageBody,           "{0}\nSelect for the next language.") \
  X(LanguageAuto,           "Automatic, as Hermes: {0}") \
  X(BackToHermes,           "Back to Hermes") \
  X(SelectToClose,          "Select to close settings.") \
  X(CheckStopped,           "Check stopped") \
  X(MicUnavailable,         "Microphone unavailable") \
  X(ListenForTone,          "Listen for a short tone") \
  X(SpeakerUnavailable,     "Speaker unavailable") \
  X(DisplayUnavailable,     "Display unavailable") \
  X(InputCheckRunning,      "Press a button. Release Cancel to leave.") \
  X(ButtonPressed,          "{0} pressed") \
  X(PowerOffConfirm,        "Select again to power off. Cancel goes back.") \
  X(PowerOffRequested,      "Power-off requested") \
  X(PowerOffFailed,         "Power-off failed. Try the physical PWR key.") \
  X(ToneFinished,           "Tone finished. Did you hear it?") \
  X(WifiSetupUnavailable,   "Wi-Fi setup unavailable; use USB") \
  /* Wi-Fi setup and phone pairing screens */ \
  X(ConnectYourPhone,       "Connect your phone") \
  X(SwipeDownToClose,       "Swipe down to close") \
  X(ButtonToClose,          "{0} to close") \
  X(PairAPhone,             "Pair a phone") \
  X(BluetoothName,          "Bluetooth: {0}") \
  X(PhonePairIntro,         "On your phone, open the Hermes app and choose {0}. This device then shows a code to type on the phone.") \
  X(PhoneCode,              "Code {0}") \
  X(TypeCodeOnPhone,        "Type this code on your phone") \
  X(OnlyPhoneInFront,       "Only a phone in front of you should ask for it.") \
  X(SwipeDownToCancel,      "Swipe down to cancel") \
  X(ButtonToCancel,         "{0} to cancel") \
  X(PhonePaired,            "Phone paired") \
  X(PhonePairingFailed,     "Phone pairing failed") \
  X(PhonePairingClosed,     "Phone pairing closed") \
  X(BluetoothUnavailable,   "Bluetooth unavailable") \
  /* Start-up, updates and errors */ \
  X(StartingFirmware,       "starting - fw {0}") \
  X(NewFirmware,            "new firmware") \
  X(FirmwareVersion,        "firmware {0}") \
  X(Restarting,             "Restarting") \
  X(Installed,              "Installed {0}") \
  X(Updating,               "Updating") \
  X(UpdateProgress,         "{0}: {1}% of {2} KB") \
  X(KeepPowerOn,            "keep the power on") \
  X(UpdateFailed,           "Update failed: {0}") \
  X(SetupNeeded,            "Setup needed") \
  X(NoServerConfigured,     "No Hermes server configured") \
  X(SerialSetServer,        "Serial console: set server ws://<host>:8765/gadget") \
  X(ServerUrl,              "Server: {0}") \
  X(SeeDocs,                "see docs/getting-started.md") \
  X(NoNetwork,              "No network") \
  X(WaitingForWifi,         "Waiting for Wi-Fi") \
  X(ConfigureWifiSerial,    "configure Wi-Fi via serial") \
  X(Authenticating,         "Authenticating") \
  X(ConnectingToHermes,     "Connecting to Hermes") \
  X(ConnectError,           "Error: {0}") \
  X(RetryIn,                "retry in {0}s") \
  X(Attempt,                "attempt {0}") \
  /* Pairing with Hermes */ \
  X(PairingCode,            "Pairing code {0}") \
  X(RunOnHost,              "On the Hermes host, run: {0}") \
  X(PairThisDevice,         "Pair this device") \
  X(AskingForCode,          "Asking Hermes for a pairing code...") \
  X(WaitingForApproval,     "waiting for approval") \
  X(PairedWithHermes,       "Paired with Hermes") \
  X(ApprovePairingFirst,    "Approve pairing first") \
  /* Talking */ \
  X(Listening,              "Listening") \
  X(SpeakNowPause,          "Speak now - pause to send") \
  X(LiftFingerToSend,       "Lift your finger to send") \
  X(ReleaseToSend,          "Release {0} to send") \
  X(ButtonToDiscard,        "{0} to discard") \
  X(HoldScreenWhileSpeaking, "Hold the screen while speaking") \
  X(HoldButtonWhileSpeaking, "Hold {0} while speaking") \
  X(DidntHearAnything,      "Didn't hear anything") \
  X(Cancelled,              "Cancelled") \
  X(Sending,                "Sending...") \
  X(CouldntSendRecording,   "Couldn't send the recording") \
  X(NotConnected,           "Not connected to Hermes") \
  X(NewSessionIn,           "New session in {0}s") \
  X(NewSession,             "New session") \
  X(StartingNewSession,     "Starting a new session") \
  X(Thinking,               "Thinking") \
  X(ButtonToStop,           "{0} to stop") \
  X(Speaking,               "Speaking") \
  X(HoldToReply,            "Hold to reply") \
  X(ButtonToReply,          "{0} to reply") \
  X(NoReplyFromHermes,      "No reply from Hermes") \
  X(RequestFailed,          "Hermes could not complete that request") \
  /* Questions, cards and images */ \
  X(HermesAsks,             "Hermes asks") \
  X(TapYes,                 "Tap: Yes") \
  X(ButtonYes,              "{0}: Yes") \
  X(SwipeNo,                "Swipe: No") \
  X(ButtonNo,               "{0}: No") \
  X(HermesWaiting,          "Hermes is waiting for you") \
  X(AnsweredYes,            "Answered: yes") \
  X(AnsweredNo,             "Answered: no") \
  X(Image,                  "Image") \
  /* Ready screen */ \
  X(Ready,                  "Ready") \
  X(HiImHermes,             "Hi, I'm Hermes") \
  X(AskMeAnything,          "Ask me anything") \
  X(UpShowsLastReply,       "UP shows my last reply") \
  X(TapScreenToTalk,        "Tap the screen to talk") \
  X(HoldScreenToTalk,       "Hold the screen to talk") \
  X(TapButtonToTalk,        "tap {0} to talk") \
  X(HoldButtonToTalk,       "hold {0} to talk") \
  X(SwipeDownForSessions,   "Swipe down for sessions") \
  X(ButtonShowsSessions,    "{0} shows sessions") \
  X(LowBattery,             "Low battery: connect USB") \
  /* Timers */ \
  X(TimerLeft,              "Timer {0}") \
  X(TimerDone,              "TIMER DONE") \
  X(TapScreenToStop,        "Tap the screen to stop") \
  /* The resting clock's date: weekday, month and day, each name at most 3 letters */ \
  X(ClockDate,              "{0} {1} {2}") \
  X(WeekdaySun,             "SUN") \
  X(WeekdayMon,             "MON") \
  X(WeekdayTue,             "TUE") \
  X(WeekdayWed,             "WED") \
  X(WeekdayThu,             "THU") \
  X(WeekdayFri,             "FRI") \
  X(WeekdaySat,             "SAT") \
  X(MonthJan,               "JAN") \
  X(MonthFeb,               "FEB") \
  X(MonthMar,               "MAR") \
  X(MonthApr,               "APR") \
  X(MonthMay,               "MAY") \
  X(MonthJun,               "JUN") \
  X(MonthJul,               "JUL") \
  X(MonthAug,               "AUG") \
  X(MonthSep,               "SEP") \
  X(MonthOct,               "OCT") \
  X(MonthNov,               "NOV") \
  X(MonthDec,               "DEC") \
  /* Hermes profiles */ \
  X(ProfileSetting,         "Hermes profile") \
  X(ProfileSettingNext,     "{0}\nSelect to switch to the next profile.") \
  X(ProfileSettingChoose,   "{0}\nSelect to choose a profile.") \
  X(ProfileCantSwitch,      "Can't switch now") \
  X(ProfileGreeting,        "Hi, I'm {0}") \
  X(ProfileTapName,         "Tap name to change") \
  X(ProfileInSettings,      "Change in Settings") \
  X(ProfileListTitle,       "Talk to") \
  X(ProfileListHint,        "Swipe down to go back") \
  X(ProfileListCurrent,     "NOW") \
  X(ProfileListMore,        "More") \
  X(ProfileSwitching,       "Switching to {0}...") \
  X(ProfileSwitched,        "Now talking to {0}") \
  X(ProfileRefusedBusy,     "Finish this turn first") \
  X(ProfileRefusedUnknown,  "Profile unavailable") \
  X(ProfileRefused,         "Switch not allowed") \
  /* Dashboard */ \
  X(HermesSessions,         "Hermes sessions") \
  X(NoSessions,             "No sessions") \
  X(DashStatus,             "Status") \
  X(NWaiting,               "{0} waiting") \
  X(NBusy,                  "{0} busy") \
  X(AllIdle,                "All idle") \
  X(DashApproval,           "approval") \
  X(DashBusy,               "busy") \
  X(DashIdle,               "idle") \
  /* Top bar */ \
  X(LinkOffline,            "OFFLINE") \
  X(LinkNoNet,              "NO NET") \
  X(LinkNet,                "NET") \
  X(LinkConnecting,         "LINK") \
  X(LinkOnline,             "ONLINE") \
  X(SettingsTarget,         "SETTINGS") \
  X(BackToHermesTarget,     "BACK TO HERMES") \
  /* Control names a board describes in words (DeviceProfile talk_label, cancel_label) */ \
  X(LabelSwipeDown,         "Swipe down") \
  X(LabelHoldScreen,        "Hold screen") \
  X(LabelScreen,            "Screen") \
  X(LabelKnob,              "Knob")
// clang-format on

enum class Msg : uint16_t {
#define HG_I18N_ENUM(id, english) id,
  HG_I18N_MESSAGES(HG_I18N_ENUM)
#undef HG_I18N_ENUM
  Count
};
constexpr size_t kMessageCount = static_cast<size_t>(Msg::Count);

enum class Lang : uint8_t { En, Es, Count };
constexpr size_t kLangCount = static_cast<size_t>(Lang::Count);

// One entry of a translation table.
struct Entry {
  Msg id;
  const char* text;
};

struct LangInfo {
  const char* code;  // the `language` setting's value
  const char* name;  // in the language itself, as the settings menu shows it
};
const LangInfo& info(Lang lang);
std::optional<Lang> parse(std::string_view code);
// The language after `lang`, wrapping: the settings menu steps through them.
Lang next(Lang lang);

// The message in `lang`, as the font draws it (fold_to_font); English where the
// translation leaves it out. Valid for the life of the program.
const char* text(Lang lang, Msg id);
// The English text, as listed above.
const char* english(Msg id);
// The translation as written, or nullptr where `lang` has none (tests and tools).
const char* translation(Lang lang, Msg id);
// `text` with {0}, {1}... replaced by `args`. A placeholder with no argument is kept.
std::string format(std::string_view text, std::initializer_list<std::string_view> args);
// The nearest ASCII for UTF-8 text: accents dropped, ¿ and ¡ left out,
// typographic punctuation replaced; anything else becomes '?'.
std::string fold_ascii(std::string_view utf8);
// UTF-8 text the font can draw: ASCII and Latin-1 kept as they are,
// typographic punctuation replaced, anything else '?'.
std::string fold_to_font(std::string_view utf8);

// A board's control name in `lang`: the words above are translated, a button's
// printed name (BOOT, PWR, TALK, ...) is kept.
std::string control_name(Lang lang, std::string_view label);

}  // namespace hg::i18n
