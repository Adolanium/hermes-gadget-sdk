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
#include "cores3.hpp"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
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
                                 WifiStarted, WifiDisconnected, WifiProvision };

// Payloads of Touch and Key events (posted by the input task).
struct TouchSample {
  bool touching;
  int16_t x, y;
};
struct KeySample {
  bool pressed;
};

struct ConsoleRequest {
  SemaphoreHandle_t done;
  std::string reply;
};

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
  void set(std::string_view key, std::string_view value) override;
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
  uint32_t generation() const { return generation_.load(); }

 private:
  static void on_event(void* arg, const char* base, int32_t id, void* data);
  esp_websocket_client_handle_t client_ = nullptr;
  std::atomic<uint32_t> generation_{0};
  std::string url_, subprotocol_;
  std::string rx_;  // fragment reassembly (WebSocket task only)
  uint8_t rx_opcode_ = 0;
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
  int bounce_rows_ = 0;
  SemaphoreHandle_t done_ = nullptr;
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
};

namespace i2c {
// The board's shared I2C master bus (created on first use).
i2c_master_bus_handle_t bus(const I2cBusConfig& cfg);
}

// ES8311 + ES7210 on one duplex I2S bus through esp_codec_dev. Both directions
// run at one fixed rate (they share the bit clock).
class CodecAudio {
 public:
  static constexpr uint32_t kRate = 16000;
  bool begin(const CodecAudioConfig& cfg, i2c_master_bus_handle_t bus);
  esp_codec_dev_handle_t out() const { return out_; }
  esp_codec_dev_handle_t in() const { return in_; }

 private:
  i2s_chan_handle_t tx_ = nullptr, rx_ = nullptr;
  esp_codec_dev_handle_t out_ = nullptr, in_ = nullptr;
};

class CodecMic final : public hg::AudioIn {
 public:
  bool begin(esp_codec_dev_handle_t dev);
  bool start(uint32_t sample_rate) override;
  void stop() override { capturing_ = false; }

 private:
  static void task(void* arg);
  esp_codec_dev_handle_t dev_ = nullptr;
  std::atomic<bool> capturing_{false};
};

class CodecSpeaker final : public hg::AudioOut {
 public:
  bool begin(esp_codec_dev_handle_t dev);
  bool begin(uint32_t sample_rate) override;
  void write(const int16_t* samples, size_t count) override;
  void end() override;
  void abort() override;
  bool busy() const override;
  void set_volume(uint8_t percent) override;

 private:
  static void task(void* arg);
  esp_codec_dev_handle_t dev_ = nullptr;
  StreamBufferHandle_t buffer_ = nullptr;
  std::atomic<bool> open_{false};
  std::atomic<bool> draining_{false};
  std::atomic<bool> flush_{false};
};

// Polls a CST9217 touchscreen and a TCA9554-mirrored key on the I2C bus from
// its own task (the controller needs a pause between write and read) and
// posts Touch and Key events to the app task.
class TouchInput {
 public:
  bool begin(const TouchConfig& touch, const ExpanderKeyConfig& key, i2c_master_bus_handle_t bus);
  bool has_touch() const { return touch_dev_ != nullptr || managed_touch_ != nullptr; }
  bool has_key() const { return key_dev_ != nullptr; }

 private:
  static void task(void* arg);
  bool read_touch(TouchSample& out);
  bool begin_box_touch(i2c_master_bus_handle_t bus);
  bool read_key(bool& pressed);
  TouchConfig touch_{};
  ExpanderKeyConfig key_{};
  i2c_master_dev_handle_t touch_dev_ = nullptr;
  i2c_master_dev_handle_t key_dev_ = nullptr;
  esp_lcd_touch_handle_t managed_touch_ = nullptr;
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
