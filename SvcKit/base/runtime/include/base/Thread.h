#pragma once

#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace darkos {

class Thread {
public:
  template <typename Function>
  Thread(std::string name, Function &&function)
      : name_(std::move(name)), thread_(std::forward<Function>(function)) {}

  ~Thread() {
    if (thread_.joinable())
      thread_.join();
  }

  Thread(const Thread &) = delete;
  Thread &operator=(const Thread &) = delete;
  Thread(Thread &&) = default;
  Thread &operator=(Thread &&) = default;

  void join() {
    if (thread_.joinable())
      thread_.join();
  }
  bool joinable() const noexcept { return thread_.joinable(); }
  const std::string &name() const noexcept { return name_; }

private:
  std::string name_;
  std::thread thread_;
};

} // namespace darkos
