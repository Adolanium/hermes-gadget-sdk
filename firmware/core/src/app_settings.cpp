#include "hg/app.hpp"

#include <algorithm>
#include <vector>

namespace hg {

bool App::settings_title_hit(int x, int y) const {
  return ui_ && !prompt_showing() && !ota_busy() && ui_->title_hit(x, y);
}

bool App::open_settings() {
  if (prompt_showing() || ota_busy() || ota_ == Ota::Restarting) return false;
  if (settings_open()) { close_settings(); return true; }
  if (mode_ == Mode::Listening) cancel_listening("local settings");
  else if (mode_ == Mode::Thinking || mode_ == Mode::Responding) cancel_turn();
  stop_playback();
  dismiss_overlay();
  menu_ = Menu::Volume;
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
    int item = static_cast<int>(menu_) + (button == Button::Up ? -1 : 1);
    if (item < static_cast<int>(Menu::Volume)) item = static_cast<int>(Menu::Back);
    if (item > static_cast<int>(Menu::Back)) item = static_cast<int>(Menu::Volume);
    menu_ = static_cast<Menu>(item);
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
    case Menu::Back: close_settings(); break;
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
  m.headline = "Settings " + std::to_string(static_cast<int>(menu_)) + "/9";
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
    case Menu::Back:
      m.detail = "Back to Hermes";
      m.body = "Select to close settings.";
      break;
    case Menu::Closed: break;
  }
  if (!check_result_.empty()) m.body += "\n" + check_result_;
}

}  // namespace hg
