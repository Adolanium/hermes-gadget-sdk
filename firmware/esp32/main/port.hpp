// ESP32 implementations of the core HAL, plus the event queue that moves
// driver events (Wi-Fi, WebSocket, microphone, console) onto the app task.
//
// Threading rule: hg::App is only touched by the app task (app_main's loop).
// Driver tasks and callbacks post Events; the loop dispatches them.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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
#include "tag_scanner.hpp"
#include "ws_link.hpp"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_http_server.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "hg/app.hpp"
#include "hg/hal.hpp"
#include "hg/setup.hpp"

namespace hgp {

// ---------------------------------------------------------------------------
// Events

enum class EventType : uint8_t { NetUp, NetDown, WsOpen, WsText, WsBinary, WsClosed, Mic, Console, Touch, Key,
                                 WifiStarted, WifiDisconnected, WifiProvision, Encoder };

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
  void random_bytes(uint8_t* out, size_t len) override;
  void log(hg::LogLevel level, std::string_view message) override;
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

 private:
  static void on_event(void* arg, const char* base, int32_t id, void* data);
  // Sends run on their own task, so a slow or dead link never stalls the app
  // task (and with it the screen): send_*() only queue a copy.
  static void tx_task(void* arg);
  bool enqueue(uint8_t opcode, const void* data, size_t len);
  esp_websocket_client_handle_t client_ = nullptr;
  QueueHandle_t tx_ = nullptr;
  hg::ws::Generations gens_;
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
  std::atomic<bool> capturing_{false};
};

// I2S amplifier fed from a stream buffer by a writer task.
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
  static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx);
  void command(uint8_t cmd, const uint8_t* data, size_t len);
  AmoledConfig cfg_{};
  esp_lcd_panel_io_handle_t io_ = nullptr;
  uint16_t* fb_ = nullptr;
  uint16_t* bounce_ = nullptr;
  SemaphoreHandle_t done_ = nullptr;
  std::optional<hg::BandFlush> bands_;
};

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

 private:
  i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
  esp_codec_dev_handle_t out_ = nullptr, in_ = nullptr;
};

class CodecMic final : public hg::AudioIn {
 public:
  bool begin(esp_codec_dev_handle_t dev, bool rmnm);
  bool start(uint32_t sample_rate) override;
  void stop() override { capturing_ = false; }

 private:
  static void task(void* arg);
  esp_codec_dev_handle_t dev_ = nullptr;
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

 private:
  static void task(void* arg);
  esp_codec_dev_handle_t dev_ = nullptr;
  bool stereo32_ = false;
  int32_t* stereo_ = nullptr;
  std::optional<hg::SpeakerPa> pa_;  // set when this speaker, not esp_codec_dev, drives the PA pin
  StreamBufferHandle_t buffer_ = nullptr;
  std::atomic<bool> open_{false};
  std::atomic<bool> draining_{false};
  std::atomic<bool> flush_{false};
  std::atomic<uint32_t> drop_bytes_{0};  // cumulative whole-frame overflow drops, reported at stream end
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
  bool begin_box_touch(i2c_master_bus_handle_t bus);
  bool read_key(bool& pressed);
  bool sample_encoder(int& direction);
  TouchConfig touch_{};
  ExpanderKeyConfig key_{};
  i2c_master_dev_handle_t touch_dev_ = nullptr;
  i2c_master_dev_handle_t key_dev_ = nullptr;
  esp_lcd_touch_handle_t managed_touch_ = nullptr;
  gpio_num_t encoder_a_ = GPIO_NUM_NC;
  gpio_num_t encoder_b_ = GPIO_NUM_NC;
  uint8_t encoder_state_ = 0;
  int8_t encoder_accumulator_ = 0;
};

class AxpPower final : public hg::Power {
 public:
  bool begin(i2c_master_bus_handle_t bus);
  bool enable_audio_supply() { return chip_ && chip_->enable_aldo1_3v3(); }
  std::optional<hg::PowerStatus> read() override { return chip_->read(); }
  bool power_off() override { return chip_->power_off(); }

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  std::unique_ptr<hg::Axp2101> chip_;
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

 private:
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
  void reconfigure();  // credentials changed through the console
  void disconnected();
  void connected(hg::App& app);
  void tick(hg::App& app, uint32_t now);
  std::string start_setup();
  void stop_setup();
  // The temporary network's credentials, for the setup screen's QR code.
  hg::WifiSetupAp setup_ap();
  void provision(const hg::WifiCredentials& credentials);

 private:
  static void on_event(void* arg, const char* base, int32_t id, void* data);
  static esp_err_t setup_http(httpd_req_t* request);
  void join(const char* ssid, const char* password);
  void setup_status(std::string status);
  NvsStorage* storage_ = nullptr;
  bool configured_ = false;
  bool auto_setup_ = false, auto_setup_tried_ = false;
  bool joining_ = false, wait_disconnect_ = false;
  uint32_t retry_at_ = 0, trial_at_ = 0, setup_until_ = 0, close_at_ = 0;
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
