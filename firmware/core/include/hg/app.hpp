// The device application: connection lifecycle, authentication, pairing,
// push-to-talk, reply display, audio playback, device actions and telemetry.
//
// Single-threaded by design. Ports call tick() frequently (every 5-20 ms) and
// deliver every asynchronous event through the on_* methods on that same
// thread. Nothing here blocks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hg/crypto.hpp"
#include "hg/frame_stats.hpp"
#include "hg/hal.hpp"
#include "hg/i18n.hpp"
#include "hg/json.hpp"
#include "hg/ui.hpp"
#include "hg/vad.hpp"

namespace hg {

enum class Button : uint8_t { Talk, Cancel, Up, Down };

// How the talk button behaves.
enum class TalkMode : uint8_t {
  Hold,  // push-to-talk: speak while held
  Tap,   // tap to start, tap again (or pause) to send
};

// Static facts the port knows about the board and build.
struct DeviceProfile {
  std::string board = "unknown";
  std::string firmware = "0.0.0";
  std::string default_name = "Hermes Gadget";
  std::string default_server_url;
  std::string default_access_token;
  uint32_t mic_rate = 16000;
  float vad_min_rms = 500.0f;  // tap mode: the quietest block that counts as speech (a quiet mic wants less)
  uint32_t speaker_rate = 16000;
  bool has_cancel_button = true;
  bool has_scroll_buttons = false;
  std::string talk_label = "TALK";
  std::string cancel_label = "CANCEL";
  // The screen stands in for the buttons (hold to talk, tap to answer yes,
  // swipe to cancel); on-screen hints are worded for touch.
  bool touch_screen = false;
  // The touchscreen's pixels per inch, so tap targets are about 7 mm tall; 0 assumes kDefaultTouchPpi.
  uint16_t touch_ppi = 0;
  static constexpr uint16_t kDefaultTouchPpi = 200;
  // A motion sensor feeds App::on_motion 5-25 times a second. Boards with one
  // and a backlit display can sleep while lying face down (face_down_sleep).
  bool motion_sensor = false;
  // Board-specific settings the console accepts besides the core ones. Changes
  // reach the port through App::on_setting_changed.
  std::vector<std::string> extra_settings;
  // Frame timing (hg::FrameStats, the console's `perf` command): on from boot,
  // the frame budget, and how often a summary goes to the log (0: never).
  bool frame_stats = false;
  uint32_t frame_budget_us = FrameStats::kDefaultBudgetUs;
  uint32_t frame_log_ms = 10000;
  // Room for a face from Hermes (docs/protocol.md#faces): about 27 KB, and its message at once.
  // Boards without PSRAM don't offer faces.
  bool faces = true;
};

// A device-side capability the agent may invoke. `params` is a JSON-schema
// object describing the arguments; `description` is written for the model.
struct Action {
  std::string name;
  std::string description;
  json::Value params = json::Value::object();
  // Return true on success and fill `result`; on failure set `error`.
  std::function<bool(const json::Value& args, json::Value& result, std::string& error)> handler;
};

// The temporary Wi-Fi setup network, as the port knows it. Lets the setup
// screen show a scannable code beside the credentials.
struct WifiSetupAp {
  std::string ssid;
  std::string password;
};

class App {
 public:
  App(Hal& hal, DeviceProfile profile);

  void begin();
  void tick();

  // --- events from the port -------------------------------------------------
  void on_network(bool up, std::string_view detail = {});
  void on_transport_open();
  void on_transport_text(std::string_view text);
  void on_transport_binary(const uint8_t* data, size_t len);
  void on_transport_closed(std::string_view reason);
  void on_button(Button button, bool pressed);
  // Accelerometer sample in the device frame, in milli-g: +x towards the
  // picture's right, +y towards its top, +z out of the screen. Called 5-25
  // times a second from the thread that calls tick().
  void on_motion(int16_t x_mg, int16_t y_mg, int16_t z_mg);
  void on_mic_samples(const int16_t* samples, size_t count);

  // --- device features ------------------------------------------------------
  // Sends typed text as a user message (keyboards, simulator, console).
  void submit_text(std::string_view text);
  // Records a sensor reading; reported to Hermes (rate limited).
  void set_sensor(std::string_view name, double value);
  // Reports a device event. With `notify_agent` the event is delivered to the
  // agent as a message, otherwise it is only recorded on the host.
  void emit_event(std::string_view name, json::Value data, bool notify_agent);
  // Registers an action. Call before begin() so it is part of the hello.
  void add_action(Action action);
  // Local settings and hardware checks. No microphone samples leave the device.
  bool open_settings();
  void close_settings();
  bool settings_open() const { return menu_ != Menu::Closed; }
  bool settings_title_hit(int x, int y) const;
  // Hermes profiles: which one answers this device. Switching asks the server; the
  // device keeps a profile only once Hermes confirms it, and asks again only after an answer.
  bool next_agent_profile();
  bool select_agent_profile(size_t index);
  // Touch: a quick tap on the name under the mascot opens the list of profiles;
  // a tap on a row asks for that profile; CANCEL (swipe down) closes it.
  bool profile_name_hit(int x, int y) const;
  bool open_profile_picker();
  bool profile_picker_open() const { return overlay_ == Overlay::Profiles; }
  bool profile_switch_pending() const { return !profile_pending_.empty(); }
  static constexpr uint32_t kProfileReplyMs = 15000;
  int profile_row_hit(int x, int y) const;
  void profile_tap(int x, int y);
  const std::string& agent_profile() const { return agent_profile_; }
  // The resting clock face is up: the time, the date and the mascot.
  bool clock_showing() const { return clock_showing_; }
  std::string agent_profile_name() const;
  // Returns true when this input only wakes a sleeping display.
  bool wake_display();
  // A short press of the PMIC's power key: turns the screen off, or back on.
  // Wi-Fi and the Hermes connection stay up.
  void on_power_key();
  // Dozing (System::set_dozing): the port's loop may wait this long for events.
  static constexpr uint32_t kDozeWaitMs = 100;
  bool dozing() const { return dozing_; }
  bool start_wifi_setup();
  void close_wifi_setup();
  bool wifi_setup_open() const { return !wifi_setup_text_.empty(); }
  // A turn is under way: listening, waiting for a reply, or responding, which lasts until the
  // reply has finished playing. The port keeps Wi-Fi at full power until it ends.
  bool in_turn() const { return mode_ != Mode::Idle; }
  // These run on the app task. Start returns private, on-screen instructions.
  std::function<std::string()> on_wifi_setup;
  std::function<void()> on_wifi_setup_close;
  // Optional: the temporary network's name and password, so the setup screen can
  // show a QR code. The port sets this only where it has the values.
  std::function<WifiSetupAp()> on_wifi_setup_ap;

  // A phone companion app over Bluetooth, on boards whose port sets the hooks
  // below. Pairing opens for kPhonePairMs from the settings menu or `phone pair`;
  // the phone types the passkey this screen shows. A paired phone sends console
  // lines (App::console) and reads the replies.
  static constexpr uint32_t kPhonePairMs = 120000;
  bool start_phone_pairing();
  void close_phone_pairing();
  bool phone_pairing_open() const { return phone_pairing_; }
  // From the port: the passkey to show, the pairing's result, and the number of
  // paired phones and whether one is connected.
  void on_phone_passkey(uint32_t passkey);
  void on_phone_paired(bool ok);
  void on_phone_status(int paired, bool connected);
  // These run on the app task. on_phone_pair returns false when Bluetooth can't start.
  std::function<bool()> on_phone_pair;
  std::function<void()> on_phone_pair_close;
  std::function<void()> on_phone_forget;  // removes every paired phone

  // Serial-console command (provisioning, bench automation). Returns the
  // response; machine-readable lines start with '@'.
  std::string console(std::string_view line);
  // Invoked after a setting changes through the console (e.g. Wi-Fi keys).
  // The `wifi` command reports a changed network list as "wifi_nets".
  std::function<void(std::string_view key)> on_setting_changed;
  // The name of the Wi-Fi network the device is on, or empty; for `wifi list`.
  std::function<std::string()> current_wifi;
  // Adds the port's facts to the console's `diag` report: memory, reset reason,
  // radio, which drivers came up.
  std::function<void(json::Value& report)> on_diag;
  // Recent log lines for `diag log`, oldest first. Ports that keep none leave it unset.
  std::function<std::string()> recent_log;

  // --- introspection --------------------------------------------------------
  Screen screen() const { return model_.screen; }
  const UiModel& model() const { return model_; }
  const std::string& device_id() const { return device_id_; }
  bool paired() const { return paired_; }
  bool online() const { return phase_ == Phase::Online; }
  // The resting clock is on (`set clock` above 0). The port runs network time
  // only then.
  bool clock_enabled() const { return clock_after_ms_ > 0; }
  std::string status_json() const;

 private:
  enum class Phase : uint8_t { Boot, NoNetwork, Connecting, Handshake, Online };
  enum class Mode : uint8_t { Idle, Listening, Thinking, Responding };

  using Handler = void (App::*)(const json::Value&);
  struct Route {
    const char* type;
    Handler handler;
  };
  static const Route kRoutes[];

  // server message handlers
  void h_challenge(const json::Value& m);
  void h_welcome(const json::Value& m);
  void h_pairing(const json::Value& m);
  void h_paired(const json::Value& m);
  void h_unpaired(const json::Value& m);
  void h_turn_start(const json::Value& m);
  void h_turn_end(const json::Value& m);
  void h_status(const json::Value& m);
  void h_transcript(const json::Value& m);
  void h_reply_delta(const json::Value& m);
  void h_reply(const json::Value& m);
  void h_audio_start(const json::Value& m);
  void h_audio_end(const json::Value& m);
  void h_audio_abort(const json::Value& m);
  void h_display(const json::Value& m);
  void h_image_start(const json::Value& m);
  void h_image_end(const json::Value& m);
  void h_action(const json::Value& m);
  void h_ping(const json::Value& m);
  void h_notice(const json::Value& m);
  void h_error(const json::Value& m);
  void h_prompt(const json::Value& m);
  void h_prompt_close(const json::Value& m);
  void h_dashboard(const json::Value& m);
  void h_ota_offer(const json::Value& m);
  void h_ota_begin(const json::Value& m);
  void h_ota_end(const json::Value& m);
  void h_ota_abort(const json::Value& m);
  void h_profile(const json::Value& m);
  void h_face(const json::Value& m);
  bool can_switch_profile() const;
  void set_agent_profile(const std::string& id);
  void profile_notice(std::string text);

  // firmware updates
  void ota_chunk(uint8_t stream, uint16_t seq, const uint8_t* data, size_t len);
  // Abandons the update (if any) and tells the server why.
  void ota_fail(std::string_view code, std::string_view message);
  void ota_reset();
  bool ota_busy() const;

  void load_settings();
  void settings_input(Button button, bool pressed);
  void settings_tick();
  void settings_model();
  void stop_hardware_check();
  void power_tick();
  // The one writer of the panel's backlight level (App::apply_backlight).
  void apply_backlight();
  // What keeps the panel busy for the user; the screen timeout and the resting
  // clock share it so the two lists cannot drift.
  bool screen_busy() const;
  // Face-down sleep (app_power.cpp): dark and quiet while the gadget lies on
  // its screen, ended only by turning it back over.
  bool can_sleep_face_down() const;
  bool face_down_sleep_enabled() const;
  void motion_tick();
  void enter_face_down();
  void leave_face_down();
  json::Value power_value() const;
  // The resting clock (app_power.cpp): it shows on an online, paired Ready
  // screen once `clock_after_ms_` of rest has passed without anything on it.
  bool clock_blocked() const;
  void clock_tick();
  json::Value status_value() const;
  std::string diag_report();
  // frame timing (app_perf.cpp)
  std::string perf_command(std::string_view args);
  void set_frame_stats(bool on);
  void perf_tick();
  void bench_step();
  // Every render of the app's own model goes through here; a bench holds the screen.
  void render(const UiModel& m);
  std::string setting(std::string_view key, std::string_view fallback = {}) const;
  void connect_now();
  void schedule_reconnect();
  void drop_session(std::string_view reason);
  bool send(const json::Value& msg);
  void send_hello();
  bool can_talk() const;
  void start_listening(bool hands_free);
  void finish_listening();
  void cancel_listening(std::string_view why);
  void stop_playback();
  void cancel_turn();
  bool known_setting(std::string_view key) const;
  std::string wifi_command(std::string_view args);
  void start_new_session();
  void scroll_body(int delta);
  // Long text pages itself, so boards without scroll buttons can read it all.
  bool turn_page();
  uint32_t page_dwell_ms() const;
  void dismiss_overlay();
  // A question from Hermes is on screen (it waits while the user is recording).
  bool prompt_showing() const;
  // Holding the title (or round settings target) would open or close settings.
  bool settings_hold_live() const;
  // True when a press that started at `pressed_at` may answer the question.
  bool prompt_armed(uint32_t pressed_at) const;
  void answer_prompt(bool yes);
  void clear_prompt();
  void flush_sensors();
  // Tells Hermes when the screen goes dark or lights up (docs/protocol.md#screen-sleep).
  void flush_display_state();
  void set_hint_flash(std::string text);
  // The device's own text in the chosen language (hg/i18n.hpp).
  const char* tr(i18n::Msg id) const { return i18n::text(lang_, id); }
  std::string tr(i18n::Msg id, std::initializer_list<std::string_view> args) const {
    return i18n::format(tr(id), args);
  }
  // A board's control name (DeviceProfile talk_label, cancel_label) in that language.
  std::string control(const std::string& label) const { return i18n::control_name(lang_, label); }
  void update_model();
  void log(LogLevel level, std::string_view msg);
  uint32_t now() const { return hal_.system->now_ms(); }
  bool speaking() const;
  std::string next_id(char prefix);
  // timers (app_timer.cpp)
  void add_timer_actions();
  void timer_tick();
  bool timer_active() const;
  uint32_t timer_left_ms() const;
  int16_t timer_permille() const;       // thousandths of the total left; -1 when no timer runs
  std::string timer_left_text() const;  // m:ss, or h:mm:ss from an hour
  json::Value timer_value() const;      // the `timer` object of status and timer.status
  void start_alarm();
  void dismiss_alarm();
  bool alarm_flash() const;
  void beep_pattern();

  Hal& hal_;
  DeviceProfile profile_;
  std::unique_ptr<Ui> ui_;
  UiModel model_;

  // identity & settings
  std::vector<uint8_t> key_;
  std::string device_id_;
  std::string name_;
  std::string server_url_;
  std::string access_token_;
  TalkMode talk_mode_ = TalkMode::Hold;
  uint8_t volume_ = 70;
  uint8_t brightness_ = 100;
  i18n::Lang lang_ = i18n::Lang::En;
  struct AgentProfile {
    std::string id, name;
  };
  std::vector<AgentProfile> agent_profiles_;  // offered in welcome; empty = no switching
  std::string agent_profile_ = "default";
  std::optional<mascot::Face> face_;  // agent_profile_'s, from Hermes; kept through reconnects
  // A profile.select awaiting Hermes's answer; one at a time (empty: none).
  std::string profile_pending_;
  uint32_t profile_pending_at_ = 0;
  int profile_page_ = 0;  // first profile on the list's page
  // `language` is auto: follow the language Hermes sends in `welcome` (English until it does).
  bool lang_auto_ = false;
  std::optional<i18n::Lang> hermes_lang_;  // from the last `welcome` that named one this firmware has
  enum class Menu : uint8_t { Closed, Volume, Brightness, TalkMode, Profile, Microphone, Speaker, Display, Inputs,
                              Info, Power, IdleTimer, FaceDownSleep, Clock, PowerOff, WifiSetup, Phone, Language, Back };
  enum class HardwareCheck : uint8_t { None, Microphone, Speaker, Display, Inputs };
  Menu menu_ = Menu::Closed;
  HardwareCheck hardware_check_ = HardwareCheck::None;
  std::string check_result_;
  std::string wifi_setup_text_;
  // The setup network's scannable code, encoded once when the screen opens.
  qr::Code wifi_setup_qr_;
  // phone pairing (on_phone_pair)
  bool phone_pairing_ = false;
  uint32_t phone_pair_until_ = 0;
  std::string phone_passkey_;
  int phones_paired_ = 0;
  bool phone_connected_ = false;
  void phone_tick();
  json::Value phone_value() const;
  // Closes the Wi-Fi setup and phone pairing screens, for what takes the screen over.
  void close_setup_screens();
  bool talk_held_ = false;
  bool settings_chord_fired_ = false;
  uint32_t talk_down_at_ = 0;
  std::optional<PowerStatus> power_status_;
  uint32_t power_read_at_ = 0, activity_at_ = 0, power_key_polled_at_ = 0;
  uint32_t screen_timeout_ms_ = 0;
  uint32_t clock_after_ms_ = 0;  // the `clock` setting: rest before the clock face, 0 = off
  bool clock_24h_ = true;        // the `time_format` setting
  bool clock_showing_ = false;   // the clock face is up (App::clock_tick)
  uint32_t clock_rest_at_ = 0;   // when the last block on the clock ended; rest counts from the later of this and activity_at_
  bool wall_valid_ = false;      // the port knows the time of day (Hal::System::wall_time)
  WallTime wall_{};              // the last reading behind the clock face
  // What the battery indicator shows, in the order a tap cycles through.
  enum class BatteryIndicator : uint8_t { Icon, Percentage, Both, Off };
  BatteryIndicator battery_display_ = BatteryIndicator::Off;
  bool battery_estimate_enabled_ = false;
  std::optional<uint8_t> display_battery_percent() const;
  bool battery_is_estimated() const;
  // Whether a native or explicitly enabled display percentage is available.
  bool battery_readable() const;
  bool display_dimmed_ = false, display_sleeping_ = false, power_off_armed_ = false;
  bool dozing_ = false, on_battery_ = false;
  void set_dozing(bool dozing);
  // Face-down sleep. Stamps are 0 until set (now() | 1 keeps 0 meaning "none").
  int16_t motion_[3] = {0, 0, 0};
  uint32_t motion_at_ = 0, down_since_ = 0, up_since_ = 0;
  bool motion_seen_ = false, face_down_ = false;
  bool face_down_sleep_ = false;      // the `face_down_sleep` setting
  bool reply_while_face_down_ = false;  // a reply arrived while dark; show it on the turn-over
  uint8_t wake_buttons_ = 0;

  // connection
  Phase phase_ = Phase::Boot;
  bool network_up_ = false;
  bool transport_active_ = false;  // a connect attempt or session is live
  std::string network_detail_;
  uint32_t boot_until_ = 0;
  uint32_t reconnect_at_ = 0;
  bool reconnect_pending_ = false;
  uint32_t phase_since_ = 0;
  uint32_t online_since_ = 0;
  uint32_t last_rx_ = 0;
  uint32_t heartbeat_ms_ = 20000;
  int backoff_ = 0;
  int attempts_ = 0;
  std::string fatal_;   // unrecoverable until reconfigured
  std::string last_close_;

  // pairing
  bool paired_ = false;
  std::string pairing_code_;
  std::string pairing_command_;

  // conversation
  Mode mode_ = Mode::Idle;
  uint32_t mode_since_ = 0;
  bool hands_free_ = false;
  uint8_t mic_stream_ = 0;
  uint32_t mic_unsent_ = 0;  // frames of this recording the transport refused
  uint16_t mic_seq_ = 0;
  std::string request_id_;
  std::string active_turn_;
  bool turn_done_ = false;
  bool reply_final_ = false;
  uint32_t last_turn_rx_ = 0;
  std::string reply_;
  static constexpr uint32_t kReplyLingerMs = 20000;  // reply text stays up this long before the mascot returns
  uint32_t reply_until_ = 0;  // reply text shown on the Ready screen until then
  bool cancel_held_ = false;
  bool cancel_long_fired_ = false;
  uint32_t cancel_down_at_ = 0;
  std::string status_;
  std::string user_echo_;
  int scroll_ = -1;
  uint32_t page_at_ = 0;  // when long text turns its next page
  uint8_t level_ = 0;
  Vad vad_;
  std::vector<uint8_t> frame_buf_;
  std::vector<int16_t> pcm_buf_;
  int in_stream_ = -1;
  bool in_decoded_ = false;  // the current speaker stream arrives compressed (hal_.speaker_decoder)
  uint32_t in_rate_ = 0;     // the current speaker stream's sample rate
  uint32_t id_counter_ = 0;

  // overlays
  enum class Overlay : uint8_t { None, Card, Image, Dashboard, Profiles };
  Overlay overlay_ = Overlay::None;
  uint32_t overlay_until_ = 0;  // 0 = until dismissed
  std::string card_title_, card_body_;
  int card_scroll_ = 0;
  int image_stream_ = -1;
  int image_w_ = 0, image_h_ = 0, image_x_ = 0, image_y_ = 0;
  size_t image_px_ = 0;

  // the dashboard: Hermes's sessions and the user's tiles, as last sent. Opened
  // from the idle screen with CANCEL (a swipe down on touch boards).
  bool dashboard_ready_ = false;  // a dashboard arrived on this connection
  std::vector<DashRow> dash_rows_;
  std::string dash_headline_;
  int dash_scroll_ = 0;
  bool can_open_dashboard() const { return profile_.has_cancel_button || profile_.touch_screen; }

  // a yes/no question from Hermes (confirmations, command approvals)
  std::string prompt_id_, prompt_title_, prompt_text_;
  uint32_t prompt_since_ = 0;
  uint32_t prompt_until_ = 0;  // 0 = until answered or withdrawn

  // transient messages
  std::string notice_;
  uint32_t notice_until_ = 0;
  std::string hint_flash_;
  bool hint_countdown_ = false;  // hint_flash_ counts down to a new session
  uint32_t hint_flash_until_ = 0;

  // actions & telemetry
  std::vector<Action> actions_;
  std::vector<std::pair<std::string, double>> sensors_;
  bool sensors_dirty_ = false;
  uint32_t sensors_sent_ = 0;
  bool display_sleep_sent_ = false;  // what Hermes was last told

  // a firmware update: offered (waiting for the server's MAC), receiving, or
  // installed and about to restart
  enum class Ota : uint8_t { Idle, Offered, Receiving, Restarting };
  Ota ota_ = Ota::Idle;
  std::string ota_nonce_, ota_sha_, ota_version_;
  size_t ota_size_ = 0, ota_received_ = 0, ota_acked_ = 0;
  uint8_t ota_stream_ = 0;
  uint16_t ota_seq_ = 0;
  uint32_t ota_last_rx_ = 0, ota_restart_at_ = 0;
  crypto::Sha256 ota_hash_;

  uint32_t frame_at_ = 0;

  // timers (app_timer.cpp): RAM only, one at a time
  bool timer_running_ = false;  // counting down
  bool timer_due_ = false;      // reached zero; the alarm starts once the device is free
  bool alarm_ = false;          // TIMER DONE is on screen
  bool alarm_ringing_ = false;  // beeping, for a minute at most
  uint32_t timer_end_ = 0;      // the clock value at zero
  uint32_t timer_total_ms_ = 0;
  uint32_t ring_since_ = 0;     // ages are signed: see timer_tick()
  uint32_t next_beep_at_ = 0;
  std::string timer_label_;
  std::vector<int16_t> beep_pcm_;  // built on the first beep

  // frame timing: the running statistics while `perf` is on, and the bench
  std::unique_ptr<FrameStats> frame_stats_;
  uint32_t frame_log_at_ = 0, frame_log_frames_ = 0;
  enum class Bench : uint8_t { None, Scroll, Mascot, Redraw };
  Bench bench_ = Bench::None, bench_last_ = Bench::None;  // running; the last one to run of `perf bench all`
  int bench_frame_ = 0;
  FrameStats bench_stats_;
  UiModel bench_model_;
  json::Value bench_results_ = json::Value::object();
};

}  // namespace hg
