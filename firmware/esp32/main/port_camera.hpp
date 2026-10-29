#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "driver/i2c_master.h"

namespace hgp {

class CameraCapture {
 public:
  // Lazily initializes the optional OV5647 CSI pipeline and returns a compact
  // RGB565 still suitable for the gateway's bounded WebSocket action result.
  bool capture_rgb565(i2c_master_bus_handle_t sccb_bus, std::vector<uint8_t>& image,
                      uint16_t& width, uint16_t& height, std::string& error);

 private:
  bool init_attempted_ = false;
  bool initialized_ = false;
};

}  // namespace hgp
