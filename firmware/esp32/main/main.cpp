// Hermes Gadget firmware entry point: wires the ESP32 drivers to the portable
// core (hg::App) and runs the app loop on the main task.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_psram.h"
#include "hg/touch.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "hg.main";

hgp::EspSystem g_system;
hgp::NvsStorage g_storage;
hgp::WsTransport g_transport;
hgp::SpiDisplay g_display;
hgp::ParallelDisplay g_parallel;
hgp::RgbDisplay g_rgb;
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
hgp::AxpPower g_power;
hgp::LatchPower g_latch_power;
hgp::CoreS3Board g_cores3;
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

// Light sleep disables every GPIO it doesn't know about; these must keep their
// level so the panel, touch controller, amplifier and buses survive a doze.
void keep_pins_through_sleep(const hgp::BoardConfig& b) {
  const int pins[] = {b.amoled.cs, b.amoled.sclk, b.amoled.d0, b.amoled.d1, b.amoled.d2, b.amoled.d3,
                      b.amoled.rst, b.touch.rst, b.codec.pa, b.i2c.sda, b.i2c.scl};
  for (int pin : pins)
    if (pin >= 0) gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
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
    case EventType::NetUp:
      g_wifi.connected(app);
      app.on_network(true, text ? text : "");
      break;
    case EventType::NetDown:
      app.on_network(false, text ? text : "");
      break;
    case EventType::WifiDisconnected:
      g_wifi.disconnected();
      app.on_network(false, text ? text : "");
      break;
    case EventType::WifiStarted: g_wifi.reconfigure(); break;
    case EventType::WifiProvision:
      if (ev.len == sizeof(hg::WifiCredentials)) g_wifi.provision(*reinterpret_cast<const hg::WifiCredentials*>(ev.data));
      break;
    case EventType::WsOpen: app.on_transport_open(); break;
    case EventType::WsText: app.on_transport_text(std::string_view(text, ev.len)); break;
    case EventType::WsBinary: app.on_transport_binary(ev.data, ev.len); break;
    case EventType::WsClosed: app.on_transport_closed(text ? text : "closed"); break;
    case EventType::Mic:
      app.on_mic_samples(reinterpret_cast<const int16_t*>(ev.data), ev.len / sizeof(int16_t));
      break;
    case EventType::Console:
      ev.console->answer(app.console(std::string_view(text ? text : "", ev.len)));
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
    case EventType::Encoder:
      if (ev.len == sizeof(hgp::EncoderSample)) {
        // A detent is a press and a release, so turning keeps scrolling after
        // the first detent wakes the screen.
        const auto* encoder = reinterpret_cast<const hgp::EncoderSample*>(ev.data);
        const hg::Button button = encoder->direction > 0 ? hg::Button::Up : hg::Button::Down;
        app.on_button(button, true);
        app.on_button(button, false);
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
  const bool latch_power = board.latch_power.enabled && g_latch_power.begin(board.latch_power);
  const char* version = esp_app_get_description()->version;
  ESP_LOGI(TAG, "Hermes Gadget %s on %s", version, board.name);
#if CONFIG_SPIRAM
#if CONFIG_SPIRAM_MODE_OCT
  constexpr const char* kPsramMode = "octal";
#else
  constexpr const char* kPsramMode = "quad";
#endif
  if (!esp_psram_is_initialized()) {
    ESP_LOGE(TAG, "no PSRAM found: this firmware is built for a module with %s PSRAM, and the display may "
                  "not start without it (see the board's requirements in docs/hardware.md)",
             kPsramMode);
  }
#endif
#if CONFIG_PM_ENABLE
  // Boards that enable power management let the CPU drop to 80 MHz when idle;
  // drivers hold it up while they work. Light sleep only on boards that ask for
  // it, and then only while dozing: the app holds the chip awake otherwise
  // (EspSystem::enable_doze), so the display, touch, audio and USB keep working.
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  pm.min_freq_mhz = 80;
  pm.light_sleep_enable = board.light_sleep && g_system.enable_doze();
  if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "power management unavailable; the CPU stays at full speed");
#endif
  g_updater.expect_board(board.name);
  g_updater.start();  // a new firmware on probation starts its clock now

  // Wi-Fi first: the radio is the entropy source for the device key.
  g_wifi.begin(g_storage);

  hg::Hal hal;
  hal.system = &g_system;
  hal.transport = &g_transport;
  hal.storage = &g_storage;
  if (latch_power) hal.power = &g_latch_power;
  if (g_updater.capacity()) hal.updater = &g_updater;
  i2c_master_bus_handle_t i2c_bus = hgp::i2c::bus(board.i2c);
  const bool peripherals_ready = (!board.cores3 || g_cores3.begin(i2c_bus)) &&
      (!board.expander_reset.mask || hgp::tca9554_reset(i2c_bus, board.expander_reset));
  if (board.cores3 && peripherals_ready)
    g_display.board_backlight = [](uint8_t percent) { g_cores3.set_backlight(percent); };
  if (peripherals_ready && board.lcd.enabled) {
    if (board.lcd.bus.type == hgp::LcdBus::Type::Rgb) {
      if (g_rgb.begin(board.lcd, i2c_bus)) hal.display = &g_rgb;
    } else if (board.lcd.bus.type == hgp::LcdBus::Type::I80) {
      if (g_parallel.begin(board.lcd, hgp::lcd_power_pin(board))) hal.display = &g_parallel;
    } else if (g_display.begin(board.lcd, i2c_bus)) {
      hal.display = &g_display;
    }
  } else if (board.amoled.enabled && g_amoled.begin(board.amoled)) {
    hal.display = &g_amoled;
  }
  if (board.mic.enabled && g_mic.begin(board.mic)) hal.mic = &g_mic;
  if (board.speaker.enabled && g_speaker.begin(board.speaker)) hal.speaker = &g_speaker;
  if (board.axp2101 && g_power.begin(i2c_bus)) {
    hal.power = &g_power;
    if (board.axp_power_key) g_power.use_power_key();
    g_power.use_curves(board.battery_curve, board.battery_charge_curve);
    // Wi-Fi retries back off only on battery; unknown counts as plugged in.
    g_wifi.on_battery = [] { return g_power.external_power() == std::optional<bool>(false); };
  }
  const bool audio_power = peripherals_ready && (!board.axp_audio_supply || g_power.enable_audio_supply());
  if (!audio_power) ESP_LOGE(TAG, "audio supply unavailable");
  if (board.codec.enabled && audio_power && g_codec.begin(board.codec, i2c_bus)) {
    if (g_codec_mic.begin(g_codec.in(), board.codec.rmnm_mics, g_codec.mic_link())) hal.mic = &g_codec_mic;
    if (g_codec_speaker.begin(g_codec.out(), board.codec.stereo32, board.codec.speaker_pa ? board.codec.pa : -1)) hal.speaker = &g_codec_speaker;
    // Light-sleep boards power the DAC down while dozing, if the mics don't share it.
    if (board.light_sleep && g_codec.mic_link().switchable) g_codec_speaker.allow_power_down(g_codec.mic_link().format);
  }
  g_buttons.begin(board.buttons);
  const bool touch = peripherals_ready && (board.touch.enabled || board.pwr_key.enabled || board.encoder.a >= 0) &&
                     g_touch.begin(board.touch, board.pwr_key, board.encoder, i2c_bus);

  hgp::diag::Parts parts;
  parts.display = hal.display == &g_display ? g_display.controller_name()
                      : hal.display == &g_parallel ? "st7789-i80"
                      : hal.display == &g_rgb ? g_rgb.controller_name()
                      : hal.display == &g_amoled ? "co5300"
                                                : "none";
  parts.mic = hal.mic == &g_codec_mic ? (board.codec.mic == hgp::MicCodec::Es8311 ? "es8311" : "es7210")
              : hal.mic == &g_mic ? "i2s" : "none";
  parts.speaker = hal.speaker == &g_codec_speaker ?
      (board.codec.speaker == hgp::SpeakerCodec::Aw88298 ? "aw88298" : "es8311") :
      hal.speaker == &g_speaker ? "i2s" : "none";
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
  profile.has_scroll_buttons = (board.buttons.up >= 0 && board.buttons.down >= 0) || board.encoder.a >= 0;
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
    if (key == "wifi_ssid" || key == "wifi_pass") { app.close_wifi_setup(); g_wifi.reconfigure(); }
    if (key == "touch_cancel") apply_touch_cancel();
  };
  app.on_wifi_setup = [] { return g_wifi.start_setup(); };
  app.on_wifi_setup_close = [] { g_wifi.stop_setup(); };
  app.on_wifi_setup_ap = [] { return g_wifi.setup_ap(); };
  app.on_diag = [](hg::json::Value& report) {
    hgp::diag::report(report);
    report.set("ota", g_updater.describe());
    if (const auto ma = g_power.charge_current_ma()) report.set("charge_current_ma", static_cast<unsigned>(*ma));
  };
  app.recent_log = &hgp::diag::recent_log;
  if (board.light_sleep) keep_pins_through_sleep(board);
  g_system.on_doze_start = [] { g_codec_speaker.power_down(); };
  g_system.on_doze_end = [] {
    g_codec_speaker.power_up();
    g_wifi.retry_now();
  };
  app.begin();
  hgp::console::begin();

  for (;;) {
    hgp::Event ev;
    // Block briefly for events, then run the core's timers and animations.
    // Dozing, the chip may sleep between events; input and the network still wake it.
    if (hgp::events::receive(ev, pdMS_TO_TICKS(app.dozing() ? hg::App::kDozeWaitMs : 10))) {
      do {
        dispatch(app, ev);
        hgp::events::release(ev);
      } while (hgp::events::receive(ev, 0));
    }
    // A loss whose WsClosed event was dropped; harmless if it was delivered too.
    if (g_transport.take_lost()) app.on_transport_closed("disconnected");
    g_buttons.poll(app);
    g_wifi.tick(app, g_system.now_ms());
    if (g_gestures) g_gestures->tick(g_system.now_ms());
    app.tick();
  }
}
