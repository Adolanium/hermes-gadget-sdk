// ESP32 implementations of the core HAL, plus the event queue that moves
// driver events (Wi-Fi, WebSocket, microphone, console) onto the app task.
//
// Threading rule: hg::App is only touched by the app task (app_main's loop).
// Driver tasks and callbacks post Events; the loop dispatches them.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>

#include "freertos/FreeRTOS.h"  // must precede every other FreeRTOS header

#include "board.hpp"
#include "axp2101.hpp"
#include "band_flush.hpp"
#include "cores3.hpp"
#include "shared_reply.hpp"
#include "speaker_pa.hpp"
#include "speaker_underruns.hpp"
#include "tag_scanner.hpp"
#include "wifi_fast.hpp"
#include "wifi_pick.hpp"
#include "wifi_power.hpp"
#include "wifi_retry.hpp"
#include "ws_link.hpp"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "soc/soc_caps.h"
#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#include "stc8_battery.hpp"
#endif
#include "esp_lcd_io_i80.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_websocket_client.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "hg/app.hpp"
#include "hg/hal.hpp"
#include "hg/setup.hpp"
#include "hg/wifi_networks.hpp"

namespace hgp {

// ---------------------------------------------------------------------------
// Events

enum class EventType : uint8_t { NetUp, NetDown, WsOpen, WsText, WsBinary, WsClosed, Mic, Console, Touch, Key,
                                 WifiStarted, WifiDisconnected, WifiProvision, WifiScanDone, Encoder, Phone };

// Payload of Phone events (posted by the Bluetooth host task).
struct PhoneSample {
  enum class Kind : uint8_t { Passkey, Paired, Failed, Status };
  Kind kind;
  uint32_t passkey;  // Passkey: the code to show
  uint8_t paired;    // Status: phones paired
  bool connected;    // Status: one is connected now
};

// Payloads of Touch, Key and Encoder events (posted by the input task).
struct TouchSample {
  bool touching;
  int16_t x, y;
};
struct KeySample {
  bool pressed;
};
struct EncoderSample {
  int8_t direction;
};

// Wakes a console command when the app task has answered it.
struct ConsoleSignal {
  SemaphoreHandle_t handle = xSemaphoreCreateBinary();
  ~ConsoleSignal() {
    if (handle) vSemaphoreDelete(handle);
  }
  void give() {
    if (handle) xSemaphoreGive(handle);
  }
  bool wait(uint32_t timeout_ms) { return handle && xSemaphoreTake(handle, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }
};

// Shared by the console task and the app task; see shared_reply.hpp.
using ConsoleRequest = hg::SharedReply<ConsoleSignal>;

struct Event {
  EventType type;
  uint32_t generation;  // WebSocket connection generation (stale events are dropped)
  uint8_t* data;        // heap payload owned by the event, freed after dispatch
  size_t len;
  ConsoleRequest* console;
};

namespace events {
void init();
// Copies `data`. Returns false (and drops the event) when the queue is full.
bool post(EventType type, const void* data = nullptr, size_t len = 0, uint32_t generation = 0,
          ConsoleRequest* console = nullptr);
bool receive(Event& out, TickType_t wait);
void release(Event& ev);
}  // namespace events

// ---------------------------------------------------------------------------
// HAL implementations

class EspSystem final : public hg::System {
 public:
  uint32_t now_ms() override;
  uint64_t now_us() override;
  void random_bytes(uint8_t* out, size_t len) override;
  void log(hg::LogLevel level, std::string_view message) override;
  // Boards with BoardConfig::light_sleep: keeps the chip out of light sleep
  // except while dozing. Call before power management enables light sleep.
  bool enable_doze();
  void set_dozing(bool dozing) override;
  static bool dozing_now();  // for tasks that poll: poll less while dozing
  std::function<void()> on_doze_start;  // e.g. power the speaker down
  std::function<void()> on_doze_end;    // e.g. power it up, retry Wi-Fi now: the user is back

 private:
  esp_pm_lock_handle_t awake_ = nullptr;  // held whenever not dozing
  bool dozing_ = false;
  int64_t doze_started_us_ = 0;
};

class NvsStorage final : public hg::Storage {
 public:
  bool begin();
  std::optional<std::string> get(std::string_view key) override;
  bool set(std::string_view key, std::string_view value) override;
  void erase(std::string_view key) override;

 private:
  uint32_t handle_ = 0;  // nvs_handle_t
  SemaphoreHandle_t lock_ = nullptr;  // the Wi-Fi task reads credentials too
};

class WsTransport final : public hg::Transport {
 public:
  void connect(const std::string& url, const std::string& subprotocol) override;
  bool send_text(std::string_view text) override;
  bool send_binary(const uint8_t* data, size_t len) override;
  void close() override;
  uint32_t generation() const { return gens_.current(); }
  // True once if the current connection was lost. Backs up the WsClosed event,
  // which the client may post from the app task itself while the queue is full.
  bool take_lost() { return gens_.take_lost(); }
  // True when the send task has been stuck on one item for hg::ws::kStuckMs, or
  // couldn't take a client to close: the transport can't recover without a restart.
  bool stuck() const;

 private:
  static void on_event(void* arg, const char* base, int32_t id, void* data);
  // Sends run on their own task, so a slow or dead link never stalls the app
  // task (and with it the screen): send_*() only queue a copy.
  static void tx_task(void* arg);
  bool enqueue(uint8_t opcode, const void* data, size_t len);
  esp_websocket_client_handle_t client_ = nullptr;
  QueueHandle_t tx_ = nullptr;
  hg::ws::Generations gens_;
  hg::ws::TxWatch watch_;
  std::atomic<bool> handoff_failed_{false};
  std::string url_, subprotocol_;
  // Fragment reassembly, WebSocket task only: an old client's task can still be
  // delivering a frame after close(), so connect() leaves this alone. Each
  // message's first frame clears it.
  std::string rx_;
  uint8_t rx_opcode_ = 0;
};

// ESP-IDF RGB timing bus with ST7701 command initialization on 3-wire SPI.
class RgbDisplay final : public hg::Display {
 public:
  bool begin(const LcdConfig& cfg, i2c_master_bus_handle_t bus);
  hg::DisplayInfo info() const override;
  uint16_t* framebuffer() override { return fb_; }
  void flush(uint16_t y0, uint16_t y1) override;
  void set_backlight(uint8_t percent) override;
  const char* controller_name() const {
    return cfg_.rgb.controller == RgbController::St7701 ? "st7701-rgb" : "ili6122-rgb";
  }

 private:
  static bool on_color_done(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t* edata, void* ctx);
  LcdConfig cfg_{};
  esp_lcd_panel_io_handle_t io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  i2c_master_dev_handle_t expander_ = nullptr;
  uint16_t* fb_ = nullptr;
  bool draw_failed_ = false;
  uint16_t* staging_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
};

#if SOC_MIPI_DSI_SUPPORTED
// CrowPanel Advanced P4 MIPI-DSI panel. DSI pins are dedicated SoC peripherals.
class DsiDisplay final : public hg::Display {
 public:
  bool begin(const LcdConfig& cfg, const DsiPanelConfig& dsi);
  hg::DisplayInfo info() const override;
  uint16_t* framebuffer() override { return fb_; }
  void flush(uint16_t y0, uint16_t y1) override;
  void set_backlight(uint8_t percent) override;
  const char* controller_name() const { return "ek79007-dsi"; }

 private:
  static bool on_color_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* event, void* ctx);
  bool wait_copy(uint32_t timeout_us);
  LcdConfig cfg_{};
  DsiPanelConfig dsi_{};
  esp_lcd_dsi_bus_handle_t bus_ = nullptr;
  esp_lcd_panel_io_handle_t io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t* fb_ = nullptr;
  bool use_dma2d_ = true;
  std::atomic<uint32_t> flushes_{0};
};
#endif
class SpiDisplay final : public hg::Display {
 public:
  bool begin(const LcdConfig& cfg, i2c_master_bus_handle_t bus);
  const char* controller_name() const { return controller_name_; }
  hg::DisplayInfo info() const override;
  uint16_t* framebuffer() override { return fb_; }
  void flush(uint16_t y0, uint16_t y1) override;
  void set_backlight(uint8_t percent) override;
  std::function<void(uint8_t)> board_backlight;

 private:
  static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx);
  LcdConfig cfg_{};
  const char* controller_name_ = "st7789";
  esp_lcd_panel_io_handle_t io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t* fb_ = nullptr;
  uint16_t* bounce_ = nullptr;  // DMA-capable staging rows
  SemaphoreHandle_t done_ = nullptr;
  std::optional<hg::BandFlush> bands_;
};

// I2S MEMS microphone: a reader task posts 20 ms PCM16 chunks while capturing.
class I2sMic final : public hg::AudioIn {
 public:
  bool begin(const I2sMicConfig& cfg);
  bool start(uint32_t sample_rate) override;
  void stop() override;

 private:
  static void task(void* arg);
  i2s_chan_handle_t rx_ = nullptr;
  uint32_t rate_ = 16000;
  bool pdm_ = false;  // PDM capture yields 16-bit samples; I2S yields 32-bit
  std::atomic<bool> capturing_{false};
};

// I2S amplifier fed from a stream buffer by a writer task.
// Speaker tasks time their underruns (speaker_underruns.hpp) with this clock and log
// them in one line when a stream has finished playing (port_audio.cpp).
uint32_t speaker_now_ms();
void log_underruns(const char* tag, const hg::SpeakerUnderruns::Totals& t);

class I2sSpeaker final : public hg::AudioOut {
 public:
  bool begin(const I2sSpeakerConfig& cfg);
  bool begin(uint32_t sample_rate) override;
  void write(const int16_t* samples, size_t count) override;
  void end() override;
  void abort() override;
  bool busy() const override;
  void set_volume(uint8_t percent) override { volume_ = percent; }

 private:
  static void task(void* arg);
  i2s_chan_handle_t tx_ = nullptr;
  StreamBufferHandle_t buffer_ = nullptr;
  std::atomic<uint32_t> rate_{16000};
  std::atomic<bool> open_{false};
  std::atomic<bool> draining_{false};
  std::atomic<bool> flush_{false};
  std::atomic<uint8_t> volume_{70};
};

#if SOC_MIPI_DSI_SUPPORTED
// P4-only I2S speaker path with stereo framing and amplifier control.
class P4I2sSpeaker final : public hg::AudioOut {
 public:
  bool begin(const I2sSpeakerConfig& pins, const P4SpeakerConfig& cfg);
  bool begin(uint32_t sample_rate) override;
  void write(const int16_t* samples, size_t count) override;
  void end() override;
  void abort() override;
  bool busy() const override;
  void set_volume(uint8_t percent) override { volume_ = percent; }

 private:
  static void task(void* arg);
  void set_pa(bool on);
  i2s_chan_handle_t tx_ = nullptr;
  StreamBufferHandle_t buffer_ = nullptr;
  std::atomic<uint32_t> rate_{16000};
  std::atomic<bool> open_{false};
  std::atomic<bool> draining_{false};
  std::atomic<bool> flush_{false};
  std::atomic<bool> flush_done_{true};
  int pa_ = -1;
  bool pa_active_low_ = false;
  bool stereo_ = true;
  bool left_align_ = true;
  std::atomic<uint8_t> volume_{70};
};
#endif
// I80 (8-bit parallel) ST7789 panel via esp_lcd. Panels on LCD modules that
// wire the controller to a parallel bus rather than SPI, e.g. LilyGO's
// T-Display-S3. Same framebuffer and bounce-buffer scheme as SpiDisplay.
class ParallelDisplay final : public hg::Display {
 public:
  bool begin(const LcdConfig& cfg, int power_pin);
  hg::DisplayInfo info() const override;
  uint16_t* framebuffer() override { return fb_; }
  void flush(uint16_t y0, uint16_t y1) override;
  void set_backlight(uint8_t percent) override;

 private:
  static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx);
  LcdConfig cfg_{};
  esp_lcd_i80_bus_handle_t i80_ = nullptr;
  esp_lcd_panel_io_handle_t io_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  uint16_t* fb_ = nullptr;
  uint16_t* bounce_ = nullptr;  // DMA-capable staging rows
  int bounce_rows_ = 0;
  uint8_t backlight_level_ = 0;
  SemaphoreHandle_t done_ = nullptr;
  std::optional<hg::BandFlush> bands_;
};

// QSPI AMOLED (CO5300) via esp_lcd panel IO. Same framebuffer and bounce-buffer
// scheme as SpiDisplay; the controller wants even window coordinates.
class AmoledDisplay final : public hg::Display {
 public:
  bool begin(const AmoledConfig& cfg);
  hg::DisplayInfo info() const override;
  uint16_t* framebuffer() override { return fb_; }
  void flush(uint16_t y0, uint16_t y1) override;
  void set_backlight(uint8_t percent) override;

 private:
  bool blank_margins();
  static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx);
  void command(uint8_t cmd, const uint8_t* data, size_t len);
  AmoledConfig cfg_{};
  esp_lcd_panel_io_handle_t io_ = nullptr;
  uint16_t* fb_ = nullptr;
  uint16_t* bounce_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  std::optional<hg::BandFlush> bands_;
  bool panel_on_ = true;
};

// The GPIO interrupt service, installed once for every driver that adds a pin handler: a second
// install is refused with an error line.
inline bool gpio_isr_service() {
  static std::once_flag once;
  static esp_err_t err = ESP_FAIL;
  std::call_once(once, [] { err = gpio_install_isr_service(0); });
  return err == ESP_OK || err == ESP_ERR_INVALID_STATE;  // (installed by someone else: there to use)
}

// Logs a failed ESP-IDF call. A display's begin() returns false on one, so the
// gadget runs without a screen instead of rebooting in a loop.
inline bool esp_ok(esp_err_t err, const char* tag, const char* what) {
  if (err == ESP_OK) return true;
  ESP_LOGE(tag, "%s failed: %s", what, esp_err_to_name(err));
  return false;
}

// What a BandFlush result means for the person reading the log.
inline void report_band_event(const char* tag, hg::BandFlush::Event event) {
  switch (event) {
    case hg::BandFlush::Event::TimedOut:
      ESP_LOGE(tag, "LCD transfer timed out; display paused until it completes");
      break;
    case hg::BandFlush::Event::Resumed:
      ESP_LOGW(tag, "late LCD transfer completed; display resumed");
      break;
    case hg::BandFlush::Event::Failed:
      ESP_LOGE(tag, "LCD transfer failed; display updates stopped until reboot");
      break;
    case hg::BandFlush::Event::None:
      break;
  }
}

// Pulses the board's TCA9554 reset lines before the display and touch start.
bool tca9554_reset(i2c_master_bus_handle_t bus, const ExpanderResetConfig& reset);

namespace i2c {
// The board's shared I2C master bus (created on first use).
i2c_master_bus_handle_t bus(const I2cBusConfig& cfg);
// A second bus on the other I2C controller, for a touch controller wired apart.
i2c_master_bus_handle_t touch_bus(const I2cBusConfig& cfg);
}

// ES8311 + ES7210 on one duplex I2S bus through esp_codec_dev. Both directions
// run at one fixed rate (they share the bit clock).
class CodecAudio {
 public:
  // 24000: the known-good xiaozhi aipi-lite pipeline is validated at 24 kHz on
  // this silicon (coeff {3072000,24000}); at 16000 playback ran ~2.3x slow.
  // Every codec board shares it; the ES8311, ES7210 and AW88298 drivers all
  // support 24 kHz as CodecAudio::begin() configures them.
  static constexpr uint32_t kRate = 24000;
  bool begin(const CodecAudioConfig& cfg, i2c_master_bus_handle_t bus);
  esp_codec_dev_handle_t out() const { return out_; }
  esp_codec_dev_handle_t in() const { return in_; }
  // How CodecMic powers its ADC down between captures (MicPower).
  struct MicLink {
    bool switchable = false;  // a recording-only codec, not shared with the speaker
    i2s_chan_handle_t rx = nullptr;
    esp_codec_dev_sample_info_t format = {};
    float gain_db = 0;
  };
  MicLink mic_link() const { return mic_link_; }

 private:
  i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
  esp_codec_dev_handle_t out_ = nullptr, in_ = nullptr;
  MicLink mic_link_;
};

class CodecMic final : public hg::AudioIn {
 public:
  bool begin(esp_codec_dev_handle_t dev, bool rmnm, const CodecAudio::MicLink& link);
  bool start(uint32_t sample_rate) override;
  void stop() override { capturing_ = false; }

 private:
  static void task(void* arg);
  esp_codec_dev_handle_t dev_ = nullptr;
  CodecAudio::MicLink link_;
  TaskHandle_t task_ = nullptr;
  bool rmnm_ = false;
  int16_t* raw_ = nullptr;
  std::atomic<bool> capturing_{false};
};

class CodecSpeaker final : public hg::AudioOut {
 public:
  bool begin(esp_codec_dev_handle_t dev, bool stereo32, int pa);
  bool begin(uint32_t sample_rate) override;
  void write(const int16_t* samples, size_t count) override;
  void end() override;
  void abort() override;
  bool busy() const override;
  void set_volume(uint8_t percent) override;
  // While dozing: close the DAC and its I2S channel (an enabled channel holds a
  // power lock that keeps the chip out of light sleep). Only for a speaker codec
  // the microphones don't share. begin() powers it back up before playing.
  void allow_power_down(const esp_codec_dev_sample_info_t& format) {
    format_ = format;
    can_power_down_ = true;
  }
  void power_down();
  void power_up();

 private:
  static void task(void* arg);
  esp_codec_dev_handle_t dev_ = nullptr;
  esp_codec_dev_sample_info_t format_ = {};
  bool can_power_down_ = false, powered_ = true;
  uint8_t volume_ = 70;
  TaskHandle_t task_ = nullptr;
  bool stereo32_ = false;
  int32_t* stereo_ = nullptr;
  std::optional<hg::SpeakerPa> pa_;  // set when this speaker, not esp_codec_dev, drives the PA pin
  StreamBufferHandle_t buffer_ = nullptr;
  std::atomic<bool> open_{false};
  std::atomic<bool> draining_{false};
  std::atomic<bool> flush_{false};
  std::atomic<uint32_t> drop_bytes_{0};  // bytes dropped on overflow, reported at stream end
};

// Polls a touchscreen, a key mirrored on an I/O expander and a rotary encoder
// from its own task (some controllers need a pause between write and read) and
// posts Touch, Key and Encoder events to the app task.
class TouchInput {
 public:
  bool begin(const TouchConfig& touch, const ExpanderKeyConfig& key, const EncoderConfig& encoder,
             i2c_master_bus_handle_t bus);
  bool has_touch() const { return touch_dev_ != nullptr || managed_touch_ != nullptr; }
  bool has_key() const { return key_dev_ != nullptr; }

 private:
  static void task(void* arg);
  bool read_touch(TouchSample& out);
  bool begin_gt911(i2c_master_bus_handle_t bus);
  bool begin_box_touch(i2c_master_bus_handle_t bus);
  void gate_on_int();
  static void on_int(void* arg);
  static void on_encoder_edge(void* arg);  // IRAM: steps the quadrature decoder per edge
  static void on_encoder_a_edge(void* arg);  // IRAM: pulse-per-detent channel A
  static void on_encoder_b_edge(void* arg);  // IRAM: pulse-per-detent channel B
  void IRAM_ATTR encoder_pulse(int8_t channel);  // +1 = channel A, -1 = channel B; direction maps at post
  bool may_read(bool was_touching);
  void wait(uint32_t ms);
  bool read_key(bool& pressed);
  TouchConfig touch_{};
  ExpanderKeyConfig key_{};
  i2c_master_dev_handle_t touch_dev_ = nullptr;
  i2c_master_dev_handle_t key_dev_ = nullptr;
  esp_lcd_touch_handle_t managed_touch_ = nullptr;
  bool int_gated_ = false;                // FT5x06 with INT: read after an edge or with a finger down
  std::atomic<bool> int_pending_{false};  // an edge since the last read
  void* task_ = nullptr;                  // the touch task, woken by an edge
  gpio_num_t encoder_a_ = GPIO_NUM_NC;
  gpio_num_t encoder_b_ = GPIO_NUM_NC;
  volatile uint8_t encoder_state_ = 0;      // last A/B sample (ISR-written)
  volatile int8_t encoder_accumulator_ = 0;  // quadrature steps toward the next detent (ISR-written)
  std::atomic<int8_t> encoder_pending_{0};   // whole detents waiting for the task to post
  bool encoder_pulse_per_detent_ = false;    // knob-ring encoder: channel = direction
  volatile int8_t encoder_first_ = 0;        // leading channel of the current detent (0 = idle)
  volatile int64_t encoder_pair_deadline_us_ = 0;  // solo-pulse timeout (task checks it)
  volatile int64_t last_edge_us_ = INT64_MIN;  // per-edge debounce (ISR-written)
};

#if SOC_MIPI_DSI_SUPPORTED
class Stc8Power final : public hg::Power {
 public:
  Stc8Power();
  bool begin(i2c_master_bus_handle_t bus);
  std::optional<hg::PowerStatus> read() override;
  bool can_power_off() const override { return false; }
  bool power_off() override { return false; }

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  hg::Stc8Battery chip_;
};
#endif

class AxpPower final : public hg::Power {
 public:
  bool begin(i2c_master_bus_handle_t bus);
  bool enable_audio_supply() { return chip_ && chip_->enable_aldo1_3v3(); }
  std::optional<hg::PowerStatus> read() override { return chip_->read(); }
  bool power_off() override { return chip_->power_off(); }
  // For boards whose PWR key reaches only the AXP2101 (BoardConfig::axp_power_key).
  void use_power_key();
  bool take_key_press() override { return key_ && chip_->take_key_press(); }
  void use_curves(hg::Curve discharge, hg::Curve charge) {
    if (chip_ && discharge) chip_->use_curves(discharge, charge);
  }
  std::optional<uint16_t> charge_current_ma() { return chip_ ? chip_->charge_current_ma() : std::nullopt; }
  std::optional<bool> external_power() override { return chip_->vbus_good(); }

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  std::unique_ptr<hg::Axp2101> chip_;
  bool key_ = false;
};

class CoreS3Board {
 public:
  bool begin(i2c_master_bus_handle_t bus);
  void set_backlight(uint8_t percent);

 private:
  i2c_master_dev_handle_t pmic_ = nullptr, expander_ = nullptr;
  std::unique_ptr<hg::CoreS3Control> control_;
};

class LatchPower final : public hg::Power {
 public:
  bool begin(const LatchPowerConfig& cfg);
  std::optional<hg::PowerStatus> read() override;
  bool can_power_off() const override { return cfg_.power_off_supported; }
  bool power_off() override;
  std::optional<bool> external_power() override;

 private:
  bool charging_signal() const;
  LatchPowerConfig cfg_{};
  adc_oneshot_unit_handle_t adc_ = nullptr;
  adc_cali_handle_t calibration_ = nullptr;
  adc_channel_t channel_ = ADC_CHANNEL_0;
};

class Buttons {
 public:
  void begin(const ButtonConfig& cfg);
  void poll(hg::App& app);  // call every ~10 ms from the app task

 private:
  struct Button {
    int gpio = -1;
    hg::Button id = hg::Button::Talk;
    bool pressed = false;
    uint8_t stable = 0;
  };
  Button buttons_[4];
};

// Over-the-air updates into the other app slot of partitions.csv. A new image
// boots on probation: it must reach Hermes (hg::App confirms it on `welcome`)
// within kConfirmWindowUs, or this rolls back to the previous one; a crash
// before then makes the bootloader roll back on its own.
class EspUpdater final : public hg::Updater {
 public:
  // Looks at the running image: if it is on probation, starts the rollback clock.
  void start();
  // The board name an image must carry (HGBOARD=<name>) to be installed.
  void expect_board(const char* name) { board_ = name ? name : ""; }
  size_t capacity() const override;
  bool begin(size_t size, std::string& error) override;
  bool write(const uint8_t* data, size_t len, std::string& error) override;
  bool finish(std::string& error) override;
  void abort() override;
  void restart() override;
  bool pending_verify() const override { return pending_; }
  void confirm() override;
  // Running and next slot, and whether this boot is on probation (for `diag`).
  hg::json::Value describe() const;

 private:
  static constexpr size_t kHeadBytes = 112;  // image + segment headers, then the app description up to its project name
  std::string board_;
  // Searches for the first 8 bytes of board_tag(): a separate "HGBOARD=" literal
  // here would put a nameless copy of the tag in every image, ahead of the real one.
  hg::TagScanner board_tag_{std::string_view()};
  const void* target_ = nullptr;             // esp_partition_t
  uint32_t handle_ = 0;                      // esp_ota_handle_t
  bool open_ = false;
  bool pending_ = false;
  size_t written_ = 0;
  uint8_t head_[kHeadBytes] = {};
  void* rollback_timer_ = nullptr;           // esp_timer_handle_t
};

class Wifi {
 public:
  void begin(NvsStorage& storage);
  void reconfigure();  // join the first saved network
  // The saved networks changed through the console: rejoin unless the device is on the first one.
  void networks_changed();
  void scan_done();
  // The network the device is on, or empty.
  std::string current() const { return link_up_ ? ssid_ : std::string(); }
  void disconnected();
  void connected(hg::App& app);
  void tick(hg::App& app, uint32_t now);
  // Retry at once instead of at the next backoff slot (the user is back).
  void retry_now();
  // Whether the device runs on its battery: the retry backoff and modem sleep (outside a turn)
  // apply only then.
  std::function<bool()> on_battery;
  std::string start_setup();
  void stop_setup();
  // The temporary network's credentials, for the setup screen's QR code.
  hg::WifiSetupAp setup_ap();
  void provision(const hg::WifiCredentials& credentials);

 private:
  static void on_event(void* arg, const char* base, int32_t id, void* data);
  static esp_err_t setup_http(httpd_req_t* request);
  void join(const std::string& ssid, const std::string& password);
  // With `aim`, the station joins that access point on its channel without scanning.
  bool configure(const std::string& ssid, const std::string& password, const hg::WifiFastAp* aim = nullptr);
  void attempt(uint32_t now);
  void remember_ap();
  void forget_unsaved_ap();
  void setup_status(std::string status);
  NvsStorage* storage_ = nullptr;
  bool configured_ = false;
  hg::WifiNetworks networks_;
  hg::WifiPick pick_;
  hg::WifiFastJoin fast_;
  bool aimed_ = false;      // the station config names one access point (fast_)
  bool static_ip_ = false;  // DHCP stopped for a reused lease (CONFIG_HG_WIFI_REUSE_IP)
  std::string ssid_, password_;  // what the station is configured for
  bool link_up_ = false, scanning_ = false;
  bool auto_setup_ = false, auto_setup_tried_ = false;
  bool joining_ = false, wait_disconnect_ = false;
  uint32_t trial_at_ = 0, setup_until_ = 0, close_at_ = 0;
  hg::WifiRetry retry_;
  esp_pm_lock_handle_t attempt_lock_ = nullptr;  // light sleep would stall an attempt
  bool attempt_held_ = false;
  void hold_while_attempting();
  hg::WifiPowerSave power_save_;
  void apply_power_save(uint32_t now, bool in_turn);
  hg::WifiCredentials candidate_{};
  httpd_handle_t http_ = nullptr;
  std::mutex setup_mutex_;
  std::string ap_name_, ap_password_, nonce_, setup_state_;
  bool accepting_setup_ = false;
};

namespace console {
// Starts the serial console REPL; lines are executed by hg::App::console on the app task.
void begin();
}

// A phone companion app over Bluetooth LE, on boards with BoardConfig::bluetooth
// (port_ble.cpp; docs/protocol.md#phone-app-over-bluetooth). The phone writes a
// console line and reads the reply as notifications. Only a phone paired with
// the passkey this device shows gets in, and pairing is accepted only while
// open_pairing() is in effect. Call from the app task; results come back as
// Phone events.
class PhoneLink {
 public:
  // Starts the Bluetooth stack. It advertises only while pairing is open or a
  // phone is paired. False when the build has no Bluetooth or it can't start.
  bool begin(const std::string& name);
  bool open_pairing();
  void close_pairing();
  void forget();  // removes every paired phone and drops a connected one
  void set_name(const std::string& name);
};

namespace diag {
// What came up at boot, for the boot summary and the `diag` report.
struct Parts {
  const char* display = "none";
  const char* mic = "none";
  const char* speaker = "none";
  bool touch = false;
  bool key = false;
  i2c_master_bus_handle_t i2c = nullptr;  // scanned by `diag`
};

// Starts keeping a RAM copy of recent log lines. Call first in app_main.
void begin();
void set_parts(const Parts& parts);
// Reset reason, build, memory and parts, logged once the drivers are up.
void log_boot_summary();
// The port's half of the console's `diag` report (hg::App::on_diag).
void report(hg::json::Value& r);
// The log copy, oldest line first (hg::App::recent_log).
std::string recent_log();
}  // namespace diag

}  // namespace hgp
