// Hermes Gadget firmware entry point: wires the ESP32 drivers to the portable
// core (hg::App) and runs the app loop on the main task.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "hg/touch.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "hg.main";

hgp::EspSystem g_system;
hgp::NvsStorage g_storage;
hgp::WsTransport g_transport;
hgp::SpiDisplay g_display;
hgp::AmoledDisplay g_amoled;
hgp::I2sMic g_mic;
hgp::I2sSpeaker g_speaker;
hgp::CodecAudio g_codec;
hgp::CodecMic g_codec_mic;
hgp::CodecSpeaker g_codec_speaker;
hgp::Buttons g_buttons;
hgp::TouchInput g_touch;
hgp::Wifi g_wifi;
hgp::EspUpdater g_updater;
hg::TouchGestures* g_gestures = nullptr;

// touch_cancel: which inputs act as CANCEL on touch boards.
//   both (default)  swipe down on the screen, and the PWR key
//   swipe           only the swipe;  pwr  only the PWR key
bool g_swipe_cancel = true;
bool g_key_cancel = true;

void apply_touch_cancel() {
  auto v = g_storage.get("touch_cancel");
  std::string mode = v ? *v : "both";
  g_swipe_cancel = mode != "pwr";
  g_key_cancel = mode != "swipe";
  if (g_gestures) g_gestures->set_swipe_cancel(g_swipe_cancel);
}

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// Example device action: a plain status LED the agent can switch with "led.set".
void add_status_led(hg::App& app, int gpio) {
  if (gpio < 0) return;
  gpio_reset_pin(static_cast<gpio_num_t>(gpio));
  gpio_set_direction(static_cast<gpio_num_t>(gpio), GPIO_MODE_OUTPUT);
  hg::Action led;
  led.name = "led.set";
  led.description = "Turn the gadget's status LED on or off.";
  hg::json::Value props = hg::json::Value::object();
  hg::json::Value color = hg::json::Value::object();
  color.set("type", "string").set("description", "'off' turns it off; any other value turns it on");
  props.set("color", color);
  hg::json::Value required = hg::json::Value::array();
  required.push("color");
  led.params.set("type", "object").set("properties", props).set("required", required);
  led.handler = [gpio](const hg::json::Value& args, hg::json::Value& result, std::string& error) {
    const std::string& c = args["color"].as_string();
    if (c.empty()) {
      error = "color is required";
      return false;
    }
    bool on = c != "off";
    gpio_set_level(static_cast<gpio_num_t>(gpio), on ? 1 : 0);
    result.set("on", on);
    return true;
  };
  app.add_action(std::move(led));
}

void dispatch(hg::App& app, hgp::Event& ev) {
  using hgp::EventType;
  const char* text = reinterpret_cast<const char*>(ev.data);
  // Frames from a connection the app has already abandoned are dropped.
  bool stale = (ev.type == EventType::WsOpen || ev.type == EventType::WsText || ev.type == EventType::WsBinary ||
                ev.type == EventType::WsClosed) &&
               ev.generation != g_transport.generation();
  if (stale) return;
  switch (ev.type) {
    case EventType::NetUp: app.on_network(true, text ? text : ""); break;
    case EventType::NetDown: app.on_network(false, text ? text : ""); break;
    case EventType::WsOpen: app.on_transport_open(); break;
    case EventType::WsText: app.on_transport_text(std::string_view(text, ev.len)); break;
    case EventType::WsBinary: app.on_transport_binary(ev.data, ev.len); break;
    case EventType::WsClosed: app.on_transport_closed(text ? text : "closed"); break;
    case EventType::Mic:
      app.on_mic_samples(reinterpret_cast<const int16_t*>(ev.data), ev.len / sizeof(int16_t));
      break;
    case EventType::Console:
      ev.console->reply = app.console(std::string_view(text ? text : "", ev.len));
      xSemaphoreGive(ev.console->done);
      break;
    case EventType::Touch:
      if (g_gestures && ev.len == sizeof(hgp::TouchSample)) {
        const auto* t = reinterpret_cast<const hgp::TouchSample*>(ev.data);
        g_gestures->update(t->touching, t->x, t->y, g_system.now_ms());
      }
      break;
    case EventType::Key:
      if (g_key_cancel && ev.len == sizeof(hgp::KeySample)) {
        app.on_button(hg::Button::Cancel, reinterpret_cast<const hgp::KeySample*>(ev.data)->pressed);
      }
      break;
  }
}

}  // namespace

extern "C" void app_main(void) {
  hgp::diag::begin();  // first, so `diag log` has the whole boot
  init_nvs();
  hgp::events::init();
  ESP_ERROR_CHECK(g_storage.begin() ? ESP_OK : ESP_FAIL);
  const hgp::BoardConfig& board = hgp::board_config();
  const char* version = esp_app_get_description()->version;
  ESP_LOGI(TAG, "Hermes Gadget %s on %s", version, board.name);
  g_updater.start();  // a new firmware on probation starts its clock now

  // Wi-Fi first: the radio is the entropy source for the device key.
  g_wifi.begin(g_storage);

  hg::Hal hal;
  hal.system = &g_system;
  hal.transport = &g_transport;
  hal.storage = &g_storage;
  if (g_updater.capacity()) hal.updater = &g_updater;
  if (board.lcd.enabled && g_display.begin(board.lcd)) hal.display = &g_display;
  else if (board.amoled.enabled && g_amoled.begin(board.amoled)) hal.display = &g_amoled;
  if (board.mic.enabled && g_mic.begin(board.mic)) hal.mic = &g_mic;
  if (board.speaker.enabled && g_speaker.begin(board.speaker)) hal.speaker = &g_speaker;
  i2c_master_bus_handle_t i2c_bus = hgp::i2c::bus(board.i2c);
  if (board.codec.enabled && g_codec.begin(board.codec, i2c_bus)) {
    if (g_codec_mic.begin(g_codec.in())) hal.mic = &g_codec_mic;
    if (g_codec_speaker.begin(g_codec.out())) hal.speaker = &g_codec_speaker;
  }
  g_buttons.begin(board.buttons);
  const bool touch = (board.touch.enabled || board.pwr_key.enabled) &&
                     g_touch.begin(board.touch, board.pwr_key, i2c_bus);

  hgp::diag::Parts parts;
  parts.display = hal.display == &g_display ? "st7789" : hal.display == &g_amoled ? "co5300" : "none";
  parts.mic = hal.mic == &g_codec_mic ? "es7210" : hal.mic == &g_mic ? "i2s" : "none";
  parts.speaker = hal.speaker == &g_codec_speaker ? "es8311" : hal.speaker == &g_speaker ? "i2s" : "none";
  parts.touch = touch && g_touch.has_touch();
  parts.key = touch && g_touch.has_key();
  parts.i2c = i2c_bus;
  hgp::diag::set_parts(parts);
  hgp::diag::log_boot_summary();

  hg::DeviceProfile profile;
  profile.board = board.name;
  profile.firmware = version;
  profile.default_name = CONFIG_HG_DEFAULT_NAME;
  profile.default_server_url = CONFIG_HG_DEFAULT_SERVER_URL;
  profile.default_access_token = CONFIG_HG_DEFAULT_ACCESS_TOKEN;
  profile.has_cancel_button = board.buttons.cancel >= 0 || touch;
  profile.has_scroll_buttons = board.buttons.up >= 0 && board.buttons.down >= 0;
  profile.talk_label = board.talk_label;
  profile.cancel_label = board.cancel_label;
  if (touch && board.touch.enabled) {
    profile.touch_screen = true;
    profile.extra_settings = {"touch_cancel"};
  }
  if (hal.mic == &g_codec_mic) profile.mic_rate = hgp::CodecAudio::kRate;
  if (hal.speaker == &g_codec_speaker) profile.speaker_rate = hgp::CodecAudio::kRate;

  static hg::App app(hal, profile);
  static hg::TouchGestures gestures(app);
  if (profile.touch_screen) g_gestures = &gestures;
  apply_touch_cancel();
  add_status_led(app, board.status_led);
  app.on_setting_changed = [](std::string_view key) {
    if (key == "wifi_ssid" || key == "wifi_pass") g_wifi.reconfigure();
    if (key == "touch_cancel") apply_touch_cancel();
  };
  app.on_diag = [](hg::json::Value& report) {
    hgp::diag::report(report);
    report.set("ota", g_updater.describe());
  };
  app.recent_log = &hgp::diag::recent_log;
  app.begin();
  hgp::console::begin();

  for (;;) {
    hgp::Event ev;
    // Block briefly for events, then run the core's timers and animations.
    if (hgp::events::receive(ev, pdMS_TO_TICKS(10))) {
      do {
        dispatch(app, ev);
        hgp::events::release(ev);
      } while (hgp::events::receive(ev, 0));
    }
    g_buttons.poll(app);
    if (g_gestures) g_gestures->tick(g_system.now_ms());
    app.tick();
  }
}
