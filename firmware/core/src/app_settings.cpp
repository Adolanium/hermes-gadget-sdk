#include "hg/app.hpp"

#include <algorithm>
#include <vector>

namespace hg {

bool App::settings_title_hit(int x, int y) const {
  return ui_ && !prompt_showing() && !ota_busy() && !wifi_setup_open() && ui_->title_hit(x, y);
}

bool App::open_settings() {
  if (prompt_showing() || ota_busy() || ota_ == Ota::Restarting || !wifi_setup_text_.empty()) return false;
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
      check_result_ = std::string(labels[static_cast<unsigned>(button)]) + " pressed";
      return;
    }
    if (button != Button::Cancel) return;
  }
  if ((button == Button::Cancel && !pressed) ||
      ((button == Button::Up || button == Button::Down) && pressed)) {
    stop_hardware_check();
    check_result_.clear();
    power_off_armed_ = false;
    int item = static_cast<int>(menu_) + (button == Button::Up ? -1 : 1);
    if (item < static_cast<int>(Menu::Volume)) item = static_cast<int>(Menu::Back);
    if (item > static_cast<int>(Menu::Back)) item = static_cast<int>(Menu::Volume);
    menu_ = static_cast<Menu>(item);
    const bool can_power_off = hal_.power && hal_.power->can_power_off();
    if ((!hal_.power && menu_ == Menu::Power) ||
        (!can_power_off && menu_ == Menu::PowerOff)) {
      menu_ = menu_ == Menu::Power ? (button == Button::Up ? Menu::Info : Menu::IdleTimer)
                                   : (button == Button::Up ? Menu::IdleTimer : Menu::WifiSetup);
    }
    if (!on_wifi_setup && menu_ == Menu::WifiSetup)
      menu_ = button == Button::Up ? (can_power_off ? Menu::PowerOff : Menu::IdleTimer) : Menu::Back;
    return;
  }
  if (button != Button::Talk || pressed) return;
  if (hardware_check_ != HardwareCheck::None) {
    stop_hardware_check();
    check_result_ = "Check stopped";
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
    case Menu::Microphone:
      if (hal_.mic && hal_.mic->start(profile_.mic_rate)) hardware_check_ = HardwareCheck::Microphone;
      else check_result_ = "Microphone unavailable";
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
        check_result_ = "Listen for a short tone";
      } else check_result_ = "Speaker unavailable";
      break;
    case Menu::Display:
      if (hal_.display) hardware_check_ = HardwareCheck::Display;
      else check_result_ = "Display unavailable";
      break;
    case Menu::Inputs:
      hardware_check_ = HardwareCheck::Inputs;
      check_result_ = "Press a button. Release Cancel to leave.";
      break;
    case Menu::IdleTimer: {
      const uint32_t seconds = screen_timeout_ms_ / 1000;
      const uint32_t next = seconds == 0 ? 30 : seconds < 60 ? 60 : seconds < 120 ? 120 : seconds < 300 ? 300 : 0;
      console("set screen_timeout " + std::to_string(next));
      break;
    }
    case Menu::PowerOff:
      if (!power_off_armed_) {
        power_off_armed_ = true;
        check_result_ = "Select again to power off. Cancel goes back.";
      } else {
        power_off_armed_ = false;
        check_result_ = hal_.power->power_off() ? "Power-off requested" : "Power-off failed. Try the physical PWR key.";
      }
      break;
    case Menu::Back: close_settings(); break;
    case Menu::WifiSetup: start_wifi_setup(); break;
    case Menu::Power:
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
    check_result_ = "Tone finished. Did you hear it?";
    update_model();
  }
}

void App::settings_model() {
  UiModel& m = model_;
  m.screen = Screen::Settings;
  m.headline = "Settings";
  m.scroll = 0;
  m.speaking = false;
  m.hint = profile_.touch_screen ? "Tap: change | Swipe: next"
                                : profile_.talk_label + ": change | " + profile_.cancel_label + ": next";
  switch (menu_) {
    case Menu::Volume:
      m.detail = "Speaker volume";
      m.body = hal_.speaker ? std::to_string(volume_) + "%\nChanges are saved." : "No speaker driver is active.";
      break;
    case Menu::Brightness:
      m.detail = "Screen brightness";
      m.body = hal_.display && hal_.display->info().has_backlight ? std::to_string(brightness_) + "%\nChanges are saved."
                                                               : "Brightness control is unavailable.";
      break;
    case Menu::TalkMode:
      m.detail = "Talk mode";
      m.body = !hal_.mic ? "No microphone driver is active." : talk_mode_ == TalkMode::Hold ? "Hold to record; release to send."
                                                                                         : "Tap to record; pause or tap to send.";
      break;
    case Menu::Microphone:
      m.detail = "Microphone check";
      m.body = hardware_check_ == HardwareCheck::Microphone ? "Speak now. Level: " + std::to_string(level_) + "%\nLocal only. Nothing is sent."
                                                          : "Start to check the microphone level. No recording is saved or sent.";
      break;
    case Menu::Speaker:
      m.detail = "Speaker check";
      m.body = "Start to play a short tone at the current volume.";
      break;
    case Menu::Display:
      m.detail = "Display check";
      m.body = "Start to show red, green, blue, white and black bands.";
      m.color_test = hardware_check_ == HardwareCheck::Display;
      break;
    case Menu::Inputs:
      m.detail = "Input check";
      m.body = "Start to check the controls. Cancel leaves this check.";
      break;
    case Menu::Info:
      m.detail = "Device information";
      m.body = profile_.board + "\nFirmware " + profile_.firmware + "\n" + device_id_ + "\nMicrophone: " +
               (hal_.mic ? "available" : "unavailable") + "\nSpeaker: " + (hal_.speaker ? "available" : "unavailable");
      break;
    case Menu::Power:
      m.detail = "Battery and power";
      if (!power_status_) m.body = "Power readings unavailable.";
      else {
        const auto& p = *power_status_;
        if (p.battery_present && !*p.battery_present) m.body = "No battery detected.\n";
        if (p.battery_percent) m.body += std::to_string(*p.battery_percent) + "% (gauge estimate)\n";
        if (p.battery_mv) m.body += std::to_string(*p.battery_mv) + " mV\n";
        if (p.charging) m.body += *p.charging ? "Charging\n" : "Not charging\n";
        if (p.external_power) m.body += *p.external_power ? "USB power\n" : "No USB power\n";
        if (p.battery_percent && *p.battery_percent <= 10 && p.external_power == false) m.body += "Low battery: connect USB.";
      }
      break;
    case Menu::IdleTimer:
      m.detail = "Screen timeout";
      m.body = screen_timeout_ms_ ? std::to_string(screen_timeout_ms_ / 1000) + " seconds\nDims halfway; first input wakes."
                                 : "Always on\nSelect to enable automatic dimming and screen sleep.";
      break;
    case Menu::PowerOff:
      m.detail = "Power off";
      m.body = "Shut down the board. Use its PWR key to turn it on again.";
      break;
    case Menu::WifiSetup:
      m.detail = "Wi-Fi setup";
      m.body = "Start a temporary network to configure this device with your phone.";
      break;
    case Menu::Back:
      m.detail = "Back to Hermes";
      m.body = "Select to close settings.";
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
  wake_display();
  talk_held_ = false;
  cancel_held_ = false;
  settings_chord_fired_ = false;
  wake_buttons_ = 0;
  wifi_setup_text_ = on_wifi_setup();
  if (wifi_setup_text_.empty()) set_hint_flash("Wi-Fi setup unavailable; use USB");
  update_model();
  return !wifi_setup_text_.empty();
}

void App::close_wifi_setup() {
  if (wifi_setup_text_.empty()) return;
  wifi_setup_text_.clear();
  if (on_wifi_setup_close) on_wifi_setup_close();
  update_model();
}

}  // namespace hg
