// Still-image capture for the GC0308 on the M5Stack CoreS3. The sensor is
// clocked by its own on-board 20 MHz crystal (the ESP32 never drives XCLK —
// see the CoreS3 camera table on docs.m5stack.com), shares the internal I2C
// bus for SCCB, and produces RGB565 over the DVP. The gateway turns a frame
// into PNG. The camera's power/reset line lives on the AW9523B and is driven
// by CoreS3Board::camera_power() from main.cpp before the first capture.
#include "port_camera.hpp"

#include <cstring>
#include <utility>

#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"

#include "camera_rgb565.hpp"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.camera";
constexpr int kSccbPort = 0;  // hgp::i2c::bus() installs the shared bus here

bool s_init_attempted = false;  // esp_camera_init() runs once per boot
bool s_ready = false;

bool begin_camera() {
  camera_config_t config = {};
  config.pin_pwdn = -1;   // power and reset live on the AW9523B, not on an ESP32 pin
  config.pin_reset = -1;
  // The sensor takes its clock from the on-board crystal, so the port never
  // generates XCLK; the pin table stays the one from docs.m5stack.com. The
  // crystal runs at 20 MHz, and the LCD_CAM divider needs that rate even with
  // no XCLK pin (a zero rate divides by zero in ll_cam_config).
  config.pin_xclk = -1;
  config.xclk_freq_hz = 20000000;
  // SCCB hangs off the internal shared bus (AXP2101 + AW9523 + camera); tell
  // the camera driver to use the already installed bus instead of installing
  // a second one on the same pins.
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
  config.fb_count = 1;
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

}  // namespace

bool CameraCapture::capture_rgb565(i2c_master_bus_handle_t sccb_bus, std::vector<uint8_t>& image,
                                   uint16_t& width, uint16_t& height, std::string& error) {
  if (!sccb_bus) {
    error = "camera SCCB bus is unavailable";
    return false;
  }
  if (!s_ready) {
    // A second esp_camera_init() fails its probe (the driver is already set up) and then
    // tears the running camera down, so the camera is started once and reused after.
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

}  // namespace hgp
