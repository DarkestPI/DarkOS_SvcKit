/**
 * @file address.cpp
 * @brief 地址解析与转换
 * @author your_name
 * @date 2026-09-16
 */

#include "network_socket.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <cerrno>
#include <cstring>

namespace darkos::network {

namespace {

int nativeFamily(AddressFamily family) noexcept {
  switch (family) {
  case AddressFamily::IPv4:
    return AF_INET;
  case AddressFamily::IPv6:
    return AF_INET6;
  case AddressFamily::Unix:
    return AF_UNIX;
  case AddressFamily::Unspecified:
    return AF_UNSPEC;
  }
  return AF_UNSPEC;
}

} // namespace

Address::Address() noexcept = default;

int Address::resolve(const std::string &host, std::uint16_t port,
                     AddressFamily family, int socketType,
                     std::vector<Address> &output, std::string &error) {
  output.clear();
  addrinfo hints{};
  hints.ai_family = nativeFamily(family);
  hints.ai_socktype = socketType;
  hints.ai_flags = AI_ADDRCONFIG;
  addrinfo *results = nullptr;
  const std::string service = std::to_string(port);
  const int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(),
                             service.c_str(), &hints, &results);
  if (rc != 0) {
    error = gai_strerror(rc);
    return rc == EAI_SYSTEM ? -errno : -EINVAL;
  }
  for (const addrinfo *entry = results; entry != nullptr;
       entry = entry->ai_next) {
    if (entry->ai_addrlen > sizeof(sockaddr_storage))
      continue;
    Address address;
    std::memcpy(&address.storage_, entry->ai_addr, entry->ai_addrlen);
    address.length_ = static_cast<socklen_t>(entry->ai_addrlen);
    output.push_back(address);
  }
  freeaddrinfo(results);
  if (output.empty()) {
    error = "name resolved without usable addresses";
    return -EADDRNOTAVAIL;
  }
  error.clear();
  return 0;
}

int Address::fromIp(const std::string &ip, std::uint16_t port,
                    Address &output) noexcept {
  Address parsed;
  sockaddr_in ipv4{};
  ipv4.sin_family = AF_INET;
  ipv4.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &ipv4.sin_addr) == 1) {
    std::memcpy(&parsed.storage_, &ipv4, sizeof(ipv4));
    parsed.length_ = sizeof(ipv4);
    output = parsed;
    return 0;
  }
  sockaddr_in6 ipv6{};
  ipv6.sin6_family = AF_INET6;
  ipv6.sin6_port = htons(port);
  if (inet_pton(AF_INET6, ip.c_str(), &ipv6.sin6_addr) == 1) {
    std::memcpy(&parsed.storage_, &ipv6, sizeof(ipv6));
    parsed.length_ = sizeof(ipv6);
    output = parsed;
    return 0;
  }
  return -EINVAL;
}

AddressFamily Address::family() const noexcept {
  switch (storage_.ss_family) {
  case AF_INET:
    return AddressFamily::IPv4;
  case AF_INET6:
    return AddressFamily::IPv6;
  case AF_UNIX:
    return AddressFamily::Unix;
  default:
    return AddressFamily::Unspecified;
  }
}

std::string Address::ip() const {
  char text[INET6_ADDRSTRLEN]{};
  const void *source = nullptr;
  if (storage_.ss_family == AF_INET)
    source = &reinterpret_cast<const sockaddr_in *>(&storage_)->sin_addr;
  else if (storage_.ss_family == AF_INET6)
    source = &reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_addr;
  if (source == nullptr ||
      inet_ntop(storage_.ss_family, source, text, sizeof(text)) == nullptr)
    return {};
  return text;
}

std::uint16_t Address::port() const noexcept {
  if (storage_.ss_family == AF_INET)
    return ntohs(reinterpret_cast<const sockaddr_in *>(&storage_)->sin_port);
  if (storage_.ss_family == AF_INET6)
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage_)->sin6_port);
  return 0;
}

const sockaddr *Address::data() const noexcept {
  return reinterpret_cast<const sockaddr *>(&storage_);
}

} // namespace darkos::network
