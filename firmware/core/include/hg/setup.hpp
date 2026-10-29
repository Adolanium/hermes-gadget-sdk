#pragma once

#include <string>
#include <string_view>

namespace hg {

// Fixed-size payload copied through the port's event queue after validation.
struct WifiCredentials {
  char ssid[33] = {};
  char password[65] = {};
  char server[201] = {};
  // Tailnet (DeviceProfile::tailnet), only when the page sent it: whether to reach
  // Hermes over the tailnet, and a new auth key (empty: keep the saved one).
  bool tailnet_given = false;
  bool tailnet_on = false;
  char ts_key[121] = {};
};

// A Tailscale auth key as the admin console prints it: tskey-<kind>-<id>-<secret>.
bool valid_tailnet_key(std::string_view key);

bool parse_wifi_setup(std::string_view body, std::string_view nonce, WifiCredentials& out, std::string& error);

}  // namespace hg
