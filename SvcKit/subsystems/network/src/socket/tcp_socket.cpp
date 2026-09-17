/**
 * @file tcp_socket.cpp
 * @brief TCP Socket 实现
 * @author your_name
 * @date 2026-09-16
 */

#include "network_socket.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace darkos::network {

namespace {

int socketDomain(AddressFamily family) noexcept {
  return family == AddressFamily::IPv6 ? AF_INET6 : AF_INET;
}

} // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket &&other) noexcept : fd_(other.release()) {}

Socket &Socket::operator=(Socket &&other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.release();
  }
  return *this;
}

Socket Socket::create(int domain, int type, int protocol) noexcept {
  int fd = ::socket(domain, type | SOCK_CLOEXEC, protocol);
  return Socket(fd);
}

Socket Socket::tcp(AddressFamily family) noexcept {
  return create(socketDomain(family), SOCK_STREAM, IPPROTO_TCP);
}

Socket Socket::udp(AddressFamily family) noexcept {
  return create(socketDomain(family), SOCK_DGRAM, IPPROTO_UDP);
}

int Socket::release() noexcept {
  const int result = fd_;
  fd_ = -1;
  return result;
}

void Socket::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int Socket::setNonBlocking(bool enabled) noexcept {
  if (fd_ < 0)
    return -EBADF;
  const int current = fcntl(fd_, F_GETFL, 0);
  if (current < 0)
    return -errno;
  const int updated = enabled ? current | O_NONBLOCK : current & ~O_NONBLOCK;
  return fcntl(fd_, F_SETFL, updated) == 0 ? 0 : -errno;
}

int Socket::setReuseAddress(bool enabled) noexcept {
  if (fd_ < 0)
    return -EBADF;
  const int value = enabled ? 1 : 0;
  return setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0
             ? 0
             : -errno;
}

int Socket::bind(const Address &address) noexcept {
  if (fd_ < 0 || !address.valid())
    return fd_ < 0 ? -EBADF : -EINVAL;
  return ::bind(fd_, address.data(), address.size()) == 0 ? 0 : -errno;
}

int Socket::listen(int backlog) noexcept {
  if (fd_ < 0)
    return -EBADF;
  return ::listen(fd_, backlog) == 0 ? 0 : -errno;
}

int Socket::connect(const Address &address) noexcept {
  if (fd_ < 0 || !address.valid())
    return fd_ < 0 ? -EBADF : -EINVAL;
  return ::connect(fd_, address.data(), address.size()) == 0 ? 0 : -errno;
}

Socket Socket::accept(Address *peer) noexcept {
  if (fd_ < 0) {
    errno = EBADF;
    return {};
  }
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  const int accepted = accept4(fd_, reinterpret_cast<sockaddr *>(&storage),
                               &length, SOCK_CLOEXEC);
  if (accepted < 0)
    return {};
  if (peer != nullptr) {
    peer->storage_ = storage;
    peer->length_ = length;
  }
  return Socket(accepted);
}

std::ptrdiff_t Socket::send(const void *data, std::size_t size,
                            int flags) noexcept {
  if (fd_ < 0)
    return -EBADF;
  const ssize_t result = ::send(fd_, data, size, flags | MSG_NOSIGNAL);
  return result >= 0 ? result : -errno;
}

std::ptrdiff_t Socket::receive(void *data, std::size_t size,
                               int flags) noexcept {
  if (fd_ < 0)
    return -EBADF;
  const ssize_t result = ::recv(fd_, data, size, flags);
  return result >= 0 ? result : -errno;
}

int Socket::localAddress(Address &output) const noexcept {
  if (fd_ < 0)
    return -EBADF;
  Address address;
  address.length_ = sizeof(address.storage_);
  if (getsockname(fd_, reinterpret_cast<sockaddr *>(&address.storage_),
                  &address.length_) != 0)
    return -errno;
  output = address;
  return 0;
}

int Socket::peerAddress(Address &output) const noexcept {
  if (fd_ < 0)
    return -EBADF;
  Address address;
  address.length_ = sizeof(address.storage_);
  if (getpeername(fd_, reinterpret_cast<sockaddr *>(&address.storage_),
                  &address.length_) != 0)
    return -errno;
  output = address;
  return 0;
}

} // namespace darkos::network
