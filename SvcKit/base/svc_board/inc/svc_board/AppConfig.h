#pragma once

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

private:
  std::string schemaVersion_;
  std::unordered_map<std::string, SerialBinding> serialBindings_;
};

} // namespace darkos
