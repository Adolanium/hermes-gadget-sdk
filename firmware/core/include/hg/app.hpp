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

#include "hg/hal.hpp"
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
  uint32_t speaker_rate = 16000;
  bool has_cancel_button = true;
  bool has_scroll_buttons = false;
  std::string talk_label = "TALK";
  std::string cancel_label = "CANCEL";
  // The screen stands in for the buttons (hold to talk, tap to answer yes,
  // swipe to cancel); on-screen hints are worded for touch.
  bool touch_screen = false;
  // Board-specific settings the console accepts besides the core ones. Changes
  // reach the port through App::on_setting_changed.
  std::vector<std::string> extra_settings;
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

  // Serial-console command (provisioning, bench automation). Returns the
  // response; machine-readable lines start with '@'.
  std::string console(std::string_view line);
  // Invoked after a setting changes through the console (e.g. Wi-Fi keys).
  std::function<void(std::string_view key)> on_setting_changed;

  // --- introspection --------------------------------------------------------
  Screen screen() const { return model_.screen; }
  const UiModel& model() const { return model_; }
  const std::string& device_id() const { return device_id_; }
  bool paired() const { return paired_; }
  bool online() const { return phase_ == Phase::Online; }
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

  void load_settings();
  std::string setting(std::string_view key, std::string_view fallback = {}) const;
  void connect_now();
  void schedule_reconnect();
  void drop_session(std::string_view reason);
  void send(const json::Value& msg);
  void send_hello();
  bool can_talk() const;
  void start_listening(bool hands_free);
  void finish_listening();
  void cancel_listening(std::string_view why);
  void stop_playback();
  void cancel_turn();
  bool known_setting(std::string_view key) const;
  void start_new_session();
  void scroll_body(int delta);
  // Long text pages itself, so boards without scroll buttons can read it all.
  bool turn_page();
  uint32_t page_dwell_ms() const;
  void dismiss_overlay();
  // A question from Hermes is on screen (it waits while the user is recording).
  bool prompt_showing() const;
  // True when a press that started at `pressed_at` may answer the question.
  bool prompt_armed(uint32_t pressed_at) const;
  void answer_prompt(bool yes);
  void clear_prompt();
  void flush_sensors();
  void set_hint_flash(std::string text);
  void update_model();
  void log(LogLevel level, std::string_view msg);
  uint32_t now() const { return hal_.system->now_ms(); }
  bool speaking() const;
  std::string next_id(char prefix);

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
  uint16_t mic_seq_ = 0;
  std::string request_id_;
  std::string active_turn_;
  bool turn_done_ = false;
  bool reply_final_ = false;
  uint32_t last_turn_rx_ = 0;
  std::string reply_;
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
  uint32_t id_counter_ = 0;

  // overlays
  enum class Overlay : uint8_t { None, Card, Image };
  Overlay overlay_ = Overlay::None;
  uint32_t overlay_until_ = 0;  // 0 = until dismissed
  std::string card_title_, card_body_;
  int card_scroll_ = 0;
  int image_stream_ = -1;
  int image_w_ = 0, image_h_ = 0, image_x_ = 0, image_y_ = 0;
  size_t image_px_ = 0;

  // a yes/no question from Hermes (confirmations, command approvals)
  std::string prompt_id_, prompt_title_, prompt_text_;
  uint32_t prompt_since_ = 0;
  uint32_t prompt_until_ = 0;  // 0 = until answered or withdrawn

  // transient messages
  std::string notice_;
  uint32_t notice_until_ = 0;
  std::string hint_flash_;
  uint32_t hint_flash_until_ = 0;

  // actions & telemetry
  std::vector<Action> actions_;
  std::vector<std::pair<std::string, double>> sensors_;
  bool sensors_dirty_ = false;
  uint32_t sensors_sent_ = 0;

  uint32_t frame_at_ = 0;
};

}  // namespace hg
