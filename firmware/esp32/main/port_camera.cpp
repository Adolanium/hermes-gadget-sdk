// Still-image capture for the optional OV5647 on the Waveshare P4 7B.
// The camera and ISP produce RGB565; the gateway turns a compact frame into PNG.
#include "port_camera.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#include "driver/gpio.h"
#include "camera_rgb565.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "linux/videodev2.h"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.camera";
constexpr uint32_t kSensorWidth = 800;
constexpr uint32_t kSensorHeight = 640;
constexpr uint32_t kOutputWidth = 640;
constexpr uint32_t kOutputHeight = 512;
constexpr uint32_t kBufferCount = 2;
constexpr uint32_t kFrameTimeoutMs = 3000;
constexpr uint32_t kSettleFrames = 2;

struct MappedBuffer {
  void* address = MAP_FAILED;
  size_t length = 0;
};

std::string io_error(const char* operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

}  // namespace

bool CameraCapture::capture_rgb565(i2c_master_bus_handle_t sccb_bus, std::vector<uint8_t>& image,
                                   uint16_t& width, uint16_t& height, std::string& error) {
  if (!sccb_bus) {
    error = "camera SCCB bus is unavailable";
    return false;
  }
  if (!initialized_) {
    if (init_attempted_) {
      error = "camera initialization failed earlier; restart the gadget after checking the OV5647 module";
      return false;
    }
    init_attempted_ = true;
    esp_video_init_csi_config_t csi{};
    csi.sccb_config.init_sccb = false;
    csi.sccb_config.i2c_handle = sccb_bus;
    csi.sccb_config.freq = 100000;
    csi.reset_pin = GPIO_NUM_NC;
    csi.pwdn_pin = GPIO_NUM_NC;
    esp_video_init_config_t config{};
    config.csi = &csi;
    const esp_err_t init_err = esp_video_init(&config);
    if (init_err != ESP_OK) {
      esp_video_deinit();
      error = std::string("OV5647 MIPI-CSI initialization failed: ") + esp_err_to_name(init_err);
      ESP_LOGW(TAG, "%s", error.c_str());
      return false;
    }
    initialized_ = true;
    ESP_LOGI(TAG, "OV5647 MIPI-CSI initialized");
  }

  const int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    error = io_error("could not open the MIPI-CSI capture device; check that an OV5647 module is attached");
    return false;
  }

  std::array<MappedBuffer, kBufferCount> buffers{};
  v4l2_requestbuffers request{};
  bool buffers_requested = false;
  bool streaming = false;
  bool captured = false;
  const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  auto cleanup = [&]() {
    if (streaming) ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& buffer : buffers) {
      if (buffer.address != MAP_FAILED) munmap(buffer.address, buffer.length);
      buffer.address = MAP_FAILED;
      buffer.length = 0;
    }
    if (buffers_requested) {
      request.count = 0;
      ioctl(fd, VIDIOC_REQBUFS, &request);
    }
    close(fd);
  };
  auto fail = [&](std::string message) {
    error = std::move(message);
    cleanup();
    return false;
  };

  v4l2_capability capability{};
  if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0)
    return fail(io_error("camera capability query failed"));
  const uint32_t capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                                    ? capability.device_caps : capability.capabilities;
  if (!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) return fail("MIPI-CSI device has no capture capability");

  v4l2_format format{};
  format.type = type;
  format.fmt.pix.width = kSensorWidth;
  format.fmt.pix.height = kSensorHeight;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  format.fmt.pix.field = V4L2_FIELD_NONE;
  if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) return fail(io_error("could not set the camera RGB565 format"));
  if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 ||
      format.fmt.pix.width != kSensorWidth || format.fmt.pix.height != kSensorHeight) {
    return fail("camera did not provide the configured 800x640 RGB565 mode");
  }
  const size_t stride = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline
                                                    : static_cast<size_t>(format.fmt.pix.width) * 2;
  if (stride < static_cast<size_t>(format.fmt.pix.width) * 2 ||
      format.fmt.pix.height > std::numeric_limits<size_t>::max() / stride) {
    return fail("camera returned invalid RGB565 frame geometry");
  }

  request.count = kBufferCount;
  request.type = type;
  request.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &request) != 0) return fail(io_error("camera buffer allocation failed"));
  buffers_requested = true;
  if (request.count < kBufferCount) return fail("camera allocated too few capture buffers");

  for (uint32_t i = 0; i < kBufferCount; ++i) {
    v4l2_buffer buffer{};
    buffer.type = type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = i;
    if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) return fail(io_error("camera buffer query failed"));
    void* address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
    if (address == MAP_FAILED) return fail(io_error("camera buffer mapping failed"));
    buffers[i] = {address, buffer.length};
    if (ioctl(fd, VIDIOC_QBUF, &buffer) != 0) return fail(io_error("could not queue a camera buffer"));
  }

  int stream_type = type;
  if (ioctl(fd, VIDIOC_STREAMON, &stream_type) != 0) return fail(io_error("camera stream start failed"));
  streaming = true;

  uint32_t good_frames = 0;
  uint32_t attempts = 0;
  const uint32_t max_attempts = kSettleFrames + 6;
  while (good_frames <= kSettleFrames && attempts++ < max_attempts) {
    v4l2_buffer buffer{};
    buffer.type = type;
    buffer.memory = V4L2_MEMORY_MMAP;
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(kFrameTimeoutMs) * 1000;
    int rc = -1;
    do {
      rc = ioctl(fd, VIDIOC_DQBUF, &buffer);
      if (rc == 0 || (errno != EAGAIN && errno != EINTR)) break;
      if (esp_timer_get_time() >= deadline) {
        errno = ETIMEDOUT;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    } while (true);
    if (rc != 0) return fail(io_error("camera frame capture failed"));
    if (buffer.index >= buffers.size()) return fail("camera returned an invalid buffer index");

    const size_t needed = stride * format.fmt.pix.height;
    const bool frame_ok = !(buffer.flags & V4L2_BUF_FLAG_ERROR) &&
                          buffer.bytesused >= needed && buffers[buffer.index].length >= needed;
    if (frame_ok && good_frames == kSettleFrames) {
      image.resize(static_cast<size_t>(kOutputWidth) * kOutputHeight * 2);
      captured = downsample_rgb565(static_cast<const uint8_t*>(buffers[buffer.index].address),
                                   buffer.bytesused, format.fmt.pix.width, format.fmt.pix.height, stride,
                                   image.data(), image.size(), kOutputWidth, kOutputHeight);
    }
    if (ioctl(fd, VIDIOC_QBUF, &buffer) != 0) return fail(io_error("could not requeue a camera buffer"));
    if (!frame_ok) continue;
    if (good_frames == kSettleFrames) break;
    ++good_frames;
  }

  cleanup();
  streaming = false;
  buffers_requested = false;
  if (!captured) {
    error = "camera did not produce a usable frame";
    return false;
  }
  width = kOutputWidth;
  height = kOutputHeight;
  return true;
}

}  // namespace hgp
