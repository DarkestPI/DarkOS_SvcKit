#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace darkos::peripheral {

/** SvcKit serial facade. Platform HAL types are intentionally hidden. */
class SerialPort final {
public:
  static std::unique_ptr<SerialPort>
  open(const std::string &device, std::uint32_t baud, std::string &error);
  ~SerialPort();

  SerialPort(const SerialPort &) = delete;
  SerialPort &operator=(const SerialPort &) = delete;

  int fd() const noexcept;

private:
  class Impl;
  explicit SerialPort(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
};

} // namespace darkos::peripheral
