#include "hg/app.hpp"

#include <algorithm>
#include <vector>

#include "hg/console_args.hpp"
#include "hg/wifi_networks.hpp"

namespace hg {

bool App::settings_hold_live() const {
  return !prompt_showing() && !ota_busy() && !wifi_setup_open() && !phone_pairing_ && !alarm_;
}

bool App::settings_title_hit(int x, int y) const {
  return ui_ && settings_hold_live() && ui_->title_hit(x, y);
}

bool App::open_settings() {
  // The alarm too: a hold that began just before it rang must not cover it and silence it.
  if (face_down_ || prompt_showing() || ota_busy() || ota_ == Ota::Restarting || !wifi_setup_text_.empty() ||
      phone_pairing_ || alarm_)
    return false;
  wake_display();
  if (settings_open()) { close_settings(); return true; }
  if (mode_ == Mode::Listening) cancel_listening("local settings");
  else if (mode_ == Mode::Thinking || mode_ == Mode::Responding) cancel_turn();
  stop_playback();
  dismiss_overlay();
  menu_ = Menu::Volume;
  power_off_armed_ = false;
  check_result_.clear();
  update_model();
  return true;
}

void App::stop_hardware_check() {
  if (hardware_check_ == HardwareCheck::Microphone && hal_.mic) hal_.mic->stop();
  if (hardware_check_ == HardwareCheck::Speaker && hal_.speaker) hal_.speaker->abort();
  hardware_check_ = HardwareCheck::None;
  level_ = 0;
}

void App::close_settings() {
  stop_hardware_check();
  menu_ = Menu::Closed;
  check_result_.clear();
  if (ui_) ui_->invalidate();
  update_model();
}

void App::settings_input(Button button, bool pressed) {
  if (hardware_check_ == HardwareCheck::Inputs) {
    if (pressed) {
      const char* labels[] = {"TALK", "CANCEL", "UP", "DOWN"};
      check_result_ = tr(i18n::Msg::ButtonPressed, {labels[static_cast<unsigned>(button)]});
      return;
    }
    if (button != Button::Cancel) return;
  }
  if ((button == Button::Cancel && !pressed) ||
      ((button == Button::Up || button == Button::Down) && pressed)) {
    stop_hardware_check();
    check_result_.clear();
    power_off_armed_ = false;
    const bool can_power_off = hal_.power && hal_.power->can_power_off();
    // Steps over the items this board can't offer.
    const auto offered = [&](Menu item) {
      switch (item) {
        case Menu::Profile: return agent_profiles_.size() > 1;
        case Menu::Power: return hal_.power != nullptr;
        case Menu::FaceDownSleep: return can_sleep_face_down();
        case Menu::Clock: return hal_.display != nullptr;
        case Menu::PowerOff: return can_power_off;
        case Menu::WifiSetup: return static_cast<bool>(on_wifi_setup);
        case Menu::Phone: return static_cast<bool>(on_phone_pair);
        default: return true;
      }
    };
    do {
      int item = static_cast<int>(menu_) + (button == Button::Up ? -1 : 1);
      if (item < static_cast<int>(Menu::Volume)) item = static_cast<int>(Menu::Back);
      if (item > static_cast<int>(Menu::Back)) item = static_cast<int>(Menu::Volume);
      menu_ = static_cast<Menu>(item);
    } while (!offered(menu_));
    return;
  }
  if (button != Button::Talk || pressed) return;
  if (hardware_check_ != HardwareCheck::None) {
    stop_hardware_check();
    check_result_ = tr(i18n::Msg::CheckStopped);
    return;
  }
  check_result_.clear();
  switch (menu_) {
    case Menu::Volume:
      if (hal_.speaker) console("set volume " + std::to_string(volume_ >= 100 ? 0 : std::min(100, volume_ + 10)));
      break;
    case Menu::Brightness:
      if (hal_.display && hal_.display->info().has_backlight) {
        const int next = brightness_ < 25 ? 25 : brightness_ < 50 ? 50 : brightness_ < 75 ? 75 : brightness_ < 100 ? 100 : 10;
        console("set brightness " + std::to_string(next));
      }
      break;
    case Menu::TalkMode:
      if (hal_.mic) console(talk_mode_ == TalkMode::Hold ? "set talk_mode tap" : "set talk_mode hold");
      break;
    case Menu::Profile:
      if (!(profile_.touch_screen ? open_profile_picker() : next_agent_profile()))
        check_result_ = tr(i18n::Msg::ProfileCantSwitch);
      break;
    case Menu::Microphone:
      if (hal_.mic && hal_.mic->start(profile_.mic_rate)) hardware_check_ = HardwareCheck::Microphone;
      else check_result_ = tr(i18n::Msg::MicUnavailable);
      break;
    case Menu::Speaker:
      if (hal_.speaker && hal_.speaker->begin(profile_.speaker_rate)) {
        // A quiet quarter-second square tone, entirely local to the device.
        std::vector<int16_t> tone(profile_.speaker_rate / 4);
        const uint32_t half_period = std::max<uint32_t>(1, profile_.speaker_rate / 1000);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = (i / half_period) % 2 ? 900 : -900;
        hal_.speaker->write(tone.data(), tone.size());
        hal_.speaker->end();
        hardware_check_ = HardwareCheck::Speaker;
        check_result_ = tr(i18n::Msg::ListenForTone);
      } else check_result_ = tr(i18n::Msg::SpeakerUnavailable);
      break;
    case Menu::Display:
      if (hal_.display) hardware_check_ = HardwareCheck::Display;
      else check_result_ = tr(i18n::Msg::DisplayUnavailable);
      break;
    case Menu::Inputs:
      hardware_check_ = HardwareCheck::Inputs;
      check_result_ = tr(i18n::Msg::InputCheckRunning);
      break;
    case Menu::IdleTimer: {
      const uint32_t seconds = screen_timeout_ms_ / 1000;
      const uint32_t next = seconds == 0 ? 30 : seconds < 60 ? 60 : seconds < 120 ? 120 : seconds < 300 ? 300 : 0;
      console("set screen_timeout " + std::to_string(next));
      break;
    }
    case Menu::FaceDownSleep:
      console(face_down_sleep_ ? "set face_down_sleep off" : "set face_down_sleep on");
      break;
    case Menu::Clock: {
      // Off, then the same steps as the screen timeout, then Off again.
      const uint32_t seconds = clock_after_ms_ / 1000;
      const uint32_t next = seconds == 0 ? 30 : seconds < 60 ? 60 : seconds < 120 ? 120 : seconds < 300 ? 300 : 0;
      console("set clock " + std::to_string(next));
      break;
    }
    case Menu::PowerOff:
      if (!power_off_armed_) {
        power_off_armed_ = true;
        check_result_ = tr(i18n::Msg::PowerOffConfirm);
      } else {
        power_off_armed_ = false;
        check_result_ = tr(hal_.power->power_off() ? i18n::Msg::PowerOffRequested : i18n::Msg::PowerOffFailed);
      }
      break;
    case Menu::Language: {
      // English, each translation, then auto (follow Hermes), then English again.
      const bool last = static_cast<size_t>(lang_) + 1 >= i18n::kLangCount;
      console(std::string("set language ") +
              (lang_auto_ ? "en" : last ? "auto" : i18n::info(i18n::next(lang_)).code));
      break;
    }
    case Menu::Back: close_settings(); break;
    case Menu::WifiSetup: start_wifi_setup(); break;
    case Menu::Phone: start_phone_pairing(); break;
    case Menu::Power:
      if (battery_readable()) {
        static const char* const kNext[] = {"percentage", "both", "off", "icon"};  // after Icon, Percentage, Both, Off
        console(std::string("set battery_display ") + kNext[static_cast<int>(battery_display_)]);
      }
      break;
    case Menu::Info:
    case Menu::Closed: break;
  }
}

void App::settings_tick() {
  if (talk_held_ && cancel_held_ && !settings_chord_fired_ &&
      now() - talk_down_at_ >= 1000 && now() - cancel_down_at_ >= 1000) {
    if (open_settings()) {
      settings_chord_fired_ = true;
      cancel_long_fired_ = true;
    }
  }
  if (hardware_check_ == HardwareCheck::Speaker && !hal_.speaker->busy()) {
    hardware_check_ = HardwareCheck::None;
    check_result_ = tr(i18n::Msg::ToneFinished);
    update_model();
  }
}

bool App::battery_readable() const {
  return display_battery_percent().has_value();
}

void App::settings_model() {
  using i18n::Msg;
  UiModel& m = model_;
  m.screen = Screen::Settings;
  m.headline = tr(Msg::Settings);
  m.scroll = 0;
  m.speaking = false;
  const std::string talk = control(profile_.talk_label);
  m.hint = profile_.touch_screen ? std::string(tr(Msg::SettingsHintTouch))
                                : tr(Msg::SettingsHintButtons, {talk, control(profile_.cancel_label)});
  switch (menu_) {
    case Menu::Volume:
      m.detail = tr(Msg::SpeakerVolume);
      m.body = hal_.speaker ? std::to_string(volume_) + "%\n" + tr(Msg::ChangesSaved) : std::string(tr(Msg::NoSpeakerDriver));
      break;
    case Menu::Brightness:
      m.detail = tr(Msg::ScreenBrightness);
      m.body = hal_.display && hal_.display->info().has_backlight ? std::to_string(brightness_) + "%\n" + tr(Msg::ChangesSaved)
                                                               : std::string(tr(Msg::BrightnessUnavailable));
      break;
    case Menu::TalkMode:
      m.detail = tr(Msg::TalkMode);
      m.body = tr(!hal_.mic ? Msg::NoMicDriver : talk_mode_ == TalkMode::Hold ? Msg::TalkModeHold : Msg::TalkModeTap);
      break;
    case Menu::Profile:
      m.detail = tr(Msg::ProfileSetting);
      m.body = tr(profile_.touch_screen ? Msg::ProfileSettingChoose : Msg::ProfileSettingNext, {agent_profile_name()});
      break;
    case Menu::Microphone:
      m.detail = tr(Msg::MicCheck);
      m.body = hardware_check_ == HardwareCheck::Microphone ? tr(Msg::MicCheckLive, {std::to_string(level_)})
                                                          : std::string(tr(Msg::MicCheckIntro));
      break;
    case Menu::Speaker:
      m.detail = tr(Msg::SpeakerCheck);
      m.body = tr(Msg::SpeakerCheckIntro);
      break;
    case Menu::Display:
      m.detail = tr(Msg::DisplayCheck);
      m.body = tr(Msg::DisplayCheckIntro);
      m.color_test = hardware_check_ == HardwareCheck::Display;
      break;
    case Menu::Inputs:
      m.detail = tr(Msg::InputCheck);
      m.body = tr(Msg::InputCheckIntro);
      break;
    case Menu::Info:
      m.detail = tr(Msg::DeviceInfo);
      m.body = tr(Msg::DeviceInfoBody, {profile_.board, profile_.firmware, device_id_,
                                        tr(hal_.mic ? Msg::Available : Msg::Unavailable),
                                        tr(hal_.speaker ? Msg::Available : Msg::Unavailable)});
      break;
    case Menu::Power:
      m.detail = tr(Msg::BatteryAndPower);
      if (!power_status_) m.body = tr(Msg::PowerUnavailable);
      else {
        const auto& p = *power_status_;
        if (p.battery_present && !*p.battery_present) m.body = std::string(tr(Msg::NoBattery)) + "\n";
        if (const auto percent = display_battery_percent()) {
          const bool on_charger = p.charging == true || p.external_power == true;
          const Msg kind = !battery_is_estimated() ? Msg::PercentGauge
                           : on_charger            ? Msg::PercentVoltageCharger
                                                   : Msg::PercentVoltage;
          m.body += tr(kind, {std::to_string(*percent)}) + "\n";
        }
        if (p.battery_mv) m.body += std::to_string(*p.battery_mv) + " mV\n";
        if (p.charging) m.body += std::string(tr(*p.charging ? Msg::Charging : Msg::NotCharging)) + "\n";
        if (p.external_power) m.body += std::string(tr(*p.external_power ? Msg::UsbPower : Msg::NoUsbPower)) + "\n";
        if (p.battery_percent && *p.battery_percent <= 10 && p.external_power == false)
          m.body += std::string(tr(Msg::LowBatteryConnectUsb)) + "\n";
        if (!battery_estimate_enabled_ && !p.battery_percent && p.battery_present != false &&
            p.battery_mv && *p.battery_mv >= 2000 && *p.battery_mv <= 4500)
          m.body += "set battery_estimate liion\n" + std::string(tr(Msg::BatteryMustBeFitted)) + "\n";
        else if (battery_is_estimated())
          m.body += std::string(tr(Msg::BatteryMustBeFitted)) + "\n";
      }
    if (battery_readable()) {  // nothing to choose when no percentage can appear
      static constexpr Msg kShown[] = {Msg::IndicatorIcon, Msg::IndicatorPercent, Msg::IndicatorBoth, Msg::IndicatorOff};
      m.body += tr(Msg::Indicator, {tr(kShown[static_cast<int>(battery_display_)])}) + "\n" +
                (profile_.touch_screen ? std::string(tr(Msg::TapToChange)) : tr(Msg::ButtonToChange, {talk}));
    }
      break;
    case Menu::IdleTimer:
      m.detail = tr(Msg::ScreenTimeout);
      m.body = screen_timeout_ms_ ? tr(Msg::TimeoutSeconds, {std::to_string(screen_timeout_ms_ / 1000)})
                                 : std::string(tr(Msg::TimeoutAlwaysOn));
      break;
    case Menu::FaceDownSleep:
      m.detail = tr(Msg::FaceDownSleep);
      m.body = tr(face_down_sleep_ ? Msg::FaceDownSleepOn : Msg::FaceDownSleepOff);
      break;
    case Menu::Clock:
      m.detail = tr(Msg::ClockFace);
      // The body says when the face is off, or why it has not appeared yet.
      if (!clock_after_ms_) m.body = tr(Msg::ClockOff);
      else m.body = tr(wall_valid_ ? Msg::ClockAfterSeconds : Msg::ClockNoTime, {std::to_string(clock_after_ms_ / 1000)});
      break;
    case Menu::PowerOff:
      m.detail = tr(Msg::PowerOff);
      m.body = tr(Msg::PowerOffIntro);
      break;
    case Menu::WifiSetup:
      m.detail = tr(Msg::WifiSetup);
      m.body = tr(Msg::WifiSetupIntro);
      break;
    case Menu::Phone:
      m.detail = tr(Msg::PhoneApp);
      m.body = phones_paired_ == 0 ? std::string(tr(Msg::NoPhonePaired))
                                   : std::string(tr(phone_connected_ ? Msg::PhoneConnected : Msg::PhonePairedOffline)) +
                                         "\n" + tr(Msg::SelectToPairAnother);
      break;
    case Menu::Language: {
      // The English word stays beside the translation: someone who picked a
      // language they can't read can still find the way back.
      m.detail = lang_ == i18n::Lang::En ? std::string(tr(Msg::Language))
                                         : std::string(tr(Msg::Language)) + " / " + i18n::english(Msg::Language);
      const std::string name = i18n::info(lang_).name;  // the font draws Latin-1 (#172)
      m.body = tr(Msg::LanguageBody, {lang_auto_ ? tr(Msg::LanguageAuto, {name}) : name});
      break;
    }
    case Menu::Back:
      m.detail = tr(Msg::BackToHermes);
      m.body = tr(Msg::SelectToClose);
      break;
    case Menu::Closed: break;
  }
  if (!check_result_.empty()) m.body += "\n" + check_result_;
}

bool App::start_wifi_setup() {
  if (!on_wifi_setup || prompt_showing() || ota_busy() || ota_ == Ota::Restarting) return false;
  if (mode_ == Mode::Listening) cancel_listening("Wi-Fi setup");
  else if (mode_ != Mode::Idle) cancel_turn();
  stop_playback();
  close_settings();
  close_phone_pairing();
  wake_display();
  talk_held_ = false;
  cancel_held_ = false;
  settings_chord_fired_ = false;
  wake_buttons_ = 0;
  wifi_setup_text_ = on_wifi_setup();
  // Encode the code once here: the model is rebuilt on every frame.
  wifi_setup_qr_ = {};
  if (on_wifi_setup_ap) {
    const WifiSetupAp ap = on_wifi_setup_ap();
    if (!ap.ssid.empty()) wifi_setup_qr_ = qr::encode(qr::payload(ap.ssid, ap.password));
  }
  if (wifi_setup_text_.empty()) set_hint_flash(tr(i18n::Msg::WifiSetupUnavailable));
  update_model();
  return !wifi_setup_text_.empty();
}

void App::close_wifi_setup() {
  if (wifi_setup_text_.empty()) return;
  wifi_setup_text_.clear();
  wifi_setup_qr_ = {};
  if (on_wifi_setup_close) on_wifi_setup_close();
  update_model();
}

// `wifi [list | add <ssid> [password] | join <ssid> | forget <ssid>]`. Arguments
// with spaces or quotes go in double quotes (hg/console_args.hpp).
std::string App::wifi_command(std::string_view line) {
  static const char* const kUsage = "@error usage: wifi [list | add <ssid> [password] | join <ssid> | forget <ssid>]";
  if (!hal_.storage) return "@error no storage";
  const std::vector<std::string> args = split_args(line);
  const std::string sub = args.empty() ? "list" : args[0];
  WifiNetworks networks = WifiNetworks::load(*hal_.storage);
  if (sub == "list") {
    if (args.size() > 1) return kUsage;
    json::Value list = json::Value::array();
    for (const auto& w : networks.all()) {
      json::Value e = json::Value::object();
      e.set("ssid", w.ssid).set("tested", w.tested).set("open", w.password.empty());
      list.push(std::move(e));
    }
    json::Value out = json::Value::object();
    out.set("networks", list).set("max", static_cast<unsigned>(WifiNetworks::kMax));
    if (current_wifi) {
      const std::string current = current_wifi();
      if (!current.empty()) out.set("current", current);
    }
    return "@wifi " + out.dump();
  }
  if (args.size() < 2 || args.size() > (sub == "add" ? 3u : 2u)) return kUsage;
  const std::string& ssid = args[1];
  if (sub == "add") {
    const std::string password = args.size() > 2 ? args[2] : std::string();
    const std::string error = wifi_network_error(ssid, password);
    if (!error.empty()) return "@error " + error;
    if (networks.add(ssid, password) == WifiNetworks::Added::Full)
      return "@error " + std::to_string(WifiNetworks::kMax) + " networks saved; forget one first";
  } else if (sub == "join" || sub == "forget") {
    if (networks.find(ssid) == networks.size()) return "@error no saved network " + ssid;
    if (sub == "join") networks.move_first(ssid);
    else networks.forget(ssid);
  } else {
    return kUsage;
  }
  if (!networks.save(*hal_.storage)) return "@error could not save the network list";
  if (on_setting_changed) on_setting_changed("wifi_nets");
  return "@ok wifi " + sub;
}

}  // namespace hg
