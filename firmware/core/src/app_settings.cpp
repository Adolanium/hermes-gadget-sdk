#include "hg/app.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "hg/console_args.hpp"
#include "hg/wifi_networks.hpp"

namespace {

std::string stc8_hex_byte(std::uint8_t value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result = "0x00";
  result[2] = kHex[value >> 4];
  result[3] = kHex[value & 0x0f];
  return result;
}

}  // namespace

namespace hg {

bool App::settings_hold_live() const {
  return !prompt_showing() && !ota_busy() && !wifi_setup_open() && !phone_pairing_;
}

bool App::settings_title_hit(int x, int y) const {
  return ui_ && settings_hold_live() && ui_->title_hit(x, y);
}

bool App::open_settings() {
  if (prompt_showing() || ota_busy() || ota_ == Ota::Restarting || !wifi_setup_text_.empty() || phone_pairing_)
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
  if ((hardware_check_ == HardwareCheck::Microphone || hardware_check_ == HardwareCheck::MicrophonePlayback) && hal_.mic)
    hal_.mic->stop();
  if ((hardware_check_ == HardwareCheck::Speaker || hardware_check_ == HardwareCheck::MicrophonePlayback) && hal_.speaker)
    hal_.speaker->abort();
  hardware_check_ = HardwareCheck::None;
  mic_loopback_buf_.clear();
  mic_meter_energy_ = 0.0;
  mic_meter_samples_ = 0;
  mic_meter_update_at_ = 0;
  mic_meter_smoothed_ = false;
  level_ = 0;
}

void App::play_mic_loopback() {
  if (hal_.mic) hal_.mic->stop();
  if (mic_loopback_buf_.empty()) {
    hardware_check_ = HardwareCheck::None;
    check_result_ = "No mic signal was captured.";
    update_model();
    return;
  }
  const size_t capture_rate = profile_.mic_rate ? profile_.mic_rate : 16000;
  const size_t playback_samples = std::min(capture_rate, mic_loopback_buf_.size());
  const size_t hop = std::max<size_t>(1, capture_rate / 10);
  size_t best_start = 0;
  float best_rms = 0.0f;
  for (size_t start = 0; start + playback_samples <= mic_loopback_buf_.size(); start += hop) {
    const float candidate = rms(mic_loopback_buf_.data() + start, playback_samples);
    if (candidate > best_rms) {
      best_rms = candidate;
      best_start = start;
    }
  }
  const size_t last_start = mic_loopback_buf_.size() - playback_samples;
  if (best_start != last_start) {
    const float candidate = rms(mic_loopback_buf_.data() + last_start, playback_samples);
    if (candidate > best_rms) {
      best_rms = candidate;
      best_start = last_start;
    }
  }
  if (best_rms < 1.0f) {
    hardware_check_ = HardwareCheck::None;
    mic_loopback_buf_.clear();
    check_result_ = "No mic signal was captured.";
    update_model();
    return;
  }
  const float port_gain = hal_.mic ? hal_.mic->loopback_gain() : 1.0f;
  const float gain = std::min(40.0f, std::max(port_gain, 8000.0f / best_rms));
  for (size_t i = 0; i < playback_samples; ++i) {
    const int32_t amplified = static_cast<int32_t>(mic_loopback_buf_[best_start + i] * gain);
    mic_loopback_buf_[i] = static_cast<int16_t>(std::max<int32_t>(-12000, std::min<int32_t>(12000, amplified)));
  }
  mic_loopback_buf_.resize(playback_samples);
  if (!hal_.speaker || !hal_.speaker->begin(profile_.speaker_rate)) {
    hardware_check_ = HardwareCheck::None;
    mic_loopback_buf_.clear();
    check_result_ = "Recording finished; speaker playback is unavailable.";
    update_model();
    return;
  }
  hal_.speaker->write(mic_loopback_buf_.data(), mic_loopback_buf_.size());
  hal_.speaker->end();
  hardware_check_ = HardwareCheck::MicrophonePlayback;
  check_result_ = "Playing the loudest second of the recording...";
  update_model();
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
    const bool can_power_off = hal_.power && hal_.power->can_power_off();
    // Steps over the items this board can't offer.
    const auto offered = [&](Menu item) {
      switch (item) {
        case Menu::Power: return hal_.power != nullptr;
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
      if (hal_.mic && hal_.mic->start(profile_.mic_rate)) {
        hardware_check_ = HardwareCheck::Microphone;
        if (profile_.mic_loopback_check) {
          const size_t rate = profile_.mic_rate ? profile_.mic_rate : 16000;
          mic_loopback_buf_.clear();
          mic_loopback_buf_.reserve(rate * 3);
          mic_meter_energy_ = 0.0;
          mic_meter_samples_ = 0;
          mic_meter_update_at_ = 0;
          mic_meter_smoothed_ = false;
          level_ = 0;
          check_result_ = "Recording three seconds locally; the loudest second will play back.";
        }
        update_model();
      } else check_result_ = "Microphone unavailable";
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
  if (hardware_check_ == HardwareCheck::MicrophonePlayback && hal_.speaker && !hal_.speaker->busy()) {
    hardware_check_ = HardwareCheck::None;
    mic_loopback_buf_.clear();
    check_result_ = "Playback finished. Did you hear the recording?";
    update_model();
  } else if (hardware_check_ == HardwareCheck::Speaker && !hal_.speaker->busy()) {
    hardware_check_ = HardwareCheck::None;
    check_result_ = "Tone finished. Did you hear it?";
    update_model();
  }
}

bool App::battery_readable() const {
  return display_battery_percent().has_value();
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
      if (hardware_check_ == HardwareCheck::Microphone)
        m.body = "Speak now. Level: " + std::to_string(level_) + "%\nLocal only. Nothing is sent.";
      else if (hardware_check_ == HardwareCheck::MicrophonePlayback)
        m.body = check_result_;
      else
        m.body = profile_.mic_loopback_check ? "Record three seconds locally; the loudest second plays back."
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
        if (const auto percent = display_battery_percent())
          m.body += std::to_string(*percent) + (battery_is_estimated() ? "% (voltage estimate)\n" : "% (gauge estimate)\n");
        if (p.battery_mv) m.body += std::to_string(*p.battery_mv) + " mV\n";
        if (p.charging) m.body += *p.charging ? "Charging\n" : "Not charging\n";
        if (p.external_power) m.body += *p.external_power ? "USB power\n" : "No USB power\n";
        if (p.battery_adc_mv) m.body += "ADC sense " + std::to_string(*p.battery_adc_mv) + " mV\n";
        if (p.battery_state) m.body += std::string("Charge state: ") + battery_charge_state_name(*p.battery_state) + "\n";
        if (p.battery_led_state) m.body += std::string("LED state: ") + battery_led_state_name(*p.battery_led_state) + "\n";
        if (p.battery_registers) {
          const auto& raw = *p.battery_registers;
          m.body += "STC8 raw registers (local only):\n";
          m.body += "0x00 adc_voltage[0] = " + stc8_hex_byte(raw[0]) + "\n";
          m.body += "0x01 adc_voltage[1] = " + stc8_hex_byte(raw[1]) + "\n";
          m.body += "0x02 adc_voltage[2] = " + stc8_hex_byte(raw[2]) + "\n";
          m.body += "0x03 adc_voltage[3] = " + stc8_hex_byte(raw[3]) + "\n";
          m.body += "0x04 bat_voltage[0] = " + stc8_hex_byte(raw[4]) + "\n";
          m.body += "0x05 bat_voltage[1] = " + stc8_hex_byte(raw[5]) + "\n";
          m.body += "0x06 bat_voltage[2] = " + stc8_hex_byte(raw[6]) + "\n";
          m.body += "0x07 bat_voltage[3] = " + stc8_hex_byte(raw[7]) + "\n";
          m.body += "0x08 percentage = " + stc8_hex_byte(raw[8]) +
                    (p.battery_percent ? " (" + std::to_string(*p.battery_percent) + "%)\n" : " (invalid)\n");
          m.body += "0x09 charge_state = " + stc8_hex_byte(raw[9]) +
                    (p.battery_state ? std::string(" (") + battery_charge_state_name(*p.battery_state) + ")\n" : " (invalid)\n");
          m.body += "0x0A led_state = " + stc8_hex_byte(raw[10]) +
                    (p.battery_led_state ? std::string(" (") + battery_led_state_name(*p.battery_led_state) + ")\n" : " (invalid)\n");
          m.body += "0x0B padding = " + stc8_hex_byte(raw[11]) + "\n";
        }
        if (p.battery_percent && *p.battery_percent <= 10 && p.external_power == false) m.body += "Low battery: connect USB.\n";
        if (!battery_estimate_enabled_ && !p.battery_percent && p.battery_present != false &&
            p.battery_mv && *p.battery_mv >= 2000 && *p.battery_mv <= 4500)
          m.body += "set battery_estimate liion\nBattery must be fitted; charging affects estimate.\n";
        else if (battery_is_estimated())
          m.body += "Battery must be fitted; charging affects estimate.\n";
      }
    if (battery_readable()) {  // nothing to choose when no percentage can appear
      static const char* const kShown[] = {"Icon", "Percent", "Both", "Off"};
      m.body += std::string("Indicator: ") + kShown[static_cast<int>(battery_display_)] + "\n" +
                (profile_.touch_screen ? "Tap to change" : profile_.talk_label + " to change");
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
    case Menu::Phone:
      m.detail = "Phone app";
      m.body = phones_paired_ == 0 ? std::string("No phone paired.\nSelect to pair one over Bluetooth.")
                                   : std::string(phone_connected_ ? "Phone connected.\n" : "Phone paired, not connected.\n") +
                                         "Select to pair another.";
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
  if (wifi_setup_text_.empty()) set_hint_flash("Wi-Fi setup unavailable; use USB");
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
