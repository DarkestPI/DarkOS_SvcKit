/**
 * @file wifi_manager.cpp
 * @brief WiFi 管理（wpa_supplicant）
 * @author your_name
 * @date 2026-09-16
 */

#include "network_interface.h"

#include <hardware/hardware.h>
#include <wifi/IWifi.h>

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace darkos::network {
namespace {

WifiSecurity convertSecurity(std::uint32_t security) {
  switch (security) {
  case WIFI_SECURITY_WPA2_PSK:
    return WifiSecurity::Wpa2Psk;
  case WIFI_SECURITY_WPA3_SAE:
    return WifiSecurity::Wpa3Sae;
  default:
    return WifiSecurity::Open;
  }
}

std::uint32_t convertSecurity(WifiSecurity security) {
  switch (security) {
  case WifiSecurity::Wpa2Psk:
    return WIFI_SECURITY_WPA2_PSK;
  case WifiSecurity::Wpa3Sae:
    return WIFI_SECURITY_WPA3_SAE;
  case WifiSecurity::Open:
    return WIFI_SECURITY_OPEN;
  }
  return WIFI_SECURITY_OPEN;
}

std::string formatBssid(const std::uint8_t *bssid) {
  char text[18]{};
  std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
                bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
  return text;
}

class PlatformWifiManager final : public WifiManager {
public:
  explicit PlatformWifiManager(wifi_device_t *device) noexcept : device_(device) {}
  ~PlatformWifiManager() override {
    if (device_ != nullptr)
      wifi_close(device_);
  }

  int enable() override {
    if (device_->ops->wifi_enable == nullptr)
      return -ENOTSUP;
    const int result = device_->ops->wifi_enable(device_);
    if (result == 0)
      enabled_ = true;
    return result;
  }

  int disable() override {
    if (device_->ops->wifi_disable == nullptr)
      return -ENOTSUP;
    const int result = device_->ops->wifi_disable(device_);
    if (result == 0)
      enabled_ = false;
    return result;
  }

  int scan(std::vector<WifiAccessPoint> &output, int timeoutMs) override {
    if (device_->ops->scan == nullptr)
      return -ENOTSUP;
    output.clear();
    struct Context {
      std::vector<WifiAccessPoint> *output;
    } context{&output};
    return device_->ops->scan(
        device_,
        [](void *opaque, const wifi_ap_info_t *input) -> int {
          if (opaque == nullptr || input == nullptr)
            return -EINVAL;
          auto &values = *static_cast<Context *>(opaque)->output;
          WifiAccessPoint value;
          value.ssid = input->ssid;
          value.bssid = formatBssid(input->bssid);
          value.rssi = input->rssi;
          value.frequencyMhz = input->frequency_mhz;
          value.security = convertSecurity(input->security);
          values.push_back(std::move(value));
          return 0;
        },
        &context, timeoutMs);
  }

  int connect(const WifiConnectionConfig &config) override {
    if (device_->ops->connect == nullptr)
      return -ENOTSUP;
    if (config.ssid.empty() || config.ssid.size() > WIFI_SSID_MAX_LEN ||
        config.passphrase.size() > WIFI_PSK_MAX_LEN)
      return -EINVAL;
    wifi_config_t native{};
    std::memcpy(native.ssid, config.ssid.data(), config.ssid.size());
    std::memcpy(native.psk, config.passphrase.data(), config.passphrase.size());
    native.security = convertSecurity(config.security);
    return device_->ops->connect(device_, &native);
  }

  int disconnect() override {
    return device_->ops->disconnect != nullptr
               ? device_->ops->disconnect(device_)
               : -ENOTSUP;
  }

  int status(WifiStatus &output) const override {
    if (!enabled_) {
      output = {};
      output.state = WifiState::Disabled;
      return 0;
    }
    auto getStatus = device_->ops->get_status != nullptr
                         ? device_->ops->get_status
                         : device_->ops->wifi_get_status;
    if (getStatus == nullptr)
      return -ENOTSUP;
    wifi_status_t native{};
    const int result = getStatus(device_, &native);
    if (result != 0)
      return result;
    output = {};
    switch (native.state) {
    case WIFI_STATE_CONNECTED:
      output.state = WifiState::Connected;
      break;
    case WIFI_STATE_CONNECTING:
      output.state = WifiState::Connecting;
      break;
    default:
      output.state = WifiState::Disconnected;
      break;
    }
    output.ssid = native.ssid;
    output.rssi = native.rssi;
    output.frequencyMhz = native.frequency_mhz;
    output.ipAddress = native.ip_addr;
    return 0;
  }

private:
  wifi_device_t *device_{nullptr};
  bool enabled_{false};
};

} // namespace

std::unique_ptr<WifiManager> WifiManager::create(std::string &error) {
  const hw_module_t *module = nullptr;
  int result = hw_get_module(WIFI_HARDWARE_MODULE_ID, &module);
  if (result != 0) {
    error = "load Wi-Fi HAL failed: " +
            std::string(std::strerror(std::abs(result)));
    return nullptr;
  }
  wifi_device_t *device = nullptr;
  result = wifi_open(module, &device);
  if (result != 0) {
    error = "open Wi-Fi HAL failed: " +
            std::string(std::strerror(std::abs(result)));
    return nullptr;
  }
  auto *manager = new (std::nothrow) PlatformWifiManager(device);
  if (manager == nullptr) {
    wifi_close(device);
    error = "out of memory creating Wi-Fi manager";
    return nullptr;
  }
  error.clear();
  return std::unique_ptr<WifiManager>(manager);
}

} // namespace darkos::network
