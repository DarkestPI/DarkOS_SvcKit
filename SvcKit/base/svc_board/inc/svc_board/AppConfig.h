#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace darkos {

class BoardConfig;

struct SerialBinding {
  std::string service;
  std::string resource;
  std::uint32_t baud = 0;
};

struct RtspAuthenticationConfig {
  std::string username;
  std::string password;
  std::string passwordEnvironment{"DARKOS_RTSP_PASSWORD"};
};

struct RtspMulticastConfig {
  bool enabled{false};
  std::string address{"239.255.0.1"};
  std::uint16_t videoPort{5004};
  std::uint16_t audioPort{5006};
  std::uint8_t ttl{16};
};

struct RtspAppConfig {
  bool enabled{true};
  std::string bindAddress{"0.0.0.0"};
  std::uint16_t port{8554};
  std::string mountPath{"live"};
  std::uint32_t sessionTimeoutSeconds{60};
  std::uint32_t rtcpReportIntervalMs{5000};
  std::size_t maximumRtpPayloadBytes{1200};
  std::size_t maximumClientBacklogBytes{2 * 1024 * 1024};
  RtspAuthenticationConfig authentication;
  RtspMulticastConfig multicast;
};

/*
 * IVA 应用配置。
 *
 * 这里只保存业务层的“是否启用”和模型路径；HAL 变体、HAL 搜索目录以及
 * LD_LIBRARY_PATH 仍属于平台部署环境，不混入 app.json。
 */
struct IvaAppConfig {
  bool enabled{false};
  std::string modelPath;
};

/*
 * 本地显示配置。
 *
 * enabled 只表示应用是否请求平台建立 Camera->Display 预览；VO 设备、
 * layer、旋转和 panel 时序属于 Platform HAL，由具体 SoC 适配层决定。
 */
struct DisplayAppConfig {
  bool enabled{false};
};

class AppConfig {
public:
  static bool load(const std::string &path, AppConfig &output,
                   std::string &error);

  bool validate(const BoardConfig &board, std::string &error) const;

  const std::string &schemaVersion() const noexcept { return schemaVersion_; }
  const std::unordered_map<std::string, SerialBinding> &
  serialBindings() const noexcept {
    return serialBindings_;
  }
  const SerialBinding *
  findSerialBinding(const std::string &service) const noexcept;
  const RtspAppConfig &rtsp() const noexcept { return rtsp_; }
  const IvaAppConfig &iva() const noexcept { return iva_; }
  const DisplayAppConfig &display() const noexcept { return display_; }

private:
  std::string schemaVersion_;
  std::unordered_map<std::string, SerialBinding> serialBindings_;
  RtspAppConfig rtsp_;
  IvaAppConfig iva_;
  DisplayAppConfig display_;
};

} // namespace darkos
