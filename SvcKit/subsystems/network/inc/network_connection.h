/**
 * @file network_connection.h
 * @brief 连接管理接口
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "network_event.h"
#include "network_socket.h"

#include <functional>
#include <memory>
#include <string>

namespace darkos::network {

/** EventLoop-backed non-blocking TCP acceptor. All callbacks run on the loop. */
class TcpServer {
public:
  using AcceptCallback = std::function<void(Socket, const Address &)>;

  static std::unique_ptr<TcpServer> create(EventLoop &loop,
                                           const Address &address,
                                           std::string &error);
  ~TcpServer();

  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;

  int start(AcceptCallback callback);
  void stop() noexcept;
  bool running() const noexcept;
  Address localAddress() const noexcept;

private:
  class Impl;
  explicit TcpServer(std::unique_ptr<Impl> implementation) noexcept;
  std::unique_ptr<Impl> implementation_;
};

} // namespace darkos::network
