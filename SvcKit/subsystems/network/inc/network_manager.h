/**
 * @file network_manager.h
 * @brief 网络管理器门面
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "network_event.h"
#include "network_types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace darkos::network {

class NetworkManager {
public:
  using EventCallback = std::function<void(const NetworkEvent &)>;

  static std::unique_ptr<NetworkManager> create(EventLoop &loop,
                                                std::string &error);
  static int snapshot(std::vector<NetworkInterface> &output,
                      std::string &error);

  virtual ~NetworkManager() = default;
  NetworkManager(const NetworkManager &) = delete;
  NetworkManager &operator=(const NetworkManager &) = delete;

  virtual int start(EventCallback callback) = 0;
  virtual void stop() noexcept = 0;
  virtual bool running() const noexcept = 0;

protected:
  NetworkManager() = default;
};

} // namespace darkos::network
