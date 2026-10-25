// Simulator HAL: forwards every driver call to host callbacks.
#include "hgsim.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hg/app.hpp"
#include "hg/touch.hpp"

namespace {

class SimPower final : public hg::Power {
 public:
  std::optional<hg::PowerStatus> status;
  std::optional<hg::PowerStatus> read() override { return status; }
  std::optional<bool> external_power() override {
    return status ? status->external_power : std::nullopt;
  }
  bool can_power_off() const override { return false; }
  bool power_off() override { return false; }
};

class SimHal final : public hg::Display,
                     public hg::AudioIn,
                     public hg::AudioOut,
                     public hg::Transport,
                     public hg::Storage,
                     public hg::System {
 public:
  SimHal(const hgsim_config& cfg, const hgsim_host& host)
      : host_(host), fb_(static_cast<size_t>(cfg.width) * static_cast<size_t>(cfg.height), 0) {
    info_.width = static_cast<uint16_t>(cfg.width);
    info_.height = static_cast<uint16_t>(cfg.height);
    info_.swap_bytes = false;
    info_.has_backlight = cfg.has_backlight != 0;
    info_.round = cfg.round != 0;
  }

  // Display
  hg::DisplayInfo info() const override { return info_; }
  uint16_t* framebuffer() override { return fb_.data(); }
  void flush(uint16_t y0, uint16_t y1) override {
    if (host_.display_flush) host_.display_flush(host_.user, y0, y1);
  }
  void set_backlight(uint8_t percent) override {
    if (host_.display_backlight) host_.display_backlight(host_.user, percent);
  }

  // AudioIn
  bool start(uint32_t rate) override { return host_.mic_start && host_.mic_start(host_.user, rate); }
  void stop() override {
    if (host_.mic_stop) host_.mic_stop(host_.user);
  }

  // AudioOut
  bool begin(uint32_t rate) override { return host_.speaker_begin && host_.speaker_begin(host_.user, rate); }
  void write(const int16_t* samples, size_t count) override {
    if (host_.speaker_write) host_.speaker_write(host_.user, samples, count);
  }
  void end() override {
    if (host_.speaker_end) host_.speaker_end(host_.user);
  }
  void abort() override {
    if (host_.speaker_abort) host_.speaker_abort(host_.user);
  }
  bool busy() const override { return host_.speaker_busy && host_.speaker_busy(host_.user); }
  void set_volume(uint8_t percent) override {
    if (host_.speaker_volume) host_.speaker_volume(host_.user, percent);
  }

  // Transport
  void connect(const std::string& url, const std::string& subprotocol) override {
    if (host_.transport_connect) host_.transport_connect(host_.user, url.c_str(), subprotocol.c_str());
  }
  bool send_text(std::string_view text) override {
    return host_.transport_send_text && host_.transport_send_text(host_.user, text.data(), text.size());
  }
  bool send_binary(const uint8_t* data, size_t len) override {
    return host_.transport_send_binary && host_.transport_send_binary(host_.user, data, len);
  }
  void close() override {
    if (host_.transport_close) host_.transport_close(host_.user);
  }

  // Storage
  std::optional<std::string> get(std::string_view key) override {
    if (!host_.storage_get) return std::nullopt;
    std::string k(key);
    char buf[512];
    int n = host_.storage_get(host_.user, k.c_str(), buf, sizeof(buf));
    if (n < 0) return std::nullopt;
    if (static_cast<size_t>(n) >= sizeof(buf)) n = static_cast<int>(sizeof(buf)) - 1;
    return std::string(buf, static_cast<size_t>(n));
  }
  bool set(std::string_view key, std::string_view value) override {
    if (!host_.storage_set) return false;
    host_.storage_set(host_.user, std::string(key).c_str(), std::string(value).c_str());
    return true;
  }
  void erase(std::string_view key) override {
    if (host_.storage_erase) host_.storage_erase(host_.user, std::string(key).c_str());
  }

  // System
  uint32_t now_ms() override { return host_.now_ms ? host_.now_ms(host_.user) : 0; }
  // Frame timing measures the core's real work on this computer, whatever clock
  // the host gives now_ms (a script may run it faster or slower than real time).
  uint64_t now_us() override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
  }
  void random_bytes(uint8_t* out, size_t len) override {
    if (host_.random_bytes) host_.random_bytes(host_.user, out, len);
    else std::memset(out, 0, len);
  }
  void log(hg::LogLevel level, std::string_view message) override {
    if (host_.log) host_.log(host_.user, static_cast<int>(level), std::string(message).c_str());
  }
  bool wall_time(hg::WallTime& out) override {
    hgsim_wall_time w{};
    if (!host_.wall_time || !host_.wall_time(host_.user, &w)) return false;
    if (w.month < 1 || w.month > 12 || w.day < 1 || w.day > 31 || w.weekday < 0 || w.weekday > 6 || w.hour < 0 ||
        w.hour > 23 || w.minute < 0 || w.minute > 59 || w.second < 0 || w.second > 60 || w.year < 1 || w.year > 9999)
      return false;
    out.year = static_cast<uint16_t>(w.year);
    out.month = static_cast<uint8_t>(w.month);
    out.day = static_cast<uint8_t>(w.day);
    out.weekday = static_cast<uint8_t>(w.weekday);
    out.hour = static_cast<uint8_t>(w.hour);
    out.minute = static_cast<uint8_t>(w.minute);
    out.second = static_cast<uint8_t>(std::min(w.second, 59));
    return true;
  }

 private:
  hgsim_host host_;
  hg::DisplayInfo info_;
  std::vector<uint16_t> fb_;
};

// The update slot, apart from SimHal: hg::Updater and hg::AudioOut both have an abort().
class SimUpdater final : public hg::Updater {
 public:
  SimUpdater(const hgsim_config& cfg, const hgsim_host& host)
      : host_(host), capacity_(cfg.update_capacity), pending_(cfg.update_pending != 0) {}

  size_t capacity() const override { return capacity_; }
  bool begin(size_t size, std::string& error) override {
    if (host_.update_begin && host_.update_begin(host_.user, size)) return true;
    error = "the simulator can't take an update";
    return false;
  }
  bool write(const uint8_t* data, size_t len, std::string& error) override {
    if (host_.update_write && host_.update_write(host_.user, data, len)) return true;
    error = "writing the update failed";
    return false;
  }
  bool finish(std::string& error) override {
    if (host_.update_finish && host_.update_finish(host_.user)) return true;
    error = "not an ESP32 app image";
    return false;
  }
  void abort() override {
    if (host_.update_abort) host_.update_abort(host_.user);
  }
  void restart() override {
    if (host_.update_restart) host_.update_restart(host_.user);
  }
  bool pending_verify() const override { return pending_; }
  void confirm() override {
    pending_ = false;
    if (host_.update_confirm) host_.update_confirm(host_.user);
  }

 private:
  hgsim_host host_;
  size_t capacity_;
  bool pending_;
};

int copy_out(const std::string& s, char* out, size_t cap) {
  if (!out || cap == 0) return static_cast<int>(s.size());
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  std::memcpy(out, s.data(), n);
  out[n] = '\0';
  return static_cast<int>(s.size());
}

}  // namespace

struct hgsim {
  std::unique_ptr<SimHal> hal_impl;
  std::unique_ptr<SimUpdater> updater;
  SimPower power;
  hg::Hal hal;
  std::unique_ptr<hg::App> app;
  std::unique_ptr<hg::TouchGestures> touch;

  // touch_cancel: "both" (default) or "swipe" keep swiping down as CANCEL; "pwr" turns it off.
  void apply_touch_setting() {
    if (!touch) return;
    auto v = hal.storage->get("touch_cancel");
    touch->set_swipe_cancel(!v || *v != "pwr");
  }
};

extern "C" {

int hgsim_abi_version(void) { return HGSIM_ABI_VERSION; }

hgsim* hgsim_create(const hgsim_config* cfg, const hgsim_host* host) {
  if (!cfg || !host || cfg->width < 0 || cfg->height < 0 ||
      cfg->width > 2048 || cfg->height > 2048 || (cfg->width == 0) != (cfg->height == 0)) return nullptr;
  auto* sim = new hgsim();
  sim->hal_impl = std::make_unique<SimHal>(*cfg, *host);
  SimHal* h = sim->hal_impl.get();
  sim->hal.system = h;
  sim->hal.transport = h;
  sim->hal.storage = h;
  sim->hal.display = cfg->width ? h : nullptr;
  sim->hal.mic = cfg->has_mic ? h : nullptr;
  sim->hal.speaker = cfg->has_speaker ? h : nullptr;
  sim->hal.power = cfg->has_power ? &sim->power : nullptr;
  if (cfg->update_capacity && host->update_begin) {
    sim->updater = std::make_unique<SimUpdater>(*cfg, *host);
    sim->hal.updater = sim->updater.get();
  }

  hg::DeviceProfile profile;
  profile.board = cfg->board ? cfg->board : "sim";
  profile.firmware = cfg->firmware ? cfg->firmware : "0.0.0-sim";
  if (cfg->default_name) profile.default_name = cfg->default_name;
  if (cfg->default_server_url) profile.default_server_url = cfg->default_server_url;
  if (cfg->default_access_token) profile.default_access_token = cfg->default_access_token;
  if (cfg->mic_rate) profile.mic_rate = cfg->mic_rate;
  if (cfg->speaker_rate) profile.speaker_rate = cfg->speaker_rate;
  profile.has_scroll_buttons = cfg->has_scroll_buttons != 0;
  profile.talk_label = cfg->talk_label ? cfg->talk_label : "SPACE";
  profile.cancel_label = cfg->cancel_label ? cfg->cancel_label : "ESC";
  if (cfg->touch) {
    profile.touch_screen = true;
    if (!cfg->cancel_label) profile.cancel_label = "Swipe down";
    profile.extra_settings = {"touch_cancel"};
  }
  profile.motion_sensor = cfg->motion != 0;
  sim->app = std::make_unique<hg::App>(sim->hal, profile);
  if (cfg->touch) {
    sim->touch = std::make_unique<hg::TouchGestures>(*sim->app);
    sim->app->on_setting_changed = [sim](std::string_view key) {
      if (key == "touch_cancel") sim->apply_touch_setting();
    };
  }
  return sim;
}

void hgsim_destroy(hgsim* sim) { delete sim; }

int hgsim_add_action(hgsim* sim, const char* name, const char* description, const char* params_json,
                     hgsim_action_fn fn, void* user) {
  if (!sim || !name || !fn) return 0;
  hg::Action a;
  a.name = name;
  a.description = description ? description : "";
  if (params_json && *params_json) {
    hg::json::Value params;
    if (!hg::json::parse(params_json, params) || !params.is_object()) return 0;
    a.params = params;
  }
  a.handler = [fn, user](const hg::json::Value& args, hg::json::Value& result, std::string& error) {
    std::string in = args.dump();
    std::vector<char> out(4096, '\0');
    int ok = fn(user, in.c_str(), out.data(), out.size());
    std::string text(out.data());
    if (!ok) {
      error = text.empty() ? "action failed" : text;
      return false;
    }
    hg::json::Value parsed;
    if (!text.empty() && hg::json::parse(text, parsed)) result = parsed;
    return true;
  };
  sim->app->add_action(std::move(a));
  return 1;
}

void hgsim_begin(hgsim* sim) {
  sim->app->begin();
  sim->apply_touch_setting();
}
void hgsim_tick(hgsim* sim) {
  if (sim->touch) sim->touch->tick(sim->hal.system->now_ms());
  sim->app->tick();
}
void hgsim_network(hgsim* sim, int up, const char* detail) { sim->app->on_network(up != 0, detail ? detail : ""); }
void hgsim_transport_open(hgsim* sim) { sim->app->on_transport_open(); }
void hgsim_transport_text(hgsim* sim, const char* data, size_t len) {
  sim->app->on_transport_text(std::string_view(data, len));
}
void hgsim_transport_binary(hgsim* sim, const uint8_t* data, size_t len) {
  sim->app->on_transport_binary(data, len);
}
void hgsim_transport_closed(hgsim* sim, const char* reason) { sim->app->on_transport_closed(reason ? reason : ""); }
void hgsim_button(hgsim* sim, int button, int pressed) {
  if (button < HGSIM_BUTTON_TALK || button > HGSIM_BUTTON_DOWN) return;
  sim->app->on_button(static_cast<hg::Button>(button), pressed != 0);
}
void hgsim_touch(hgsim* sim, int touching, int x, int y) {
  if (sim->touch) sim->touch->update(touching != 0, x, y, sim->hal.system->now_ms());
}
void hgsim_motion(hgsim* sim, int x_mg, int y_mg, int z_mg) {
  auto clamp = [](int v) { return static_cast<int16_t>(v < -16000 ? -16000 : v > 16000 ? 16000 : v); };
  sim->app->on_motion(clamp(x_mg), clamp(y_mg), clamp(z_mg));
}
void hgsim_mic_samples(hgsim* sim, const int16_t* samples, size_t count) { sim->app->on_mic_samples(samples, count); }
void hgsim_submit_text(hgsim* sim, const char* text) { sim->app->submit_text(text ? text : ""); }
void hgsim_set_sensor(hgsim* sim, const char* name, double value) {
  if (name) sim->app->set_sensor(name, value);
}
int hgsim_set_power(hgsim* sim, int available, int battery_present, int battery_mv,
                    int battery_percent, int charging, int external_power) {
  if (!sim || !sim->hal.power || available < 0 || available > 1 ||
      battery_present < -1 || battery_present > 1 || charging < -1 || charging > 1 ||
      external_power < -1 || external_power > 1 || battery_mv < -1 || battery_mv > 65535 ||
      battery_percent < -1 || battery_percent > 100) return 0;
  if (!available) {
    sim->power.status.reset();
    return 1;
  }
  hg::PowerStatus status;
  if (battery_present != -1) status.battery_present = battery_present != 0;
  if (battery_mv != -1) status.battery_mv = static_cast<uint16_t>(battery_mv);
  if (battery_percent != -1) status.battery_percent = static_cast<uint8_t>(battery_percent);
  if (charging != -1) status.charging = charging != 0;
  if (external_power != -1) status.external_power = external_power != 0;
  sim->power.status = status;
  return 1;
}
void hgsim_emit_event(hgsim* sim, const char* name, const char* data_json, int notify_agent) {
  if (!name) return;
  hg::json::Value data = hg::json::Value::object();
  if (data_json && *data_json) hg::json::parse(data_json, data);
  sim->app->emit_event(name, data, notify_agent != 0);
}
int hgsim_console(hgsim* sim, const char* line, char* out, size_t cap) {
  return copy_out(sim->app->console(line ? line : ""), out, cap);
}
int hgsim_status(hgsim* sim, char* out, size_t cap) { return copy_out(sim->app->status_json(), out, cap); }
const char* hgsim_screen(hgsim* sim) { return hg::screen_name(sim->app->screen()); }
const uint16_t* hgsim_framebuffer(hgsim* sim, int* width, int* height) {
  hg::DisplayInfo di = sim->hal_impl->info();
  if (width) *width = di.width;
  if (height) *height = di.height;
  return sim->hal_impl->framebuffer();
}

}  // extern "C"
