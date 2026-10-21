// Pairing a phone companion app over Bluetooth. The port owns the radio; the core
// owns the pairing window, its screen and the `phone` console command.
#include "hg/app.hpp"

#include <cstdio>

namespace hg {

bool App::start_phone_pairing() {
  if (!on_phone_pair || prompt_showing() || ota_busy() || ota_ == Ota::Restarting) return false;
  if (mode_ == Mode::Listening) cancel_listening("phone pairing");
  else if (mode_ != Mode::Idle) cancel_turn();
  stop_playback();
  close_settings();
  close_wifi_setup();
  wake_display();
  talk_held_ = false;
  cancel_held_ = false;
  settings_chord_fired_ = false;
  wake_buttons_ = 0;
  if (!on_phone_pair()) {
    set_hint_flash(tr(i18n::Msg::BluetoothUnavailable));
    update_model();
    return false;
  }
  phone_pairing_ = true;
  phone_pair_until_ = now() + kPhonePairMs;
  phone_passkey_.clear();
  update_model();
  return true;
}

void App::close_phone_pairing() {
  if (!phone_pairing_) return;
  phone_pairing_ = false;
  phone_passkey_.clear();
  if (on_phone_pair_close) on_phone_pair_close();
  update_model();
}

void App::close_setup_screens() {
  close_wifi_setup();
  close_phone_pairing();
}

void App::on_phone_passkey(uint32_t passkey) {
  if (!phone_pairing_) return;
  char code[8];
  std::snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(passkey % 1000000));
  phone_passkey_ = code;
  wake_display();
  update_model();
}

void App::on_phone_paired(bool ok) {
  if (!phone_pairing_) return;
  close_phone_pairing();
  set_hint_flash(tr(ok ? i18n::Msg::PhonePaired : i18n::Msg::PhonePairingFailed));
  update_model();
}

void App::on_phone_status(int paired, bool connected) {
  phones_paired_ = paired < 0 ? 0 : paired;
  phone_connected_ = connected;
  if (settings_open()) update_model();
}

void App::phone_tick() {
  if (phone_pairing_ && static_cast<int32_t>(now() - phone_pair_until_) >= 0) {
    close_phone_pairing();
    set_hint_flash(tr(i18n::Msg::PhonePairingClosed));
    update_model();
  }
}

json::Value App::phone_value() const {
  json::Value v = json::Value::object();
  v.set("paired", phones_paired_).set("connected", phone_connected_).set("pairing", phone_pairing_);
  return v;
}

}  // namespace hg
