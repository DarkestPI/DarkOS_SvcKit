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
  LinkChanged,
  InterfaceAdded,
  InterfaceRemoved,
  AddressAdded,
  AddressRemoved,
  Error,
};

struct NetworkEvent {
  NetworkEventType type{NetworkEventType::LinkChanged};
  std::uint64_t timestampNs{0};
  std::uint32_t interfaceIndex{0};
  std::string interfaceName;
  std::string address;
  bool up{false};
  bool running{false};
  int code{0};
};

} // namespace darkos::network
