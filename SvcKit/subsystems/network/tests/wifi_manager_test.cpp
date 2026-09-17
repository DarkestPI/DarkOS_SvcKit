#include "network_interface.h"

#include <memory>
#include <string>
#include <vector>

int main() {
  std::string error;
  auto wifi = darkos::network::WifiManager::create(error);
  if (!wifi || wifi->enable() != 0)
    return 1;
  std::vector<darkos::network::WifiAccessPoint> accessPoints;
  if (wifi->scan(accessPoints, 1000) != 0 || accessPoints.empty())
    return 1;
  darkos::network::WifiConnectionConfig config;
  config.ssid = "DarkOS-Office";
  config.passphrase = "test-password";
  config.security = darkos::network::WifiSecurity::Wpa2Psk;
  if (wifi->connect(config) != 0)
    return 1;
  darkos::network::WifiStatus status;
  if (wifi->status(status) != 0 ||
      status.state != darkos::network::WifiState::Connected ||
      status.ssid != config.ssid || status.ipAddress.empty())
    return 1;
  return wifi->disconnect() == 0 && wifi->disable() == 0 ? 0 : 1;
}
