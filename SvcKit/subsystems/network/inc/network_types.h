/**
 * @file network_types.h
 * @brief 网络基础类型定义（地址、错误、配置、枚举）
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace darkos::network {

enum class AddressFamily {
  Unspecified,
  IPv4,
  IPv6,
  Unix,
};

struct InterfaceAddress {
  AddressFamily family{AddressFamily::Unspecified};
  std::string address;
};

struct NetworkInterface {
  std::uint32_t index{0};
  std::string name;
  bool up{false};
  bool running{false};
  bool loopback{false};
  std::vector<InterfaceAddress> addresses;
};

} // namespace darkos::network
