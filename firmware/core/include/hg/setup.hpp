#pragma once

#include <string>
#include <string_view>

namespace hg {

// Fixed-size payload copied through the port's event queue after validation.
struct WifiCredentials {
  char ssid[33] = {};
  char password[65] = {};
  char server[201] = {};
};

bool parse_wifi_setup(std::string_view body, std::string_view nonce, WifiCredentials& out, std::string& error);

}  // namespace hg
