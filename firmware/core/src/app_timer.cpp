// Timers Hermes sets on the device ("set a pasta timer for nine minutes").
// The screen shows the time left (an arc around a round panel, a line along a
// rectangular one, and the Ready caption). At zero the screen shows TIMER DONE
// and, on a board with a speaker, a double beep sounds for at most a minute.
// The alarm stays until someone dismisses it. RAM only: a reboot forgets it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hg/app.hpp"
#include "hg/font.hpp"

namespace hg {
namespace {

constexpr int64_t kMaxTimerS = 86400;
constexpr size_t kMaxLabelCols = 40;     // code points, as JSON Schema counts maxLength
constexpr uint32_t kRingMaxMs = 60000;   // beep for at most this long; the alarm stays up
constexpr uint32_t kBeepCycleMs = 1500;  // one double beep per cycle

json::Value int_param(const char* description, int64_t lo, int64_t hi) {
  json::Value p = json::Value::object();
  p.set("type", "integer").set("minimum", lo).set("maximum", hi).set("description", description);
  return p;
}

}  // namespace

void App::add_timer_actions() {
  if (!hal_.display && !hal_.speaker) return;

  Action start;
  start.name = "timer.start";
  start.description =
      "Start a countdown timer on the device, replacing any running one. The screen shows the time left; "
      "at zero the device beeps (if it has a speaker) and shows the label until someone dismisses it.";
  json::Value props = json::Value::object();
  props.set("seconds", int_param("Length of the timer in seconds", 1, kMaxTimerS));
  json::Value label = json::Value::object();
  label.set("type", "string").set("maxLength", static_cast<int64_t>(kMaxLabelCols)).set(
      "description", "Optional short name shown when it ends, e.g. 'Pasta'");
  props.set("label", label);
  json::Value req = json::Value::array();
  req.push("seconds");
  start.params.set("type", "object").set("properties", props).set("required", req);
  start.handler = [this](const json::Value& args, json::Value& result, std::string& error) {
    if (!args["seconds"].is_number()) {
      error = "seconds is required";
      return false;
    }
    const int64_t s = args["seconds"].as_int();
    if (s < 1 || s > kMaxTimerS) {
      error = "seconds must be between 1 and 86400";
      return false;
    }
    std::string name(args["label"].str_or(""));
    name.resize(font::prefix(name, kMaxLabelCols));
    timer_due_ = false;
    timer_running_ = true;
    timer_total_ms_ = static_cast<uint32_t>(s) * 1000u;
    timer_end_ = now() + timer_total_ms_;
    timer_label_ = name;
    if (alarm_) dismiss_alarm();  // one repaint: the alarm goes and the new timer's arc comes
    else update_model();
    result.set("seconds", s).set("label", name);
    return true;
  };
  add_action(std::move(start));

  Action cancel;
  cancel.name = "timer.cancel";
  cancel.description = "Cancel the device's timer, or stop its alarm.";
  cancel.params.set("type", "object").set("properties", json::Value::object());
  cancel.handler = [this](const json::Value&, json::Value& result, std::string&) {
    const bool had = timer_active();
    timer_running_ = timer_due_ = false;
    if (alarm_) dismiss_alarm();
    update_model();
    result.set("cancelled", had);
    return true;
  };
  add_action(std::move(cancel));

  Action status;
  status.name = "timer.status";
  status.description = "Report the device's timer: whether it runs or rings, the seconds left and its label.";
  status.params.set("type", "object").set("properties", json::Value::object());
  status.handler = [this](const json::Value&, json::Value& result, std::string&) {
    result = timer_value();
    return true;
  };
  add_action(std::move(status));
}

bool App::timer_active() const { return timer_running_ || timer_due_ || alarm_; }

uint32_t App::timer_left_ms() const {
  if (!timer_running_) return 0;
  // Signed: a tick that reads the clock late still sees zero, never a wrapped age.
  return static_cast<uint32_t>(std::max<int32_t>(0, static_cast<int32_t>(timer_end_ - now())));
}

int16_t App::timer_permille() const {
  if (!timer_running_ || timer_total_ms_ == 0) return -1;
  const uint64_t left = timer_left_ms();
  return static_cast<int16_t>(std::min<uint64_t>(1000, left * 1000 / timer_total_ms_));
}

std::string App::timer_left_text() const {
  // The ceiling: 0:01 until the last second is gone. unsigned, not uint32_t: that is
  // `unsigned long` on the ESP32's compiler, and %u must match on every target.
  const unsigned secs = static_cast<unsigned>((timer_left_ms() + 999) / 1000);
  char buf[24];
  if (secs >= 3600)
    std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", secs / 3600, (secs % 3600) / 60, secs % 60);
  else
    std::snprintf(buf, sizeof(buf), "%u:%02u", secs / 60, secs % 60);
  return buf;
}

json::Value App::timer_value() const {
  const bool active = timer_active();
  const uint32_t secs = (timer_left_ms() + 999) / 1000;
  json::Value v = json::Value::object();
  v.set("running", timer_running_)
      .set("ringing", alarm_ringing_)
      .set("done", timer_due_ || alarm_)
      .set("remaining_s", static_cast<int64_t>(secs))
      .set("label", active ? timer_label_ : std::string());
  return v;
}

void App::timer_tick() {
  const uint32_t t = now();
  if (timer_running_ && static_cast<int32_t>(t - timer_end_) >= 0) {
    timer_running_ = false;
    timer_due_ = true;
    json::Value data = json::Value::object();
    data.set("label", timer_label_);
    emit_event("timer.done", data, false);  // the device shows the alarm; Hermes is not interrupted
  }
  // The alarm waits for a quiet moment: never beep into an open microphone, and let
  // a question from Hermes, a Wi-Fi setup, a pairing or a firmware update go first.
  if (timer_due_ && mode_ != Mode::Listening && !prompt_showing() && !wifi_setup_open() && !phone_pairing_ &&
      !ota_busy() && ota_ != Ota::Restarting) {
    timer_due_ = false;
    start_alarm();
  }
  if (!alarm_ringing_) return;
  if (model_.screen != Screen::Alarm) {  // covered by a screen that takes the buttons (Wi-Fi setup, settings)
    alarm_ringing_ = false;
    return;
  }
  // Signed ages: start_alarm() stamps ring_since_ after `t` was read (sending timer.done takes
  // time), and a negative age must neither look like the 60 s cap nor silence the beep.
  if (static_cast<int32_t>(t - ring_since_) >= static_cast<int32_t>(kRingMaxMs)) {
    alarm_ringing_ = false;  // quiet now; the alarm stays until dismissed
  } else if (static_cast<int32_t>(t - next_beep_at_) >= 0) {
    next_beep_at_ = t + kBeepCycleMs;
    beep_pattern();
  }
}

void App::start_alarm() {
  if (settings_open()) close_settings();
  dismiss_overlay();  // a card, image or list under the alarm would take the touches
  stop_playback();    // Hermes speech stops: there is no mixer, and the beep would cut it anyway
  alarm_ = alarm_ringing_ = true;
  ring_since_ = next_beep_at_ = now();
  wake_display();
  update_model();
}

void App::dismiss_alarm() {
  // While it rings, the speaker plays only our beeps: start_alarm() stopped Hermes, and a Hermes
  // stream that starts later ends the ringing (h_audio_start). So stop the speaker only then,
  // and never cut a reply that is still draining after its stream ended.
  const bool beeping = alarm_ringing_ && in_stream_ < 0 && hal_.speaker && hal_.speaker->busy();
  alarm_ = alarm_ringing_ = false;
  if (beeping) hal_.speaker->abort();
  if (ui_) ui_->invalidate();  // the alarm painted the whole panel
  update_model();
}

bool App::alarm_flash() const {
  // The first half of each cycle is green, in step with the double beep that starts it.
  return alarm_ringing_ && static_cast<int32_t>(next_beep_at_ - now()) > static_cast<int32_t>(kBeepCycleMs / 2);
}

void App::beep_pattern() {
  if (!hal_.speaker || in_stream_ >= 0) return;
  const uint32_t rate = profile_.speaker_rate;
  if (beep_pcm_.empty()) {
    // Two 120 ms beeps at 880 Hz, 80 ms apart, after the lead-in every short sound has: each
    // double beep is a stream of its own, and the amplifier comes up with each one.
    static constexpr Tone kBeeps[] = {{880, 120, 80, 9000}, {880, 120, 0, 9000}};
    beep_pcm_ = synth_tones(kBeeps, 2, rate, kToneLeadMs);
  }
  if (!hal_.speaker->begin(rate)) return;
  hal_.speaker->write(beep_pcm_.data(), beep_pcm_.size());
  hal_.speaker->end();
}

}  // namespace hg
