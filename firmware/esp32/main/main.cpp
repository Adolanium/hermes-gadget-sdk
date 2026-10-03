// Hermes Gadget firmware entry point: wires the ESP32 drivers to the portable
// core (hg::App) and runs the app loop on the main task.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "hg.main";

hgp::EspSystem g_system;
hgp::NvsStorage g_storage;
hgp::WsTransport g_transport;
hgp::SpiDisplay g_display;
hgp::I2sMic g_mic;
hgp::I2sSpeaker g_speaker;
hgp::Buttons g_buttons;
hgp::Wifi g_wifi;

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
  }
}

}  // namespace

extern "C" void app_main(void) {
  init_nvs();
  hgp::events::init();
  ESP_ERROR_CHECK(g_storage.begin() ? ESP_OK : ESP_FAIL);
  const hgp::BoardConfig& board = hgp::board_config();
  ESP_LOGI(TAG, "Hermes Gadget %s on %s", CONFIG_HG_FIRMWARE_VERSION, board.name);

  // Wi-Fi first: the radio is the entropy source for the device key.
  g_wifi.begin(g_storage);

  hg::Hal hal;
  hal.system = &g_system;
  hal.transport = &g_transport;
  hal.storage = &g_storage;
  if (board.lcd.enabled && g_display.begin(board.lcd)) hal.display = &g_display;
  if (board.mic.enabled && g_mic.begin(board.mic)) hal.mic = &g_mic;
  if (board.speaker.enabled && g_speaker.begin(board.speaker)) hal.speaker = &g_speaker;
  g_buttons.begin(board.buttons);

  hg::DeviceProfile profile;
  profile.board = board.name;
  profile.firmware = CONFIG_HG_FIRMWARE_VERSION;
  profile.default_name = CONFIG_HG_DEFAULT_NAME;
  profile.default_server_url = CONFIG_HG_DEFAULT_SERVER_URL;
  profile.default_access_token = CONFIG_HG_DEFAULT_ACCESS_TOKEN;
  profile.has_cancel_button = board.buttons.cancel >= 0;
  profile.has_scroll_buttons = board.buttons.up >= 0 && board.buttons.down >= 0;
  profile.talk_label = board.talk_label;
  profile.cancel_label = board.cancel_label;

  static hg::App app(hal, profile);
  add_status_led(app, board.status_led);
  app.on_setting_changed = [](std::string_view key) {
    if (key == "wifi_ssid" || key == "wifi_pass") g_wifi.reconfigure();
  };
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
    app.tick();
  }
}
