#include "hg/app.hpp"
#include "hg/battery.hpp"

#include <algorithm>
#include <cstdlib>

namespace hg {
namespace {

// Face down: the screen points at the floor (device-frame milli-g). Down is z
// under -700 with x and y each under 500; up is z over -300.
constexpr int kDownZ = -700, kDownTiltMg = 500, kUpZ = -300;
constexpr uint32_t kFaceDownMs = 800, kFaceUpMs = 300;
constexpr uint32_t kMotionStaleMs = 2000;  // a silent sensor never keeps the screen dark
// The resting clock's share of the brightness setting: AMOLED care, with a floor
// so a very low setting keeps the face readable.
constexpr int kClockPercent = 70;
// The low battery alert stays up this long unless an input closes it first.
constexpr uint32_t kBatteryAlertMs = 10000;
// A spent alert level rearms once a charger lifts the reading this far above it, so
// a percentage that wobbles across a level, or a short time on USB, never repeats it.
constexpr int kBatteryAlertRearm = 5;
// The low battery alert's sound: a falling double beep, 880 Hz then 660 Hz, a little
// softer than the timer alarm's (9000).
constexpr Tone kLowBatteryTones[] = {{880, 120, 80, 8000}, {660, 160, 0, 8000}};
// Plugging in shows the charging notice this long; the screen then times out as usual.
constexpr uint32_t kChargingNoticeMs = 5000;
// Plugging in's sound, when charging_sound is on: a rising 660/990 Hz chirp.
constexpr Tone kChargingTones[] = {{660, 80, 20, 8000}, {990, 140, 0, 8000}};
// A full battery's sound: a soft rising 784/1046 Hz chime.
constexpr Tone kChargedTones[] = {{784, 140, 40, 7000}, {1046, 240, 0, 7000}};
// Readings in a row (5 s apart) that the charging signal must read false, after reading
// true, before the charge counts as finished.
constexpr uint8_t kChargeEndReads = 3;

}  // namespace

bool App::battery_is_estimated() const {
  if (!power_status_ || power_status_->battery_present == false) return false;
  const auto& p = *power_status_;
  return !p.battery_percent && battery_estimate_enabled_ && p.battery_mv &&
         *p.battery_mv >= 2000 && *p.battery_mv <= 4500;
}

std::optional<uint8_t> App::display_battery_percent() const {
  if (!power_status_ || power_status_->battery_present == false) return std::nullopt;
  if (power_status_->battery_percent) return power_status_->battery_percent;
  if (battery_is_estimated()) return estimate_liion_curve_percent(*power_status_->battery_mv);
  return std::nullopt;
}

// The one writer of the panel's backlight level: the brightness setting, capped
// by the resting clock, by the dim step, and dark while asleep or face down.
void App::apply_backlight() {
  if (!hal_.display || !hal_.display->info().has_backlight) return;
  int level = brightness_;
  if (clock_showing_) level = std::min(level, std::max(5, brightness_ * kClockPercent / 100));
  if (display_dimmed_) level = std::min(level, 10);
  if (display_sleeping_ || face_down_) level = 0;
  hal_.display->set_backlight(static_cast<uint8_t>(level));
}

bool App::wake_display() {
  if (face_down_) {
    // Lying face down, only turning the gadget over lights the screen. Ending a
    // doze lets what asked (an alarm's first beep) find the port awake; power_tick
    // dozes again when it is idle.
    set_dozing(false);
    return true;  // the input that asked is consumed, as on a timed-out screen
  }
  activity_at_ = now();
  const bool sleeping = display_sleeping_;
  const bool was_clock = clock_showing_;
  clock_showing_ = false;  // the clock ends at once, with the dim and sleep states
  if (display_dimmed_ || sleeping || was_clock) {
    display_dimmed_ = display_sleeping_ = false;
    on_battery_ = false;  // checked afresh before the next doze
    set_dozing(false);  // before drawing: the port wakes its peripherals
    apply_backlight();
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
  close_notices_for_input();  // like any other press: it closes the alert and does its own thing
  if (display_sleeping_ || display_dimmed_) {
    wake_display();
    return;
  }
  // Off means off: the menu, a list or a card the user was on closes, so the
  // next press wakes to the home screen and nothing open turns the screen back on.
  if (settings_open()) close_settings();
  if (overlay_ != Overlay::None) {
    dismiss_overlay();
    update_model();
  }
  display_sleeping_ = true;
  apply_backlight();
}

bool App::menu_open() const { return overlay_ == Overlay::Profiles || overlay_ == Overlay::Dashboard; }

// A turn, speech, held buttons, a prompt, Wi-Fi setup, phone pairing, an update,
// a card or an image: something for the user is on the screen. A list the user
// opened is not: left alone, it times out like the home screen. Side-effect free,
// so the screen timeout and the resting clock can both ask.
bool App::screen_busy() const {
  return mode_ != Mode::Idle || speaking() || talk_held_ || cancel_held_ || prompt_showing() ||
         !wifi_setup_text_.empty() || phone_pairing_ || ota_busy() || ota_ == Ota::Restarting ||
         (overlay_ != Overlay::None && !menu_open()) || battery_alert_;
}

// After each power reading. A level is crossed on battery when the percentage is at or
// below it with an explicit battery-only reading; the lowest level crossed is the one shown.
void App::battery_alert_check() {
  if (!battery_alert_levels_[0] || !power_status_) return;
  const auto percent = display_battery_percent();
  if (!percent) return;
  const auto& p = *power_status_;
  if (p.external_power == true || p.charging == true) {
    // Charging rearms the spent levels the reading has climbed clear of, lowest first.
    while (battery_alert_spent_ && *percent >= battery_alert_spent_ + kBatteryAlertRearm) {
      uint8_t next = 0;  // the next level above the spent one, or none
      for (uint8_t level : battery_alert_levels_)
        if (level > battery_alert_spent_ && (!next || level < next)) next = level;
      battery_alert_spent_ = next;
    }
    if (battery_alert_) dismiss_battery_alert();  // plugging in answers it
    return;
  }
  if (p.external_power != false) return;  // unknown supply: no alert, as with the red indicator
  uint8_t crossed = 0;
  for (uint8_t level : battery_alert_levels_)
    if (level && *percent <= level) crossed = level;  // highest first: the last match is the lowest
  if (!crossed || (battery_alert_spent_ && crossed >= battery_alert_spent_)) return;
  // A busy screen, a dark face-down gadget or the boot screen: the next reading tries again.
  if (battery_alert_ || face_down_ || alarm_ || phase_ == Phase::Boot || settings_open() || screen_busy()) return;
  battery_alert_spent_ = crossed;
  battery_alert_ = crossed;
  battery_alert_percent_ = *percent;
  battery_alert_until_ = now() + kBatteryAlertMs;
  log(LogLevel::Info, "low battery alert: " + std::to_string(*percent) + "%");
  wake_display();
  if (ui_) ui_->invalidate();
  update_model();
  if (battery_alert_sound_) play_tones(kLowBatteryTones, sizeof(kLowBatteryTones) / sizeof(kLowBatteryTones[0]));
}

void App::play_tones(const Tone* tones, size_t count) {
  if (!hal_.speaker || in_stream_ >= 0 || speaking() || hal_.speaker->busy() || mode_ != Mode::Idle || alarm_ ||
      face_down_)
    return;
  const auto pcm = synth_tones(tones, count, profile_.speaker_rate, kToneLeadMs);
  if (pcm.empty() || !hal_.speaker->begin(profile_.speaker_rate)) return;
  hal_.speaker->write(pcm.data(), pcm.size());
  hal_.speaker->end();
}

void App::close_notices_for_input() {
  if (battery_alert_) dismiss_battery_alert();
  if (power_notice_ != PowerNotice::None) dismiss_charging_notice(false);
}

// Lights the screen with a notice for a few seconds, unless the screen is in use or dark
// on purpose (face down). False when it could not show.
bool App::show_power_notice(PowerNotice notice) {
  if (!hal_.display || face_down_ || alarm_ || settings_open() || screen_busy()) return false;
  // A dark screen with no timeout to darken it again goes dark when the notice ends.
  charging_notice_darkens_ = (display_sleeping_ && !screen_timeout_ms_) ||
                             (power_notice_ != PowerNotice::None && charging_notice_darkens_);
  power_notice_ = notice;
  charging_notice_until_ = now() + kChargingNoticeMs;
  wake_display();
  if (ui_) ui_->invalidate();
  update_model();
  return true;
}

// Each reading on USB: the battery is full once a charge ends, or once the gauge climbs
// to 100%. A charge has ended when the charging signal, after reading true, reads false
// kChargeEndReads readings in a row with USB still in: a charger that is still starting
// up, or that flickers off for a reading, ends nothing. The gauge counts only after it read
// below 100% on this plug (and, where the board reports charging, after charging was seen).
// Once per plug; a busy screen holds it for a later reading.
void App::charged_check() {
  if (plugged_ != true || !power_status_) {
    reset_charge();
    return;
  }
  if (charged_done_ || phase_ == Phase::Boot) return;
  const auto& p = *power_status_;
  const auto percent = display_battery_percent();
  const bool gauge = percent && !battery_is_estimated();  // an estimate reads high on a charger
  if (p.charging == true) {
    charge_seen_ = true;
    charge_stopped_reads_ = 0;
  } else if (p.charging == false && charge_seen_ && charge_stopped_reads_ < kChargeEndReads) {
    ++charge_stopped_reads_;
  }
  if (gauge && *percent < 100) below_full_seen_ = true;
  const bool ended = charge_seen_ && charge_stopped_reads_ >= kChargeEndReads;
  const bool reached = gauge && *percent >= 100 && below_full_seen_ && (charge_seen_ || !p.charging.has_value());
  if (!ended && !reached) return;
  if (charged_notice_enabled_ && !show_power_notice(PowerNotice::Charged)) return;
  charged_done_ = true;
  if (charged_sound_) play_tones(kChargedTones, sizeof(kChargedTones) / sizeof(kChargedTones[0]));
}

// Each supply reading: USB coming in after a known battery-only reading lights the
// screen with the charging notice. A busy or face-down screen is left as it is.
void App::note_supply(std::optional<bool> plugged) {
  const bool plugged_in = plugged == true && plugged_ == false;
  // Any change of supply starts a new charge, also an unplug and replug that only the
  // dark screen's quick poll saw between two readings.
  if (plugged != plugged_) reset_charge();
  plugged_ = plugged;
  if (!plugged_in || phase_ == Phase::Boot) return;
  if (charging_notice_enabled_) show_power_notice(PowerNotice::Charging);
  if (charging_sound_) play_tones(kChargingTones, sizeof(kChargingTones) / sizeof(kChargingTones[0]));
}

void App::reset_charge() {
  charge_seen_ = charged_done_ = below_full_seen_ = false;
  charge_stopped_reads_ = 0;
}

void App::dismiss_charging_notice(bool darken) {
  if (power_notice_ == PowerNotice::None) return;
  power_notice_ = PowerNotice::None;
  if (ui_) ui_->invalidate();
  update_model();
  if (darken && charging_notice_darkens_) {
    display_sleeping_ = true;
    apply_backlight();
  }
  charging_notice_darkens_ = false;
}

void App::dismiss_battery_alert() {
  if (!battery_alert_) return;
  battery_alert_ = 0;
  if (ui_) ui_->invalidate();
  update_model();
}

void App::power_tick() {
  if (hal_.power && (!power_read_at_ || now() - power_read_at_ >= 5000)) {
    power_status_ = hal_.power->read();
    power_read_at_ = now();
    sensors_dirty_ = true;
    if (settings_open()) update_model();
    battery_alert_check();  // first: plugging in closes the low battery alert for the notice
    if (power_status_) {
      const auto& p = *power_status_;
      note_supply(p.external_power.has_value() ? p.external_power : p.charging);
    } else {
      note_supply(std::nullopt);
    }
    charged_check();
  }
  // Its own sound keeps the speaker busy, so it gives way to what takes the screen, not to that.
  // An open list stays under it, as under the low battery alert.
  if (power_notice_ != PowerNotice::None &&
      (static_cast<int32_t>(now() - charging_notice_until_) >= 0 || mode_ != Mode::Idle || prompt_showing() ||
       (overlay_ != Overlay::None && !menu_open()) || battery_alert_ || alarm_ || face_down_ || settings_open() ||
       !wifi_setup_text_.empty() || phone_pairing_ || ota_busy() || ota_ == Ota::Restarting || plugged_ != true))
    dismiss_charging_notice(static_cast<int32_t>(now() - charging_notice_until_) >= 0);
  // Something else took the screen, or its time ran out: the alert goes.
  if (battery_alert_ && (static_cast<int32_t>(now() - battery_alert_until_) >= 0 || mode_ != Mode::Idle ||
                         prompt_showing() || alarm_ || face_down_ || settings_open() || !wifi_setup_text_.empty() ||
                         phone_pairing_ || ota_busy() || ota_ == Ota::Restarting))
    dismiss_battery_alert();
  constexpr uint32_t kPowerKeyPollMs = 100;
  if (hal_.power && now() - power_key_polled_at_ >= kPowerKeyPollMs) {
    power_key_polled_at_ = now();
    if (hal_.power->take_key_press()) on_power_key();
    // Dozing needs the battery to be the only supply; check as often as the key
    // while the screen is dark, so plugging in ends a doze within ~100 ms.
    if (display_sleeping_) {
      const auto external = hal_.power->external_power();
      on_battery_ = external.has_value() && !*external;
      if (external.has_value()) note_supply(external);  // a dark screen lights at once on USB
    }
  }
  bool doze = false;
  // Without a timeout, only a screen the power key turned off needs watching.
  if (hal_.display && hal_.display->info().has_backlight && (screen_timeout_ms_ || display_sleeping_)) {
    // The settings menu can sleep too; the wake input leaves it on the same item.
    const bool busy = screen_busy();
    const bool settled = phase_ == Phase::NoNetwork || (phase_ == Phase::Online && paired_);
    // A ringing alarm keeps the screen lit without counting as activity itself, so once it is
    // quiet the screen dims and sleeps on the normal timeout.
    const bool ringing = alarm_ringing_;
    const bool idle = !busy && !ringing && settled;
    // Something for the user wakes the screen. A connection still settling keeps a
    // lit screen lit, but leaves a dark one dark: Wi-Fi coming and going (a commute)
    // must not turn the screen on and leave it on.
    if (busy || (!settled && !display_sleeping_)) {
      wake_display();
    } else if (ringing) {
      if (display_sleeping_ || display_dimmed_) wake_display();
    } else if (idle && screen_timeout_ms_) {
      const uint32_t elapsed = now() - activity_at_;
      if (elapsed >= screen_timeout_ms_ && !display_sleeping_) {
        display_sleeping_ = true;
        apply_backlight();
        // A list left open closes in the dark: the wake shows the home screen.
        if (menu_open()) {
          dismiss_overlay();
          update_model();
        }
      } else if (elapsed >= screen_timeout_ms_ / 2 && !display_dimmed_ && !display_sleeping_) {
        display_dimmed_ = true;
        apply_backlight();
      }
    }
    // Online, or waiting for Wi-Fi between join attempts (the port holds the chip
    // awake while an attempt runs). Reconnecting to Hermes isn't idle: awake.
    doze = idle && display_sleeping_ && on_battery_;
  }
  set_dozing(doze);
}

bool App::can_sleep_face_down() const {
  // Every path that darkens the screen and wakes it again needs a backlight: a
  // board without one would freeze a lit frame while telling Hermes it is dark.
  return profile_.motion_sensor && hal_.display && hal_.display->info().has_backlight;
}

bool App::face_down_sleep_enabled() const { return can_sleep_face_down() && face_down_sleep_; }

void App::on_motion(int16_t x_mg, int16_t y_mg, int16_t z_mg) {
  motion_[0] = x_mg;
  motion_[1] = y_mg;
  motion_[2] = z_mg;
  motion_at_ = now();
  motion_seen_ = true;
}

void App::motion_tick() {
  if (!motion_seen_) return;
  const uint32_t t = now() | 1u;  // 0 means "not since"
  if (now() - motion_at_ > kMotionStaleMs) {
    // The sensor stopped reporting: never stay dark on old data.
    down_since_ = up_since_ = 0;
    if (face_down_) leave_face_down();
    return;
  }
  const int x = motion_[0], y = motion_[1], z = motion_[2];
  const bool down = z < kDownZ && std::abs(x) < kDownTiltMg && std::abs(y) < kDownTiltMg;
  const bool up = z > kUpZ;
  down_since_ = down ? (down_since_ ? down_since_ : t) : 0;
  up_since_ = up ? (up_since_ ? up_since_ : t) : 0;
  if (face_down_) {
    if (!face_down_sleep_enabled() || (up_since_ && t - up_since_ >= kFaceUpMs)) leave_face_down();
  } else if (face_down_sleep_enabled() && down_since_ && t - down_since_ >= kFaceDownMs) {
    enter_face_down();
  }
}

void App::enter_face_down() {
  face_down_ = true;
  // A reply being spoken goes quiet and its text waits for the turn-over, like a
  // reply that arrives face down. The turn carries on: Hermes is not told to stop.
  reply_while_face_down_ = mode_ == Mode::Responding;
  if (mode_ == Mode::Listening) cancel_listening("face down");
  // A ringing alarm goes quiet and stays up: flipping the gadget over means "hush".
  alarm_ringing_ = false;
  stop_playback();  // the speech, an alarm beep, a tone, or a stream a later turn.start left playing
  if (settings_open()) close_settings();
  // A screen sleep that only turning the gadget over ends: every reader of a
  // dark screen (the model, Hermes, the status) stays right without new branches.
  display_dimmed_ = false;
  display_sleeping_ = true;
  clock_showing_ = false;
  apply_backlight();
  log(LogLevel::Info, "face down: screen off, Hermes muted");
  update_model();
}

void App::leave_face_down() {
  face_down_ = false;
  down_since_ = 0;
  if (reply_while_face_down_ && !reply_.empty()) {
    // The reply that arrived face down shows from the top for the usual window.
    reply_until_ = now() + kReplyLingerMs;
    scroll_ = 0;
    page_at_ = now() + page_dwell_ms();
  }
  reply_while_face_down_ = false;
  wake_display();  // lights at the brightness setting, ends a doze, redraws
  log(LogLevel::Info, "face up: screen on");
}

// The clock rests only on an online, paired Ready screen that nothing else is
// using: no turn, no overlay, no settings, no message waiting to be read.
bool App::clock_blocked() const {
  if (!clock_after_ms_ || !hal_.display || !wall_valid_) return true;
  if (phase_ != Phase::Online || !paired_) return true;
  if (screen_busy() || settings_open() || overlay_ != Overlay::None) return true;
  if (alarm_ || timer_active()) return true;
  if (!fatal_.empty() || !notice_.empty()) return true;
  // The low-battery caption (update_model) holds the Ready screen the same way.
  if (power_status_ && power_status_->battery_percent && *power_status_->battery_percent <= 10 &&
      power_status_->external_power == false)
    return true;
  if (!reply_.empty() && static_cast<int32_t>(reply_until_ - now()) > 0) return true;  // the reply text lingers
  if (!hint_flash_.empty()) return true;
  if (display_sleeping_) return true;  // face-down sleep holds this flag until the turn-over
  return false;
}

void App::clock_tick() {
  if (clock_blocked()) {
    // Rest starts when the block ends, never during it.
    clock_rest_at_ = now();
    if (clock_showing_) {
      clock_showing_ = false;
      apply_backlight();  // keeps the dim or dark state: what ended the clock decides
      update_model();
    }
    return;
  }
  if (clock_showing_) return;
  // Rest counts from the later of the last blocked frame and the last activity
  // (signed ages, like the timers), so speech and a reply's text hold it off.
  const uint32_t rest_at = static_cast<int32_t>(clock_rest_at_ - activity_at_) >= 0 ? clock_rest_at_ : activity_at_;
  if (static_cast<int32_t>(now() - rest_at) < static_cast<int32_t>(clock_after_ms_)) return;
  clock_showing_ = true;
  apply_backlight();  // a clock that starts on a dimmed screen keeps the dim level
  update_model();
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
