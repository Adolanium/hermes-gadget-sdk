// Hermes Gadget firmware entry point: wires the ESP32 drivers to the portable
// core (hg::App) and runs the app loop on the main task.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "hg/touch.hpp"
#include "nvs_flash.h"
#include "sdkconfig.h"
#if SOC_MIPI_DSI_SUPPORTED
#include "esp_ldo_regulator.h"
#endif

namespace {

const char* TAG = "hg.main";

#if SOC_MIPI_DSI_SUPPORTED
esp_ldo_channel_handle_t g_p4_vdd_io_ldo = nullptr;

bool enable_p4_vdd_io_rail() {
  esp_ldo_channel_config_t config{};
  config.chan_id = 4;
  config.voltage_mv = 3300;
  const esp_err_t err = esp_ldo_acquire_channel(&config, &g_p4_vdd_io_ldo);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "VDD_IO_5 LDO4 at 3300 mV failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "VDD_IO_5 enabled by LDO4 at 3300 mV");
  return true;
}
#endif

hgp::EspSystem g_system;
hgp::NvsStorage g_storage;
hgp::WsTransport g_transport;
hgp::SpiDisplay g_display;
// The microphone object holds handles and no buffers, so the CYD uses it too:
// its PDM mic runs on I2S0, the one port with the PDM-to-PCM converter.
hgp::I2sMic g_mic;
#if !CONFIG_HG_BOARD_ESP32_CYD
// The CYD has no PSRAM: its 320x240 framebuffer takes all but a few KiB of
// internal RAM, so only the drivers a board profile can select are
// instantiated. Its own peripherals are wired in the CYD branch of app_main.
hgp::ParallelDisplay g_parallel;
hgp::RgbDisplay g_rgb;
hgp::AmoledDisplay g_amoled;
#if SOC_MIPI_DSI_SUPPORTED
hgp::DsiDisplay g_dsi;
hgp::P4I2sSpeaker g_p4_speaker;
hgp::Stc8Power g_stc8_power;
#endif
hgp::I2sSpeaker g_speaker;
hgp::CodecAudio g_codec;
hgp::CodecMic g_codec_mic;
hgp::CodecSpeaker g_codec_speaker;
hgp::OpusSpeakerDecoder g_opus;
hgp::TouchInput g_touch;
hgp::MotionInput g_motion;
hgp::AxpPower g_power;
hgp::LatchPower g_latch_power;
hgp::CoreS3Board g_cores3;
hgp::PhoneLink g_phone;
#endif
hgp::Buttons g_buttons;
hgp::Wifi g_wifi;
hgp::TimePort g_time;
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

// The name a phone sees: the `name` setting, as hg::App uses it.
#if !CONFIG_HG_BOARD_ESP32_CYD  // the CYD has no Bluetooth phone link
std::string device_name() {
  auto v = g_storage.get("name");
  return v && !v->empty() ? *v : std::string(CONFIG_HG_DEFAULT_NAME);
}
#endif

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

// Supplies switched straight from a GPIO go on before anything talks to the
// parts behind them.
void power_rails_on(const hgp::BoardConfig& b) {
  bool any = false;
  for (const hgp::PowerRail& rail : b.power_rails) {
    if (rail.gpio < 0) continue;
    const auto pin = static_cast<gpio_num_t>(rail.gpio);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, rail.on_level ? 1 : 0);
    any = true;
  }
  // One tick more than the rounded-down count, so the wait is never shorter at 100 Hz.
  if (any && b.power_rails_settle_ms) vTaskDelay(pdMS_TO_TICKS(b.power_rails_settle_ms) + 1);
}

// Light sleep disables every GPIO it doesn't know about; these must keep their
// level so the panel, touch controller, amplifier and buses survive a doze.
void keep_pins_through_sleep(const hgp::BoardConfig& b) {
  // A latch-powered board must also keep its battery latch high, or light sleep cuts the supply.
  const int pins[] = {b.amoled.cs, b.amoled.sclk, b.amoled.d0, b.amoled.d1, b.amoled.d2, b.amoled.d3,
                      b.amoled.rst, b.lcd.cs, b.lcd.dc, b.lcd.rst, b.lcd.backlight, b.touch.rst,
                      b.touch.bus.sda, b.touch.bus.scl, b.codec.pa, b.i2c.sda, b.i2c.scl,
                      b.latch_power.enabled ? b.latch_power.enable : -1};
  for (int pin : pins)
    if (pin >= 0) gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
  for (const hgp::PowerRail& rail : b.power_rails)
    if (rail.gpio >= 0) gpio_sleep_sel_dis(static_cast<gpio_num_t>(rail.gpio));
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
      g_time.set_network(true);
      break;
    case EventType::NetDown:
      app.on_network(false, text ? text : "");
      g_time.set_network(false);
      break;
    case EventType::WifiDisconnected:
      g_wifi.disconnected();
      app.on_network(false, text ? text : "");
      g_time.set_network(false);
      break;
    case EventType::WifiStarted: g_wifi.reconfigure(); break;
    case EventType::WifiScanDone: g_wifi.scan_done(); break;
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
    case EventType::Phone:
      if (ev.len == sizeof(hgp::PhoneSample)) {
        const auto* phone = reinterpret_cast<const hgp::PhoneSample*>(ev.data);
        switch (phone->kind) {
          case hgp::PhoneSample::Kind::Passkey: app.on_phone_passkey(phone->passkey); break;
          case hgp::PhoneSample::Kind::Paired: app.on_phone_paired(true); break;
          case hgp::PhoneSample::Kind::Failed: app.on_phone_paired(false); break;
          case hgp::PhoneSample::Kind::Status: app.on_phone_status(phone->paired, phone->connected); break;
        }
      }
      break;
    case EventType::TimeSynced:
      g_time.on_synced();  // writes the RTC from the now-set clock
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
  power_rails_on(board);
  // A board whose amplifier hangs off the chip's own DAC (the CYD's SC8002B on
  // GPIO26) gets three boot beeps: they show the amplifier and speaker work
  // before any reply is routed there (port_dac.cpp). Boards without a DAC
  // speaker do nothing. Blocks for about 0.6 s.
  if (board.speaker_dac >= 0) hgp::dac_beep(board.speaker_dac);
#if !CONFIG_HG_BOARD_ESP32_CYD
  const bool latch_power = board.latch_power.enabled && g_latch_power.begin(board.latch_power);
#endif
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
#if !CONFIG_HG_BOARD_ESP32_CYD
  if (latch_power) hal.power = &g_latch_power;
  if (latch_power && board.latch_power.usb_external_power)
    g_wifi.on_battery = [] { return g_latch_power.external_power() == std::optional<bool>(false); };
#endif
  if (g_updater.capacity()) hal.updater = &g_updater;
#if CONFIG_HG_BOARD_ESP32_CYD
  // The CYD's peripherals are the panel, the BOOT key and a PDM microphone on
  // the 4-pin header: no I2C bus, expander, codec, touch controller or PMIC to
  // bring up.
  if (board.lcd.enabled && g_display.begin(board.lcd, nullptr)) hal.display = &g_display;
  if (board.mic.enabled && g_mic.begin(board.mic)) {
    hal.mic = &g_mic;
  } else if (board.mic.enabled) {
    // The PDM mic's I2S0 DMA and task stack did not fit next to the reserved
    // framebuffer band: lower kCydFrameRows in port_display.cpp if this appears.
    ESP_LOGE(TAG, "microphone did not start: not enough internal RAM for its buffers");
  }
  // No I2C bus on this board; the timezone setting and network time still apply.
  g_time.begin(board.rtc, nullptr, g_storage);
  constexpr bool touch = false;
  constexpr bool imu = false;  // no I2C bus: no motion sensor to bring up
  hgp::diag::Parts parts;
  parts.display = hal.display ? g_display.controller_name() : "none";
  parts.mic = hal.mic == &g_mic ? (board.mic.pdm ? "pdm" : "i2s") : "none";
  parts.time = &g_time;
#else
  bool io_rail_ready = true;
#if SOC_MIPI_DSI_SUPPORTED
  if (board.lcd.bus.type == hgp::LcdBus::Type::Dsi) io_rail_ready = enable_p4_vdd_io_rail();
#endif
  // GPIO45/46 belong to VDD_IO_5 on the P4; acquire LDO4 before creating I2C0.
  i2c_master_bus_handle_t i2c_bus = io_rail_ready ? hgp::i2c::bus(board.i2c) : nullptr;
  const bool peripherals_ready = io_rail_ready && (!board.cores3 || g_cores3.begin(i2c_bus)) &&
      (!board.expander_reset.mask || hgp::tca9554_reset(i2c_bus, board.expander_reset));
  if (board.cores3 && peripherals_ready)
    g_display.board_backlight = [](uint8_t percent) { g_cores3.set_backlight(percent); };
  if (peripherals_ready && board.lcd.enabled) {
#if SOC_MIPI_DSI_SUPPORTED
    if (board.lcd.bus.type == hgp::LcdBus::Type::Dsi) {
      if (g_dsi.begin(board.lcd, board.lcd.dsi)) hal.display = &g_dsi;
    } else
#endif
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
#if SOC_MIPI_DSI_SUPPORTED
  if (board.p4_speaker.enabled) {
    if (g_p4_speaker.begin(board.speaker, board.p4_speaker)) hal.speaker = &g_p4_speaker;
  } else
#endif
  if (board.speaker.enabled && g_speaker.begin(board.speaker)) hal.speaker = &g_speaker;
  if (board.axp2101 && g_power.begin(i2c_bus)) {
    hal.power = &g_power;
    if (board.axp_power_key) g_power.use_power_key();
    g_power.use_curves(board.battery_curve, board.battery_charge_curve);
    // Wi-Fi retries back off only on battery; unknown counts as plugged in.
    g_wifi.on_battery = [] { return g_power.external_power() == std::optional<bool>(false); };
  }
#if SOC_MIPI_DSI_SUPPORTED
  if (peripherals_ready && board.stc8_battery && g_stc8_power.begin(i2c_bus)) hal.power = &g_stc8_power;
#endif
  const bool audio_power = peripherals_ready && (!board.axp_audio_supply || g_power.enable_audio_supply());
  if (!audio_power) ESP_LOGE(TAG, "audio supply unavailable");
  if (board.codec.enabled && audio_power && g_codec.begin(board.codec, i2c_bus)) {
    if (g_codec_mic.begin(g_codec.in(), board.codec.rmnm_mics, g_codec.mic_link())) hal.mic = &g_codec_mic;
    if (g_codec_speaker.begin(g_codec.out(), board.codec.stereo32, board.codec.speaker_pa ? board.codec.pa : -1)) hal.speaker = &g_codec_speaker;
    // Light-sleep boards power the DAC down while dozing, if the mics don't share it.
    if (board.light_sleep && g_codec.mic_link().switchable) g_codec_speaker.allow_power_down(g_codec.mic_link().format);
  }
  if (hal.speaker) hal.speaker_decoder = &g_opus;  // Opus replies when Hermes offers them
  const bool imu = peripherals_ready && board.imu.enabled && g_motion.begin(board.imu, i2c_bus);
  g_time.begin(board.rtc, i2c_bus, g_storage);
  const bool touch = peripherals_ready && (board.touch.enabled || board.pwr_key.enabled || board.encoder.a >= 0) &&
                     g_touch.begin(board.touch, board.pwr_key, board.encoder,
                                   board.touch.bus.sda >= 0 ? hgp::i2c::touch_bus(board.touch.bus) : i2c_bus);

  hgp::diag::Parts parts;
#if SOC_MIPI_DSI_SUPPORTED
  if (hal.display == &g_dsi) {
    parts.display = g_dsi.controller_name();
  } else
#endif
  {
    parts.display = hal.display == &g_display ? g_display.controller_name()
                    : hal.display == &g_parallel ? "st7789-i80"
                    : hal.display == &g_rgb ? g_rgb.controller_name()
                    : hal.display == &g_amoled ? "co5300"
                                              : "none";
  }
  parts.mic = hal.mic == &g_codec_mic ? (board.codec.mic == hgp::MicCodec::Es8311 ? "es8311" : "es7210")
              : hal.mic == &g_mic ? (board.mic.pdm ? "pdm" : "i2s") : "none";
#if SOC_MIPI_DSI_SUPPORTED
  if (hal.speaker == &g_p4_speaker) parts.speaker = "i2s";
  else
#endif
  parts.speaker = hal.speaker == &g_codec_speaker ?
      (board.codec.speaker == hgp::SpeakerCodec::Aw88298 ? "aw88298" : "es8311") :
      hal.speaker == &g_speaker ? "i2s" : "none";
  parts.touch = touch && g_touch.has_touch();
  parts.key = touch && g_touch.has_key();
  parts.i2c = i2c_bus;
  parts.imu = imu ? "qmi8658" : "none";
  parts.rtc = g_time.rtc_ready() ? "pcf85063" : "none";
  parts.motion = imu ? &g_motion : nullptr;
  parts.time = &g_time;
#endif
  g_buttons.begin(board.buttons);
  hgp::diag::set_parts(parts);
  hgp::diag::log_boot_summary();

  hg::DeviceProfile profile;
  profile.board = board.name;
  profile.firmware = version;
#if CONFIG_HG_FRAME_STATS
  profile.frame_stats = true;
#endif
  profile.frame_budget_us = CONFIG_HG_FRAME_BUDGET_US;
  profile.frame_log_ms = CONFIG_HG_FRAME_LOG_S * 1000u;
  profile.default_name = CONFIG_HG_DEFAULT_NAME;
#if CONFIG_SPIRAM
  profile.faces = esp_psram_is_initialized();
#else
  profile.faces = false;
#endif
  profile.default_server_url = CONFIG_HG_DEFAULT_SERVER_URL;
  profile.default_access_token = CONFIG_HG_DEFAULT_ACCESS_TOKEN;
  profile.has_cancel_button = board.buttons.cancel >= 0 || touch;
  profile.has_scroll_buttons = (board.buttons.up >= 0 && board.buttons.down >= 0) || board.encoder.a >= 0;
  profile.talk_label = board.talk_label;
  profile.cancel_label = board.cancel_label;
  if (touch && board.touch.enabled) {
    profile.touch_screen = true;
    profile.touch_ppi = board.touch.ppi;
    profile.extra_settings = {"touch_cancel"};
  }
  profile.extra_settings.push_back("timezone");  // the port keeps its own time
  profile.motion_sensor = imu;
#if !CONFIG_HG_BOARD_ESP32_CYD
  if (hal.mic == &g_codec_mic) {
    profile.mic_rate = hgp::CodecAudio::kRate;
    profile.vad_min_rms = board.codec.vad_min_rms;
  }
  if (hal.speaker == &g_codec_speaker) profile.speaker_rate = hgp::CodecAudio::kRate;
#endif

#if CONFIG_HG_BOARD_ESP32_CYD
  // From the heap, so the early framebuffer can take the one large contiguous
  // block before this claims part of what is left of internal RAM.
  static hg::App& app = *new hg::App(hal, profile);
#else
  static hg::App app(hal, profile);
#endif
  static hg::TouchGestures gestures(app);
  if (profile.touch_screen) g_gestures = &gestures;
  apply_touch_cancel();
  add_status_led(app, board.status_led);
  app.on_setting_changed = [](std::string_view key) {
    if (key == "wifi_ssid" || key == "wifi_pass") app.close_wifi_setup();
    if (key == "wifi_ssid" || key == "wifi_pass" || key == "wifi_nets") g_wifi.networks_changed();
    if (key == "touch_cancel") apply_touch_cancel();
    if (key == "clock") g_time.set_wanted(app.clock_enabled());
    if (key == "timezone") g_time.apply_timezone(g_storage);
#if !CONFIG_HG_BOARD_ESP32_CYD
    if (key == "name" && hgp::board_config().bluetooth) g_phone.set_name(device_name());
#endif
  };
#if !CONFIG_HG_BOARD_ESP32_CYD
  if (board.bluetooth && g_phone.begin(device_name())) {
    app.on_phone_pair = [] { return g_phone.open_pairing(); };
    app.on_phone_pair_close = [] { g_phone.close_pairing(); };
    app.on_phone_forget = [] { g_phone.forget(); };
  }
#endif
  app.on_wifi_setup = [] { return g_wifi.start_setup(); };
  app.on_wifi_setup_close = [] { g_wifi.stop_setup(); };
  app.on_wifi_setup_ap = [] { return g_wifi.setup_ap(); };
  app.current_wifi = [] { return g_wifi.current(); };
  app.on_diag = [](hg::json::Value& report) {
    hgp::diag::report(report);
    report.set("ota", g_updater.describe());
#if !CONFIG_HG_BOARD_ESP32_CYD
    if (const auto ma = g_power.charge_current_ma()) report.set("charge_current_ma", static_cast<unsigned>(*ma));
#endif
  };
  app.recent_log = &hgp::diag::recent_log;
  if (board.light_sleep) keep_pins_through_sleep(board);
#if !CONFIG_HG_BOARD_ESP32_CYD
  g_system.on_doze_start = [] { g_codec_speaker.power_down(); };
  g_system.on_doze_end = [] {
    g_codec_speaker.power_up();
    g_wifi.retry_now();
  };
#endif
  app.begin();
  g_time.set_wanted(app.clock_enabled());  // SNTP only while the clock face is on
  hgp::console::begin();

#if !CONFIG_HG_BOARD_ESP32_CYD
  TickType_t motion_at = 0;     // last motion poll (the loop runs at ~10 ms)
  uint32_t motion_samples = 0;  // the motion task's count at the last forwarded sample
#endif

  for (;;) {
    hgp::Event ev;
    // Block briefly for events, then run the core's timers and animations.
    // Dozing on a light-sleep board, the chip may sleep between events; input and
    // the network still wake it. Other boards keep the short wait.
    if (hgp::events::receive(ev, pdMS_TO_TICKS(board.light_sleep && app.dozing() ? hg::App::kDozeWaitMs : 10))) {
      do {
        dispatch(app, ev);
        hgp::events::release(ev);
      } while (hgp::events::receive(ev, 0));
    }
    // A loss whose WsClosed event was dropped; harmless if it was delivered too.
    if (g_transport.take_lost()) app.on_transport_closed("disconnected");
    // Waiting would leave the device answering nothing until someone resets it.
    if (g_transport.stuck()) {
      ESP_LOGE(TAG, "the WebSocket send task is stuck; restarting");
      vTaskDelay(pdMS_TO_TICKS(100));  // let the log line out
      esp_restart();
    }
    g_buttons.poll(app);
    g_wifi.tick(app, g_system.now_ms());
    if (g_gestures) g_gestures->tick(g_system.now_ms());
    app.tick();
#if !CONFIG_HG_BOARD_ESP32_CYD
    // The motion task keeps the latest sample; poll it about every 100 ms instead
    // of letting 10 Hz events fill the queue. Only new reads go to the app, so a
    // sensor that stops answering still goes stale there.
    if (imu && xTaskGetTickCount() - motion_at >= pdMS_TO_TICKS(100)) {
      motion_at = xTaskGetTickCount();
      const uint32_t n = g_motion.samples();
      if (n != motion_samples) {
        motion_samples = n;
        int16_t mx, my, mz;
        g_motion.latest(mx, my, mz);
        app.on_motion(mx, my, mz);
      }
    }
#endif
  }
}
