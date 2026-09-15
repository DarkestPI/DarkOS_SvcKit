#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace darkos {

enum class SerialElectrical {
  kTtl,
  kRs232,
  kRs485,
};

struct BoardSerialPort {
  std::string id;
  std::string device;
  SerialElectrical electrical = SerialElectrical::kTtl;
  std::vector<std::uint32_t> baudRates;
  std::string rs485DirectionGpio;
  bool exclusive = true;

  bool supportsBaud(std::uint32_t baud) const;
};

class BoardConfig {
public:
  static bool load(const std::string &path, BoardConfig &output,
                   std::string &error);

  const std::string &schemaVersion() const noexcept { return schemaVersion_; }
  const std::string &boardId() const noexcept { return boardId_; }
  const std::string &compatibleSoc() const noexcept { return compatibleSoc_; }
  const std::unordered_map<std::string, BoardSerialPort> &
  serialPorts() const noexcept {
    return serialPorts_;
  }
  const BoardSerialPort *findSerialPort(const std::string &id) const noexcept;

private:
  std::string schemaVersion_;
  std::string boardId_;
  std::string compatibleSoc_;
  std::unordered_map<std::string, BoardSerialPort> serialPorts_;
};

const char *serialElectricalName(SerialElectrical electrical) noexcept;

} // namespace darkos
