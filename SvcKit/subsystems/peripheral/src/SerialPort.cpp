#include "peripheral/SerialPort.h"

#include <hardware/hardware.h>
#include <serial/ISerial.h>

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace darkos::peripheral {

class SerialPort::Impl final {
public:
  explicit Impl(serial_device_t *device) noexcept : device_(device) {}
  ~Impl() {
    if (device_ != nullptr)
      serial_close(device_);
  }

  int fd() const noexcept {
    return device_ != nullptr && device_->ops != nullptr &&
                   device_->ops->fd != nullptr
               ? device_->ops->fd(device_)
               : -1;
  }

private:
  serial_device_t *device_{nullptr};
};

SerialPort::SerialPort(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

SerialPort::~SerialPort() = default;

std::unique_ptr<SerialPort>
SerialPort::open(const std::string &path, std::uint32_t baud,
                 std::string &error) {
  if (path.empty() || baud == 0) {
    error = "serial device and baud are required";
    return nullptr;
  }
  const hw_module_t *module = nullptr;
  int result = hw_get_module(SERIAL_HARDWARE_MODULE_ID, &module);
  if (result != 0) {
    error = "load serial HAL failed: " +
            std::string(std::strerror(std::abs(result)));
    return nullptr;
  }
  serial_device_t *device = nullptr;
  result = serial_open(module, &device);
  if (result != 0) {
    error = "open serial HAL failed: " +
            std::string(std::strerror(std::abs(result)));
    return nullptr;
  }
  const serial_config_t config{path.c_str(), baud};
  result = device->ops->open(device, &config);
  if (result != 0) {
    serial_close(device);
    error = "configure serial device failed: " +
            std::string(std::strerror(std::abs(result)));
    return nullptr;
  }
  auto implementation = std::unique_ptr<Impl>(new (std::nothrow) Impl(device));
  if (!implementation) {
    serial_close(device);
    error = "out of memory creating serial port";
    return nullptr;
  }
  auto port = std::unique_ptr<SerialPort>(
      new (std::nothrow) SerialPort(std::move(implementation)));
  if (!port) {
    error = "out of memory creating serial port";
    return nullptr;
  }
  error.clear();
  return port;
}

int SerialPort::fd() const noexcept {
  return implementation_ != nullptr ? implementation_->fd() : -1;
}

} // namespace darkos::peripheral
