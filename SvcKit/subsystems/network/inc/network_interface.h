/**
 * @file network_interface.h
 * @brief 网络接口与连接管理接口（有线/WiFi/4G）
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace darkos::network {

enum class WifiSecurity { Open, Wpa2Psk, Wpa3Sae };

struct WifiAccessPoint {
  std::string ssid;
  std::string bssid;
  std::int32_t rssi{0};
  std::uint32_t frequencyMhz{0};
  WifiSecurity security{WifiSecurity::Open};
};

struct WifiConnectionConfig {
  std::string ssid;
  std::string passphrase;
  WifiSecurity security{WifiSecurity::Wpa2Psk};
};

enum class WifiState { Disabled, Disconnected, Connecting, Connected };

struct WifiStatus {
  WifiState state{WifiState::Disabled};
  std::string ssid;
  std::int32_t rssi{0};
  std::uint32_t frequencyMhz{0};
  std::string ipAddress;
};

class WifiManager {
public:
  static std::unique_ptr<WifiManager> create(std::string &error);
  virtual ~WifiManager() = default;

  WifiManager(const WifiManager &) = delete;
  WifiManager &operator=(const WifiManager &) = delete;

  virtual int enable() = 0;
  virtual int disable() = 0;
  virtual int scan(std::vector<WifiAccessPoint> &output,
                   int timeoutMs = 5000) = 0;
  virtual int connect(const WifiConnectionConfig &config) = 0;
  virtual int disconnect() = 0;
  virtual int status(WifiStatus &output) const = 0;

protected:
  WifiManager() = default;
};

} // namespace darkos::network
