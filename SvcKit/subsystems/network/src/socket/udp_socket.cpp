/**
 * @file udp_socket.cpp
 * @brief UDP Socket 实现
 * @author your_name
 * @date 2026-09-16
 */

#include "network_socket.h"

#include <netinet/in.h>

#include <cerrno>

namespace darkos::network {

int Socket::setMulticastTtl(std::uint8_t ttl) noexcept {
  if (fd_ < 0)
    return -EBADF;
  return setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) == 0
             ? 0
             : -errno;
}

std::ptrdiff_t Socket::sendTo(const void *data, std::size_t size,
                              const Address &destination,
                              int flags) noexcept {
  if (fd_ < 0 || !destination.valid())
    return fd_ < 0 ? -EBADF : -EINVAL;
  const ssize_t result =
      ::sendto(fd_, data, size, flags | MSG_NOSIGNAL, destination.data(),
               destination.size());
  return result >= 0 ? result : -errno;
}

std::ptrdiff_t Socket::receiveFrom(void *data, std::size_t size,
                                   Address &source, int flags) noexcept {
  if (fd_ < 0)
    return -EBADF;
  Address address;
  address.length_ = sizeof(address.storage_);
  const ssize_t result =
      ::recvfrom(fd_, data, size, flags,
                 reinterpret_cast<sockaddr *>(&address.storage_),
                 &address.length_);
  if (result < 0)
    return -errno;
  source = address;
  return result;
}

} // namespace darkos::network
