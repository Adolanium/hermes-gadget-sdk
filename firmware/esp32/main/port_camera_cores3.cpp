// Still-image capture for the GC0308 on the M5Stack CoreS3, plus the
// hg::Camera implementation the camera screen drives: one esp32-camera
// instance, frame size switched between VGA (captures) and QVGA (the live
// preview), drawing straight into the panel framebuffer (the core's camera
// screen calls draw_preview from its render step, per the #294 plan). The
// sensor is clocked by its own on-board 20 MHz crystal (the ESP32 never
// drives XCLK — see the CoreS3 camera table on docs.m5stack.com), shares the
// internal I2C bus for SCCB, and produces RGB565 over the DVP. The camera's
// power/reset line lives on the AW9523B and is driven by
// CoreS3Board::camera_power() from main.cpp before the first capture.
#include "port_camera.hpp"

#include <cstring>
#include <utility>
#include <vector>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera_rgb565.hpp"
#include "hg/hal.hpp"

namespace hgp {

constexpr const char* TAG = "hg.camera";
constexpr int kSccbPort = 0;  // hgp::i2c::bus() installs the shared bus here

bool s_init_attempted = false;  // esp_camera_init() runs once per boot
bool s_ready = false;           // init succeeded; captures reuse the pipeline

bool begin_camera() {
  if (s_ready) return true;
  camera_config_t config = {};
  config.pin_pwdn = -1;   // power and reset live on the AW9523B, not on an ESP32 pin
  config.pin_reset = -1;
  // The sensor takes its clock from the on-board crystal, so the port never
  // generates XCLK; the pin table stays the one from docs.m5stack.com. The
  // crystal runs at 20 MHz, and the LCD_CAM divider needs that rate even with
  // no XCLK pin (a zero rate divides by zero in ll_cam_config).
  config.pin_xclk = -1;
  config.xclk_freq_hz = 20000000;
  // SCCB hangs off the internal shared bus (AXP2101 + AW9523 + camera); the
  // camera driver uses the already installed bus (sccb_i2c_port) instead of
  // installing a second one on the same pins.
  config.pin_sccb_sda = -1;
  config.pin_sccb_scl = -1;
  config.sccb_i2c_port = static_cast<i2c_port_t>(kSccbPort);
  config.pin_d7 = 47;
  config.pin_d6 = 48;
  config.pin_d5 = 16;
  config.pin_d4 = 15;
  config.pin_d3 = 42;
  config.pin_d2 = 41;
  config.pin_d1 = 40;
  config.pin_d0 = 39;
  config.pin_vsync = 46;
  config.pin_href = 38;
  config.pin_pclk = 45;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_VGA;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.jpeg_quality = 10;  // unused for RGB565; keeps the config well-formed

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "GC0308 ready: 640x480 RGB565 frame in PSRAM");
  return true;
}

bool CameraCapture::capture_rgb565(i2c_master_bus_handle_t sccb_bus, std::vector<uint8_t>& image,
                                   uint16_t& width, uint16_t& height, std::string& error) {
  if (!sccb_bus) {
    error = "camera SCCB bus is unavailable";
    return false;
  }
  if (!s_ready) {
    if (s_init_attempted) {
      error = "camera initialization failed earlier; restart the gadget after checking the GC0308 camera";
      return false;
    }
    s_init_attempted = true;
    if (!begin_camera()) {
      error = "GC0308 initialization failed; restart the gadget after checking the camera";
      return false;
    }
    s_ready = true;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    error = "camera did not produce a usable frame";
    return false;
  }
  if (fb->width > 640 || fb->height > 512 || fb->len > 640u * 512u * 2u) {
    esp_camera_fb_return(fb);
    error = "captured image exceeds the action-result limit";
    return false;
  }
  image.assign(fb->buf, fb->buf + fb->len);
  width = static_cast<uint16_t>(fb->width);
  height = static_cast<uint16_t>(fb->height);
  esp_camera_fb_return(fb);

  // The GC0308 transfers the high byte of each 16-bit pixel first, so the DVP
  // frame buffer is byte-swapped relative to the little-endian layout the
  // gateway documents for image_b64.
  if (!swap_rgb565_bytes(image.data(), image.size())) {
    error = "captured image had an odd pixel payload";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// hg::Camera for the CoreS3 camera screen (#294): the one camera instance runs
// at VGA and stays there — the sensor's frame buffers are allocated at VGA, and
// esp32-camera drops any frame smaller than its fb allocation, so switching to
// QVGA for the live view on the same instance yields no frames (found on
// hardware: every preview frame warned `cam_hal: FB-SIZE: 153600 != 614400`
// and none was queued). draw_preview instead blits each VGA frame scaled
// straight into the panel's window, which the capture path already uses.
// ---------------------------------------------------------------------------

void blit_preview(const uint8_t* source, uint32_t source_width, uint32_t source_height,
                  hg::Display& display, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  const hg::DisplayInfo info = display.info();
  uint16_t* framebuffer = display.framebuffer();
  if (framebuffer == nullptr || w == 0 || h == 0) return;
  if (x + w > info.width || y + h > info.height) return;
  // The sensor sends each pixel's high byte first, which is exactly the byte
  // order a panel that wants big-endian RGB565 (this CoreS3's) takes, so the raw
  // pairs go out as they are; a little-endian framebuffer gets each pair swapped
  // as it is copied. The core draws the captured photo through a Canvas, which
  // applies the same byte order from the little-endian layout capture() returns.
  const bool be = info.swap_bytes;
  for (uint16_t row = 0; row < h; ++row) {
    const uint32_t source_y = (static_cast<uint64_t>(row) * source_height) / h;
    const uint8_t* src = source + source_y * source_width * 2;
    uint16_t* dst = framebuffer + static_cast<uint32_t>(y + row) * info.width + x;
    for (uint16_t col = 0; col < w; ++col) {
      const uint32_t source_x = (static_cast<uint64_t>(col) * source_width) / w;
      const uint16_t raw = static_cast<uint16_t>(src[source_x * 2 + 1] << 8 | src[source_x * 2]);
      dst[col] = be ? raw : static_cast<uint16_t>(raw >> 8 | raw << 8);
    }
  }
  display.flush(y, y + h);
}

class CoreS3CameraHal final : public hg::Camera {
 public:
  void set_display(hg::Display* display) { display_ = display; }

  bool available() const override { return s_ready; }

  bool draw_preview(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override {
    if (display_ == nullptr || !s_ready || w == 0 || h == 0) return false;
    // Single-threaded: never wait on the driver. esp_camera_fb_get blocks up to
    // 4 s on an empty queue, which stalls the whole app and drops touch input;
    // the sensor outpaces the preview's ~10 fps cap, so an empty queue this tick
    // just means the next one draws.
    if (!esp_camera_available_frames()) return false;
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) return false;
    if (fb->len != static_cast<size_t>(fb->width) * fb->height * 2) {
      esp_camera_fb_return(fb);
      return false;
    }
    const uint16_t fw = fb->width, fh = fb->height;
    // The frame is painted before it goes back to the driver: no copy, and no
    // per-frame scratch buffer, at the preview's ~10 fps. The VGA frame scales
    // into whatever window the core gives.
    blit_preview(fb->buf, fw, fh, *display_, x, y, w, h);
    esp_camera_fb_return(fb);
    return true;
  }

  bool capture(Still& out, std::string& error) override {
    if (!s_ready) {
      error = "camera is not running";
      return false;
    }
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      error = "camera did not produce a usable frame";
      return false;
    }
    if (fb->width > 640 || fb->height > 512 || fb->len > 640u * 512u * 2u) {
      esp_camera_fb_return(fb);
      error = "captured image exceeds the action-result limit";
      return false;
    }
    out.pixels.assign(fb->buf, fb->buf + fb->len);
    out.width = static_cast<uint16_t>(fb->width);
    out.height = static_cast<uint16_t>(fb->height);
    esp_camera_fb_return(fb);
    if (!swap_rgb565_bytes(out.pixels.data(), out.pixels.size())) {
      error = "captured image had an odd pixel payload";
      return false;
    }
    return true;
  }

 private:
  hg::Display* display_ = nullptr;
};

CoreS3CameraHal g_cores3_camera_hal;

hg::Camera* camera_hal() { return &g_cores3_camera_hal; }

void camera_hal_set_display(hg::Display* display) { g_cores3_camera_hal.set_display(display); }

// Started at boot once the board's camera rails and reset line are up, so the
// camera mode's settings item and preview are ready without a capture first;
// the capture action then reuses the running pipeline. Once per boot.
bool camera_hal_begin() {
  if (s_ready) return true;
  if (s_init_attempted) return false;  // a failed start stays failed until restart
  s_init_attempted = true;
  if (!begin_camera()) return false;
  s_ready = true;
  return true;
}

}  // namespace hgp
