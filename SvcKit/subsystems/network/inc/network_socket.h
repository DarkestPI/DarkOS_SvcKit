/**
 * @file network_socket.h
 * @brief Socket 抽象接口（TCP/UDP/Unix）
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "network_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <sys/socket.h>

namespace darkos::network {

class Address {
public:
  Address() noexcept;

  static int resolve(const std::string &host, std::uint16_t port,
                     AddressFamily family, int socketType,
                     std::vector<Address> &output, std::string &error);
  static int fromIp(const std::string &ip, std::uint16_t port,
                    Address &output) noexcept;

  AddressFamily family() const noexcept;
  std::string ip() const;
  std::uint16_t port() const noexcept;
  bool valid() const noexcept { return length_ != 0; }

  const sockaddr *data() const noexcept;
  socklen_t size() const noexcept { return length_; }

private:
  friend class Socket;
  sockaddr_storage storage_{};
  socklen_t length_{0};
};

class Socket {
public:
  Socket() noexcept = default;
  explicit Socket(int fd) noexcept : fd_(fd) {}
  ~Socket();

  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&other) noexcept;
  Socket &operator=(Socket &&other) noexcept;

  static Socket tcp(AddressFamily family = AddressFamily::IPv4) noexcept;
  static Socket udp(AddressFamily family = AddressFamily::IPv4) noexcept;

  bool valid() const noexcept { return fd_ >= 0; }
  int nativeHandle() const noexcept { return fd_; }
  int release() noexcept;
  void close() noexcept;

  int setNonBlocking(bool enabled = true) noexcept;
  int setReuseAddress(bool enabled = true) noexcept;
  int setMulticastTtl(std::uint8_t ttl) noexcept;
  int bind(const Address &address) noexcept;
  int listen(int backlog = 16) noexcept;
  int connect(const Address &address) noexcept;
  Socket accept(Address *peer = nullptr) noexcept;

  std::ptrdiff_t send(const void *data, std::size_t size,
                      int flags = 0) noexcept;
  std::ptrdiff_t receive(void *data, std::size_t size,
                         int flags = 0) noexcept;
  std::ptrdiff_t sendTo(const void *data, std::size_t size,
                        const Address &destination, int flags = 0) noexcept;
  std::ptrdiff_t receiveFrom(void *data, std::size_t size, Address &source,
                             int flags = 0) noexcept;

  int localAddress(Address &output) const noexcept;
  int peerAddress(Address &output) const noexcept;

private:
  static Socket create(int domain, int type, int protocol) noexcept;
  int fd_{-1};
};

} // namespace darkos::network
