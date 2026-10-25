// Hardware abstraction layer.
//
// The core never touches drivers directly. A port (ESP32, the desktop
// simulator, a future board family) implements these interfaces and feeds
// asynchronous events back through hg::App's on_* methods, always from the
// thread that calls App::tick().
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hg {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

struct DisplayInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  // Store RGB565 pixels byte-swapped (big-endian), as most SPI panels expect.
  // The canvas writes pre-swapped colours so flushing is a plain copy.
  bool swap_bytes = false;
  bool has_backlight = false;
  // A circular panel (width == height). The UI keeps to the square inscribed
  // in the circle and leaves the rest dark.
  bool round = false;
  // Multiplier on the UI's automatic font scale, for panels small enough that
  // the automatic pick reads as too small. The UI clamps the final scale.
  uint8_t font_scale = 1;
  // Extra left and right padding for the top bar's text, for a rectangular
  // panel with rounded corners.
  uint8_t corner_inset = 0;
  // An OLED/AMOLED panel: unlit pixels draw no power, so the background is
  // true black instead of the near-black used on backlit LCDs.
  bool emissive = false;
};

// An RGB565 framebuffer owned by the port (PSRAM on hardware). On a board that
// cannot afford one for the whole panel it holds a band of rows instead; the UI
// renders in bands of that size (see framebuffer_rows).
class Display {
 public:
  virtual ~Display() = default;
  virtual DisplayInfo info() const = 0;
  virtual uint16_t* framebuffer() = 0;
  // Push rows [y0, y1) to the panel. Rows are contiguous in the framebuffer.
  virtual void flush(uint16_t y0, uint16_t y1) = 0;
  virtual void set_backlight(uint8_t percent) { (void)percent; }
  // How many panel rows the framebuffer holds at once, or 0 when it holds the
  // whole panel. A board with no PSRAM reports a band here: the UI then draws
  // every changed band once per window (see set_frame_origin), so nothing has
  // to change in the drawing code.
  virtual int framebuffer_rows() const { return 0; }
  // Maps the framebuffer onto panel rows [y, y + framebuffer_rows()) for draws
  // and flushes until it is moved again. Ignored when framebuffer_rows() is 0.
  virtual void set_frame_origin(int y) { (void)y; }
  // True once when the panel can no longer be trusted to show what was flushed
  // (a transfer was lost and this display cannot hold the rows to resend them).
  // The UI answers it by redrawing the whole screen.
  virtual bool take_redraw_request() { return false; }
};

// Microphone: after start() succeeds, the port delivers mono PCM16 samples at
// `sample_rate` via App::on_mic_samples() until stop().
class AudioIn {
 public:
  virtual ~AudioIn() = default;
  virtual bool start(uint32_t sample_rate) = 0;
  virtual void stop() = 0;
};

// Speaker: begin() a stream, write() mono PCM16 (non-blocking, the port
// buffers), end() marks end-of-stream. busy() stays true until the buffered
// audio has played out.
class AudioOut {
 public:
  virtual ~AudioOut() = default;
  virtual bool begin(uint32_t sample_rate) = 0;
  virtual void write(const int16_t* samples, size_t count) = 0;
  virtual void end() = 0;
  // Drop anything buffered and stop immediately (barge-in, cancel).
  virtual void abort() = 0;
  virtual bool busy() const = 0;
  virtual void set_volume(uint8_t percent) { (void)percent; }
};

// Decodes compressed speaker audio into mono PCM16. Optional: a device without
// one announces and plays PCM16 only. Each binary audio frame carries one packet.
class AudioDecoder {
 public:
  virtual ~AudioDecoder() = default;
  virtual const char* format() const = 0;  // the protocol name, e.g. "opus"
  // A new stream at this rate; false when the rate is unsupported.
  virtual bool begin(uint32_t sample_rate) = 0;
  // One packet in; returns the samples written to pcm, or -1 for a packet it cannot decode.
  virtual int decode(const uint8_t* packet, size_t len, int16_t* pcm, size_t max_samples) = 0;
};

// Message transport (a WebSocket on every current port). connect() is
// asynchronous: the port later calls App::on_transport_open() or
// App::on_transport_closed().
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void connect(const std::string& url, const std::string& subprotocol) = 0;
  virtual bool send_text(std::string_view text) = 0;
  virtual bool send_binary(const uint8_t* data, size_t len) = 0;
  virtual void close() = 0;
};

// Small persistent key/value store (NVS on ESP32).
class Storage {
 public:
  virtual ~Storage() = default;
  virtual std::optional<std::string> get(std::string_view key) = 0;
  // False when the value could not be saved (the key stays as it was).
  virtual bool set(std::string_view key, std::string_view value) = 0;
  virtual void erase(std::string_view key) = 0;
};

// Local calendar time. The port knows the timezone.
struct WallTime {
  uint16_t year = 0;
  uint8_t month = 1, day = 1;  // 1-based
  uint8_t weekday = 0;         // 0 = Sunday
  uint8_t hour = 0, minute = 0, second = 0;
};

class System {
 public:
  virtual ~System() = default;
  virtual uint32_t now_ms() = 0;
  // A monotonic microsecond clock, read only while frame timing is on
  // (hg::FrameStats). Override it where the port has a finer clock than now_ms.
  virtual uint64_t now_us() { return static_cast<uint64_t>(now_ms()) * 1000; }
  virtual void random_bytes(uint8_t* out, size_t len) = 0;
  virtual void log(LogLevel level, std::string_view message) = 0;
  // Dozing: the screen is off, nothing is happening and the device runs on its
  // battery. The port may let the chip sleep between events; input, the network
  // and the power key still reach the app, only less often (App::kDozeWaitMs).
  virtual void set_dozing(bool dozing) { (void)dozing; }
  // Local time of day. False until the port knows it (network time or a
  // battery-backed clock); the clock face stays off until then.
  virtual bool wall_time(WallTime& out) { (void)out; return false; }
};

// The slot a firmware update is written to. The core authorizes and checks the
// image (size, SHA-256, the server's MAC) and streams it in; the port owns the
// flash layout, the image format check and the restart.
class Updater {
 public:
  virtual ~Updater() = default;
  // The largest image the slot holds.
  virtual size_t capacity() const = 0;
  // Prepares the slot for `size` bytes. On failure, says why in `error`.
  virtual bool begin(size_t size, std::string& error) = 0;
  virtual bool write(const uint8_t* data, size_t len, std::string& error) = 0;
  // Checks the complete image and makes it the one to boot next.
  virtual bool finish(std::string& error) = 0;
  virtual void abort() = 0;
  // Restarts into the new image, after finish().
  virtual void restart() = 0;
  // This boot runs a newly installed image that hasn't proven itself yet:
  // confirm() keeps it; without that the port goes back to the previous one.
  virtual bool pending_verify() const { return false; }
  virtual void confirm() {}
};

struct PowerStatus {
  std::optional<bool> battery_present;
  std::optional<uint16_t> battery_mv;
  std::optional<uint8_t> battery_percent;
  std::optional<bool> charging;
  std::optional<bool> external_power;
};

class Power {
 public:
  virtual ~Power() = default;
  // A failed read must return nullopt, never the previous reading.
  virtual std::optional<PowerStatus> read() = 0;
  // Override when this controls a peripheral rail rather than device shutdown.
  virtual bool can_power_off() const { return true; }
  virtual bool power_off() = 0;
  // True once per short press of a power key only the PMIC can see. Polled.
  virtual bool take_key_press() { return false; }
  // Whether external (USB) power is present right now: a cheap read, polled every
  // 100 ms while the screen is dark. Unknown counts as plugged in, so Wi-Fi
  // retries keep their shortest interval and the app never dozes.
  virtual std::optional<bool> external_power() { return std::nullopt; }
};

// Everything except `system` and `transport` may be null when the board lacks it.
struct Hal {
  System* system = nullptr;
  Transport* transport = nullptr;
  Storage* storage = nullptr;
  Display* display = nullptr;
  AudioIn* mic = nullptr;
  AudioOut* speaker = nullptr;
  AudioDecoder* speaker_decoder = nullptr;  // optional compressed speaker audio
  Updater* updater = nullptr;
  Power* power = nullptr;
};

}  // namespace hg
