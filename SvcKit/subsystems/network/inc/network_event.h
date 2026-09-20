/**
 * @file network_event.h
 * @brief 事件循环与定时器接口
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include <base/EventLoop.h>

#include <cstdint>
#include <string>

namespace darkos::network {

using EventLoop = ::darkos::EventLoop;

enum class NetworkEventType {
  LinkChanged,      // Linux RTM_NEWLINK：管理状态或工作链路状态变化
  InterfaceAdded,   // Linux RTM_NEWLINK：新增接口（当前实现由链路事件承载）
  InterfaceRemoved, // Linux RTM_DELLINK：接口被移除
  AddressAdded,     // Linux RTM_NEWADDR：新增 IPv4/IPv6 地址
  AddressRemoved,   // Linux RTM_DELADDR：移除 IPv4/IPv6 地址
  Error,            // Netlink 或事件循环发生错误
};

struct NetworkEvent {
  NetworkEventType type{NetworkEventType::LinkChanged};
  std::uint64_t timestampNs{0};
  std::uint32_t interfaceIndex{0};
  std::string interfaceName;
  std::string address;
  // up 对应 IFF_UP：管理员是否启用接口。
  bool up{false};
  // running 对应 IFF_RUNNING：是否建立实际链路；Wi-Fi 关联成功前通常为 false。
  bool running{false};
  int code{0};
};

} // namespace darkos::network
