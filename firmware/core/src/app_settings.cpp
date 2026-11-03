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

// Steps over the items this board can't offer.
bool App::menu_offered(Menu item) const {
  switch (item) {
    case Menu::Profile: return agent_profiles_.size() > 1;
    case Menu::ProfileGrid: return profile_.touch_screen && agent_profiles_.size() > 1 && hal_.display != nullptr;
    case Menu::Power: return hal_.power != nullptr;
    case Menu::BatteryAlert: return hal_.power != nullptr && hal_.display != nullptr;
    case Menu::BatteryAlertSound: return hal_.power != nullptr && hal_.display != nullptr && hal_.speaker != nullptr;
    case Menu::ChargingNotice: return hal_.power != nullptr && hal_.display != nullptr;
    case Menu::ChargingSound: return hal_.power != nullptr && hal_.speaker != nullptr;
    case Menu::ChargedNotice: return hal_.power != nullptr && hal_.display != nullptr;
    case Menu::ChargedSound: return hal_.power != nullptr && hal_.speaker != nullptr;
    case Menu::WifiDeepSleep: return hal_.power != nullptr && static_cast<bool>(current_wifi);
    case Menu::TouchTalk: return profile_.touch_screen && profile_.talk_key;
    case Menu::TouchWake: return profile_.touch_screen && profile_.wake_key && hal_.display != nullptr;
    case Menu::DoubleHome: return profile_.talk_key;
    case Menu::WakeTalk: return profile_.talk_key && hal_.mic != nullptr && hal_.display != nullptr;
    case Menu::FaceDownSleep: return can_sleep_face_down();
    case Menu::Clock: return hal_.display != nullptr;
    case Menu::PowerOff: return hal_.power && hal_.power->can_power_off();
    case Menu::WifiSetup: return static_cast<bool>(on_wifi_setup);
    case Menu::Phone: return static_cast<bool>(on_phone_pair);
    case Menu::Closed: return false;
    default: return true;
  }
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
  // The next visit starts at the top.
  settings_scroll_ = settings_target_ = 0;
  settings_follow_ = true;
  sliding_ = false;
  slide_level_ = -1;
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
    settings_follow_ = true;
    do {
      int item = static_cast<int>(menu_) + (button == Button::Up ? -1 : 1);
      if (item < static_cast<int>(Menu::Volume)) item = static_cast<int>(Menu::Back);
      if (item > static_cast<int>(Menu::Back)) item = static_cast<int>(Menu::Volume);
      menu_ = static_cast<Menu>(item);
    } while (!menu_offered(menu_));
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
    case Menu::TouchTalk:
      console(touch_talk_ ? "set touch_talk off" : "set touch_talk on");
      break;
    case Menu::Profile:
      if (!(profile_.touch_screen ? open_profile_picker() : next_agent_profile()))
        check_result_ = tr(i18n::Msg::ProfileCantSwitch);
      break;
    case Menu::ProfileGrid:
      console("set profile_grid " + std::to_string(profile_grid_ == 4 ? 6 : profile_grid_ == 6 ? 9 : profile_grid_ == 9 ? 16 : 4));
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
    case Menu::BatteryAlert: {
      // The presets in turn, then Off again; another list set from the console goes to Off.
      static const char* const kPresets[] = {"off", "10", "10,5", "15,10,5", "20,10"};
      constexpr size_t kCount = sizeof(kPresets) / sizeof(kPresets[0]);
      std::string current;
      for (uint8_t level : battery_alert_levels_)
        if (level) current += (current.empty() ? "" : ",") + std::to_string(level);
      if (current.empty()) current = "off";
      size_t next = 0;  // Off, unless the current list is a preset
      for (size_t i = 0; i < kCount; ++i)
        if (current == kPresets[i]) next = (i + 1) % kCount;
      console(std::string("set battery_alert ") + kPresets[next]);
      break;
    }
    case Menu::BatteryAlertSound:
      console(battery_alert_sound_ ? "set battery_alert_sound off" : "set battery_alert_sound on");
      break;
    case Menu::ChargingNotice:
      console(charging_notice_enabled_ ? "set charging_notice off" : "set charging_notice on");
      break;
    case Menu::ChargingSound:
      console(charging_sound_ ? "set charging_sound off" : "set charging_sound on");
      break;
    case Menu::ChargedNotice:
      console(charged_notice_enabled_ ? "set charged_notice off" : "set charged_notice on");
      break;
    case Menu::ChargedSound:
      console(charged_sound_ ? "set charged_sound off" : "set charged_sound on");
      break;
    case Menu::TouchWake:
      console(touch_wake_ ? "set touch_wake off" : "set touch_wake on");
      break;
    case Menu::WifiDeepSleep:
      console(wifi_deep_sleep_ ? "set wifi_deep_sleep off" : "set wifi_deep_sleep on");
      break;
    case Menu::DoubleHome:
      console(double_home_ ? "set double_home off" : "set double_home on");
      break;
    case Menu::WakeTalk:
      console(wake_talk_ ? "set wake_talk off" : "set wake_talk on");
      break;
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
  // The list eases toward its target: half the way each step, about 60 steps a second.
  constexpr uint32_t kScrollStepMs = 16;
  if (settings_open() && settings_scroll_ != settings_target_ && now() - settings_step_at_ >= kScrollStepMs) {
    settings_step_at_ = now();
    const int d = settings_target_ - settings_scroll_;
    settings_scroll_ += d > 0 ? (d + 1) / 2 : (d - 1) / 2;
    update_model();
  }
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
  // Every item the board offers is a row; the selected one also fills detail and body.
  m.settings.clear();
  m.settings_sel = 0;
  UiModel item_text;
  for (int i = static_cast<int>(Menu::Volume); i <= static_cast<int>(Menu::Back); ++i) {
    const Menu item = static_cast<Menu>(i);
    if (!menu_offered(item)) continue;
    item_text.detail.clear();
    item_text.body.clear();
    item_text.color_test = false;
    settings_item(item, item_text);
    m.settings.push_back(settings_row(item, item_text.detail, item_text.body));
    if (item != menu_) continue;
    m.settings_sel = static_cast<int>(m.settings.size()) - 1;
    m.detail = std::move(item_text.detail);
    m.body = std::move(item_text.body);
    m.color_test = item_text.color_test;
  }
  if (!check_result_.empty()) m.body += "\n" + check_result_;
  if (!ui_) settings_scroll_ = settings_target_ = 0;
  else if (settings_follow_) settings_target_ = ui_->settings_scroll_for(m, settings_target_);
  m.settings_scroll = settings_scroll_;
}

// The state at the right of an item's row: by default the first line of its text.
SettingRow App::settings_row(Menu item, const std::string& label, const std::string& body) const {
  using Icon = SettingRow::Icon;
  using Kind = SettingRow::Kind;
  SettingRow row;
  row.label = label;
  row.value = body.substr(0, body.find('\n'));
  switch (item) {
    case Menu::Volume:
      row.icon = Icon::Volume;
      if (hal_.speaker) { row.kind = Kind::Slider; row.level = static_cast<uint8_t>(volume_); }
      if (row.kind == Kind::Slider && item == menu_ && item == slide_item_ && slide_level_ >= 0) row.level = static_cast<uint8_t>(slide_level_);
      break;
    case Menu::Brightness:
      row.icon = Icon::Sun;
      if (hal_.display && hal_.display->info().has_backlight) { row.kind = Kind::Slider; row.level = static_cast<uint8_t>(brightness_); }
      if (row.kind == Kind::Slider && item == menu_ && item == slide_item_ && slide_level_ >= 0) row.level = static_cast<uint8_t>(slide_level_);
      break;
    case Menu::TalkMode:
      row.icon = Icon::Talk;
      row.value = hal_.mic ? tr(talk_mode_ == TalkMode::Hold ? i18n::Msg::TalkHold : i18n::Msg::TalkTap) : "";
      break;
    case Menu::Profile: row.icon = Icon::Person; break;
    case Menu::ProfileGrid: row.icon = Icon::Grid; break;
    case Menu::Microphone: row.icon = Icon::Mic; row.kind = Kind::Action; break;
    case Menu::Speaker: row.icon = Icon::Note; row.kind = Kind::Action; break;
    case Menu::Display: row.icon = Icon::Screen; row.kind = Kind::Action; break;
    case Menu::Inputs: row.icon = Icon::Keys; row.kind = Kind::Action; break;
    case Menu::Info: row.icon = Icon::Info; row.kind = Kind::Action; break;
    case Menu::Power:
      row.icon = Icon::Battery;
      row.value.clear();
      if (const auto percent = display_battery_percent()) row.value = std::to_string(*percent) + "%";
      break;
    case Menu::BatteryAlert: row.icon = Icon::Bell; break;
    case Menu::BatteryAlertSound: row.icon = Icon::Volume; row.kind = Kind::Toggle; row.level = battery_alert_sound_; break;
    case Menu::ChargingNotice: row.icon = Icon::Bolt; row.kind = Kind::Toggle; row.level = charging_notice_enabled_; break;
    case Menu::ChargingSound: row.icon = Icon::Volume; row.kind = Kind::Toggle; row.level = charging_sound_; break;
    case Menu::ChargedNotice: row.icon = Icon::Check; row.kind = Kind::Toggle; row.level = charged_notice_enabled_; break;
    case Menu::ChargedSound: row.icon = Icon::Volume; row.kind = Kind::Toggle; row.level = charged_sound_; break;
    case Menu::WifiDeepSleep: row.icon = Icon::Wifi; row.kind = Kind::Toggle; row.level = wifi_deep_sleep_; break;
    case Menu::IdleTimer: row.icon = Icon::Timer; break;
    case Menu::TouchTalk: row.icon = Icon::Touch; row.kind = Kind::Toggle; row.level = touch_talk_; break;
    case Menu::TouchWake: row.icon = Icon::Touch; row.kind = Kind::Toggle; row.level = touch_wake_; break;
    case Menu::FaceDownSleep: row.icon = Icon::FaceDown; row.kind = Kind::Toggle; row.level = face_down_sleep_; break;
    case Menu::Clock: row.icon = Icon::Clock; break;
    case Menu::PowerOff: row.icon = Icon::Power; row.kind = Kind::Action; break;
    case Menu::WifiSetup: row.icon = Icon::Wifi; row.kind = Kind::Action; break;
    case Menu::Phone: row.icon = Icon::Phone; row.kind = Kind::Action; break;
    case Menu::Language: row.icon = Icon::Globe; break;
    case Menu::DoubleHome: row.icon = Icon::Back; row.kind = Kind::Toggle; row.level = double_home_; break;
    case Menu::WakeTalk: row.icon = Icon::Talk; row.kind = Kind::Toggle; row.level = wake_talk_; break;
    case Menu::Back: row.icon = Icon::Back; row.kind = Kind::Action; break;
    case Menu::Closed: break;
  }
  if (row.kind == Kind::Action) row.value.clear();
  if (row.kind == Kind::Slider) row.value = std::to_string(row.level) + "%";
  return row;
}

bool App::settings_list_live() const {
  return ui_ && settings_open() && hardware_check_ == HardwareCheck::None;
}

App::Menu App::settings_item_at(int row) const {
  for (int i = static_cast<int>(Menu::Volume); i <= static_cast<int>(Menu::Back); ++i) {
    const Menu item = static_cast<Menu>(i);
    if (menu_offered(item) && row-- == 0) return item;
  }
  return Menu::Closed;
}

void App::settings_select(Menu item) {
  stop_hardware_check();
  check_result_.clear();
  power_off_armed_ = false;
  menu_ = item;
  settings_follow_ = true;
}

// Slider levels land on steps of 5, the steps a finger can pick on a small screen.
void App::settings_set_level(int level) {
  level = (level + 2) / 5 * 5;
  if (menu_ == Menu::Volume) console("set volume " + std::to_string(level));
  else if (menu_ == Menu::Brightness) console("set brightness " + std::to_string(std::max(5, level)));
}

void App::settings_press(int x, int y) {
  // The list stops where it is: a tap acts on the row under the finger at touch-down.
  if (settings_scroll_ != settings_target_) {
    settings_target_ = settings_scroll_;
    settings_follow_ = false;
  }
  press_y_ = y;
  press_scroll_ = settings_scroll_;
  slide_level_ = -1;
  sliding_ = settings_list_live() && ui_->settings_slider_at(model_, x, y, false) >= 0;
  slide_item_ = menu_;
}

void App::settings_drag(int x, int y) {
  if (!settings_list_live()) return;
  if (sliding_ && menu_ != slide_item_) {  // a knob or a key moved the selection: the slide ends unsaved,
    slide_level_ = -1;                      // and the rest of this drag does nothing
  } else if (sliding_) {
    slide_level_ = ui_->settings_slider_at(model_, x, y, true);
  } else {
    // The list follows the finger, within its ends.
    settings_follow_ = false;
    settings_scroll_ = settings_target_ =
        std::max(0, std::min(press_scroll_ - (y - press_y_), ui_->settings_max_scroll(model_)));
  }
  update_model();
}

void App::settings_tap(int x, int y) {
  sliding_ = false;
  if (!settings_list_live()) return;
  const int level = ui_->settings_slider_at(model_, x, y, false);
  if (level >= 0) {
    settings_set_level(level);
    update_model();
    return;
  }
  const int row = ui_->settings_row_hit(model_, x, y);
  if (row < 0) return;
  const Menu item = settings_item_at(row);
  if (item == Menu::Closed) return;
  if (item != menu_) {
    const bool slider = model_.settings[static_cast<size_t>(row)].kind == SettingRow::Kind::Slider;
    settings_select(item);
    if (slider) {  // the first tap opens the slider; the next ones on the bar set it
      update_model();
      return;
    }
  }
  settings_input(Button::Talk, false);
  update_model();
}

void App::settings_lift() {
  sliding_ = false;
  if (slide_level_ >= 0) {
    const int level = slide_level_;
    slide_level_ = -1;
    if (menu_ == slide_item_) settings_set_level(level);  // a knob moved off the slider meanwhile: no save
  }
  update_model();
}

void App::settings_item(Menu item, UiModel& m) const {
  using i18n::Msg;
  const std::string talk = control(profile_.talk_label);
  switch (item) {
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
    case Menu::TouchTalk:
      m.detail = tr(Msg::TouchTalk);
      m.body = touch_talk_ ? std::string(tr(Msg::TouchTalkOn)) : tr(Msg::TouchTalkOff, {talk});
      break;
    case Menu::Profile:
      m.detail = tr(Msg::ProfileSetting);
      m.body = tr(profile_.touch_screen ? Msg::ProfileSettingChoose : Msg::ProfileSettingNext, {agent_profile_name()});
      break;
    case Menu::ProfileGrid: {
      const int cols = profile_grid_ == 16 ? 4 : profile_grid_ == 9 ? 3 : 2;
      m.detail = tr(Msg::ProfileGridSetting);
      m.body = tr(Msg::ProfileGridValue, {std::to_string(cols) + " x " + std::to_string(profile_grid_ / cols)});
      break;
    }
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
    case Menu::BatteryAlert: {
      m.detail = tr(Msg::BatteryAlert);
      std::string levels;
      for (uint8_t level : battery_alert_levels_)
        if (level) levels += (levels.empty() ? "" : ", ") + std::to_string(level) + "%";
      m.body = levels.empty() ? std::string(tr(Msg::BatteryAlertOff)) : tr(Msg::BatteryAlertAt, {levels});
      break;
    }
    case Menu::BatteryAlertSound:
      m.detail = tr(Msg::BatteryAlertSound);
      m.body = tr(battery_alert_sound_ ? Msg::BatteryAlertSoundOn : Msg::BatteryAlertSoundOff);
      break;
    case Menu::ChargingNotice:
      m.detail = tr(Msg::ChargingNotice);
      m.body = tr(charging_notice_enabled_ ? Msg::ChargingNoticeOn : Msg::ChargingNoticeOff);
      break;
    case Menu::ChargingSound:
      m.detail = tr(Msg::ChargingSound);
      m.body = tr(charging_sound_ ? Msg::ChargingSoundOn : Msg::ChargingSoundOff);
      break;
    case Menu::ChargedNotice:
      m.detail = tr(Msg::ChargedNotice);
      m.body = tr(charged_notice_enabled_ ? Msg::ChargedNoticeOn : Msg::ChargedNoticeOff);
      break;
    case Menu::ChargedSound:
      m.detail = tr(Msg::ChargedSound);
      m.body = tr(charged_sound_ ? Msg::ChargedSoundOn : Msg::ChargedSoundOff);
      break;
    case Menu::WifiDeepSleep:
      m.detail = tr(Msg::WifiDeepSleep);
      m.body = tr(wifi_deep_sleep_ ? Msg::WifiDeepSleepOn : Msg::WifiDeepSleepOff);
      break;
    case Menu::TouchWake:
      m.detail = tr(Msg::TouchWake);
      m.body = tr(touch_wake_ ? Msg::TouchWakeOn : Msg::TouchWakeOff);
      break;
    case Menu::DoubleHome:
      m.detail = tr(Msg::DoubleHome);
      m.body = tr(double_home_ ? Msg::DoubleHomeOn : Msg::DoubleHomeOff, {control(profile_.talk_label)});
      break;
    case Menu::WakeTalk:
      m.detail = tr(Msg::WakeTalk);
      m.body = tr(wake_talk_ ? Msg::WakeTalkOn : Msg::WakeTalkOff, {control(profile_.talk_label)});
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
