/**
 * @file tcp_server.cpp
 * @brief TCP 服务端
 * @author your_name
 * @date 2026-09-16
 */

#include "network_connection.h"

#include <sys/epoll.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace darkos::network {

class TcpServer::Impl {
public:
  Impl(EventLoop &loop, Socket socket, Address local) noexcept
      : loop_(loop), socket_(std::move(socket)), local_(local) {}

  ~Impl() { stop(); }

  int start(AcceptCallback callback) {
    if (running_)
      return -EALREADY;
    if (!callback)
      return -EINVAL;
    callback_ = std::move(callback);
    running_ = true;
    if (!rearm()) {
      running_ = false;
      callback_ = {};
      return -EIO;
    }
    return 0;
  }

  void stop() noexcept {
    if (!running_ && !socket_.valid())
      return;
    running_ = false;
    if (socket_.valid())
      loop_.unwatchFd(socket_.nativeHandle());
    socket_.close();
    callback_ = {};
  }

  bool running() const noexcept { return running_; }
  Address localAddress() const noexcept { return local_; }

private:
  bool rearm() {
    return socket_.valid() &&
           loop_.watchFd(socket_.nativeHandle(), EPOLLIN,
                         [this](std::uint32_t events) { onReady(events); });
  }

  void onReady(std::uint32_t events) {
    if (!running_)
      return;
    if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
      stop();
      return;
    }
    for (;;) {
      Address peer;
      Socket connection = socket_.accept(&peer);
      if (!connection.valid()) {
        if (errno == EINTR)
          continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          stop();
        break;
      }
      connection.setNonBlocking();
      try {
        callback_(std::move(connection), peer);
      } catch (...) {
        // The moved connection closes when the callback stack unwinds.
      }
      if (!running_)
        return;
    }
    if (running_ && !rearm())
      stop();
  }

  EventLoop &loop_;
  Socket socket_;
  Address local_;
  AcceptCallback callback_;
  bool running_{false};
};

std::unique_ptr<TcpServer> TcpServer::create(EventLoop &loop,
                                             const Address &address,
                                             std::string &error) {
  if (!address.valid() || (address.family() != AddressFamily::IPv4 &&
                           address.family() != AddressFamily::IPv6)) {
    error = "TCP server requires an IPv4 or IPv6 address";
    return nullptr;
  }
  Socket socket = Socket::tcp(address.family());
  if (!socket.valid()) {
    error = "socket: " + std::string(std::strerror(errno));
    return nullptr;
  }
  int rc = socket.setReuseAddress();
  if (rc == 0)
    rc = socket.setNonBlocking();
  if (rc == 0)
    rc = socket.bind(address);
  if (rc == 0)
    rc = socket.listen();
  if (rc != 0) {
    error = "TCP server setup failed: " + std::string(std::strerror(-rc));
    return nullptr;
  }
  Address local;
  rc = socket.localAddress(local);
  if (rc != 0) {
    error = "getsockname failed: " + std::string(std::strerror(-rc));
    return nullptr;
  }
  error.clear();
  auto implementation =
      std::make_unique<Impl>(loop, std::move(socket), local);
  return std::unique_ptr<TcpServer>(
      new TcpServer(std::move(implementation)));
}

TcpServer::TcpServer(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

TcpServer::~TcpServer() = default;

int TcpServer::start(AcceptCallback callback) {
  return implementation_->start(std::move(callback));
}

void TcpServer::stop() noexcept { implementation_->stop(); }

bool TcpServer::running() const noexcept {
  return implementation_->running();
}

Address TcpServer::localAddress() const noexcept {
  return implementation_->localAddress();
}

} // namespace darkos::network
