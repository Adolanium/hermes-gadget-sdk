#include "hg/app.hpp"

#include <algorithm>

namespace hg {

bool App::wake_display() {
  activity_at_ = now();
  const bool sleeping = display_sleeping_;
  if (display_dimmed_ || sleeping) {
    display_dimmed_ = display_sleeping_ = false;
    on_battery_ = false;  // checked afresh before the next doze
    set_dozing(false);  // before drawing: the port wakes its peripherals
    hal_.display->set_backlight(brightness_);
    if (ui_) ui_->invalidate();
    update_model();
  }
  return sleeping;
}

void App::set_dozing(bool dozing) {
  if (dozing == dozing_) return;
  dozing_ = dozing;
  if (hal_.system) hal_.system->set_dozing(dozing);
}

void App::on_power_key() {
  if (!hal_.display || !hal_.display->info().has_backlight) return;
  if (display_sleeping_ || display_dimmed_) {
    wake_display();
    return;
  }
  display_sleeping_ = true;
  hal_.display->set_backlight(0);
}

void App::power_tick() {
  if (hal_.power && (!power_read_at_ || now() - power_read_at_ >= 5000)) {
    power_status_ = hal_.power->read();
    power_read_at_ = now();
    sensors_dirty_ = true;
    if (settings_open()) update_model();
  }
  constexpr uint32_t kPowerKeyPollMs = 100;
  if (hal_.power && now() - power_key_polled_at_ >= kPowerKeyPollMs) {
    power_key_polled_at_ = now();
    if (hal_.power->take_key_press()) on_power_key();
    // Dozing needs the battery to be the only supply; check as often as the key
    // while the screen is dark, so plugging in ends a doze within ~100 ms.
    if (display_sleeping_) {
      const auto external = hal_.power->external_power();
      on_battery_ = external.has_value() && !*external;
    }
  }
  bool doze = false;
  // Without a timeout, only a screen the power key turned off needs watching.
  if (hal_.display && hal_.display->info().has_backlight && (screen_timeout_ms_ || display_sleeping_)) {
    // The settings menu can sleep too; the wake input leaves it on the same item.
    const bool busy = mode_ != Mode::Idle || speaking() || talk_held_ || cancel_held_ || prompt_showing() ||
                      !wifi_setup_text_.empty() || ota_busy() || ota_ == Ota::Restarting || overlay_ != Overlay::None;
    const bool settled = phase_ == Phase::NoNetwork || (phase_ == Phase::Online && paired_);
    const bool idle = !busy && settled;
    // Something for the user wakes the screen. A connection still settling keeps a
    // lit screen lit, but leaves a dark one dark: Wi-Fi coming and going (a commute)
    // must not turn the screen on and leave it on.
    if (busy || (!settled && !display_sleeping_)) {
      wake_display();
    } else if (idle && screen_timeout_ms_) {
      const uint32_t elapsed = now() - activity_at_;
      if (elapsed >= screen_timeout_ms_ && !display_sleeping_) {
        display_sleeping_ = true;
        hal_.display->set_backlight(0);
      } else if (elapsed >= screen_timeout_ms_ / 2 && !display_dimmed_ && !display_sleeping_) {
        display_dimmed_ = true;
        hal_.display->set_backlight(std::min<uint8_t>(brightness_, 10));
      }
    }
    // Online, or waiting for Wi-Fi between join attempts (the port holds the chip
    // awake while an attempt runs). Reconnecting to Hermes isn't idle: awake.
    doze = idle && display_sleeping_ && on_battery_;
  }
  set_dozing(doze);
}

json::Value App::power_value() const {
  json::Value value = json::Value::object();
  value.set("available", power_status_.has_value());
  if (!power_status_) return value;
  const auto& p = *power_status_;
  if (p.battery_present) value.set("battery_present", *p.battery_present);
  if (p.battery_mv) value.set("battery_mv", *p.battery_mv);
  if (p.battery_percent) value.set("battery_percent", *p.battery_percent);
  if (p.charging) value.set("charging", *p.charging);
  if (p.external_power) value.set("external_power", *p.external_power);
  return value;
}

}  // namespace hg
