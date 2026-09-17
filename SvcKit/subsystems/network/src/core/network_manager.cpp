/**
 * @file network_manager.cpp
 * @brief 网络管理器实现
 * @author your_name
 * @date 2026-09-16
 */

#include "network_manager.h"

#include <base/TimeUtil.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <new>
#include <utility>

namespace darkos::network {
namespace {

std::string interfaceName(std::uint32_t index) {
  char name[IF_NAMESIZE]{};
  return if_indextoname(index, name) == nullptr ? std::string{} : name;
}

std::string addressText(int family, const void *data) {
  char text[INET6_ADDRSTRLEN]{};
  return inet_ntop(family, data, text, sizeof(text)) == nullptr
             ? std::string{}
             : text;
}

class LinuxNetworkManager final : public NetworkManager {
public:
  LinuxNetworkManager(EventLoop &loop, int fd) noexcept : loop_(loop), fd_(fd) {}
  ~LinuxNetworkManager() override {
    stop();
    if (fd_ >= 0)
      close(fd_);
  }

  int start(EventCallback callback) override {
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

  void stop() noexcept override {
    if (!running_)
      return;
    running_ = false;
    loop_.unwatchFd(fd_);
    callback_ = {};
  }

  bool running() const noexcept override { return running_; }

private:
  bool rearm() {
    return loop_.watchFd(fd_, EPOLLIN,
                         [this](std::uint32_t events) { onReady(events); });
  }

  void publish(NetworkEvent event) noexcept {
    event.timestampNs = monoNowNs();
    try {
      callback_(event);
    } catch (...) {
    }
  }

  void onReady(std::uint32_t events) {
    if (!running_)
      return;
    if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
      publish(NetworkEvent{NetworkEventType::Error, 0, 0, {}, {}, false,
                           false, -EIO});
    }

    alignas(nlmsghdr) char buffer[8192];
    for (;;) {
      const ssize_t count = recv(fd_, buffer, sizeof(buffer), 0);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
          publish(NetworkEvent{NetworkEventType::Error, 0, 0, {}, {}, false,
                               false, -errno});
        break;
      }
      if (count == 0)
        break;
      parseMessages(buffer, static_cast<std::size_t>(count));
    }
    if (running_ && !rearm()) {
      publish(NetworkEvent{NetworkEventType::Error, 0, 0, {}, {}, false,
                           false, -EIO});
      stop();
    }
  }

  void parseMessages(const char *data, std::size_t size) {
    int remaining = static_cast<int>(size);
    for (const nlmsghdr *header = reinterpret_cast<const nlmsghdr *>(data);
         NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_type == NLMSG_DONE)
        break;
      if (header->nlmsg_type == NLMSG_ERROR) {
        const auto *error = static_cast<const nlmsgerr *>(NLMSG_DATA(header));
        publish(NetworkEvent{NetworkEventType::Error, 0, 0, {}, {}, false,
                             false, error->error});
        continue;
      }
      if (header->nlmsg_type == RTM_NEWLINK ||
          header->nlmsg_type == RTM_DELLINK) {
        parseLink(header);
      } else if (header->nlmsg_type == RTM_NEWADDR ||
                 header->nlmsg_type == RTM_DELADDR) {
        parseAddress(header);
      }
    }
  }

  void parseLink(const nlmsghdr *header) {
    const auto *info = static_cast<const ifinfomsg *>(NLMSG_DATA(header));
    NetworkEvent event;
    event.type = header->nlmsg_type == RTM_DELLINK
                     ? NetworkEventType::InterfaceRemoved
                     : NetworkEventType::LinkChanged;
    event.interfaceIndex = static_cast<std::uint32_t>(info->ifi_index);
    event.interfaceName = interfaceName(event.interfaceIndex);
    event.up = (info->ifi_flags & IFF_UP) != 0;
    event.running = (info->ifi_flags & IFF_RUNNING) != 0;
    publish(std::move(event));
  }

  void parseAddress(const nlmsghdr *header) {
    const auto *info = static_cast<const ifaddrmsg *>(NLMSG_DATA(header));
    NetworkEvent event;
    event.type = header->nlmsg_type == RTM_NEWADDR
                     ? NetworkEventType::AddressAdded
                     : NetworkEventType::AddressRemoved;
    event.interfaceIndex = info->ifa_index;
    event.interfaceName = interfaceName(info->ifa_index);
    int length = IFA_PAYLOAD(header);
    for (const rtattr *attribute = IFA_RTA(info); RTA_OK(attribute, length);
         attribute = RTA_NEXT(attribute, length)) {
      if (attribute->rta_type != IFA_LOCAL &&
          attribute->rta_type != IFA_ADDRESS)
        continue;
      if (info->ifa_family == AF_INET || info->ifa_family == AF_INET6) {
        event.address = addressText(info->ifa_family, RTA_DATA(attribute));
        if (!event.address.empty())
          break;
      }
    }
    publish(std::move(event));
  }

  EventLoop &loop_;
  int fd_{-1};
  EventCallback callback_;
  bool running_{false};
};

} // namespace

std::unique_ptr<NetworkManager> NetworkManager::create(EventLoop &loop,
                                                       std::string &error) {
  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        NETLINK_ROUTE);
  if (fd < 0) {
    error = "netlink socket: " + std::string(std::strerror(errno));
    return nullptr;
  }
  sockaddr_nl address{};
  address.nl_family = AF_NETLINK;
  address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
  if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    const int saved = errno;
    close(fd);
    error = "netlink bind: " + std::string(std::strerror(saved));
    return nullptr;
  }
  error.clear();
  auto *manager = new (std::nothrow) LinuxNetworkManager(loop, fd);
  if (manager == nullptr) {
    close(fd);
    error = "out of memory creating network manager";
    return nullptr;
  }
  return std::unique_ptr<NetworkManager>(manager);
}

int NetworkManager::snapshot(std::vector<NetworkInterface> &output,
                             std::string &error) {
  ifaddrs *addresses = nullptr;
  if (getifaddrs(&addresses) != 0) {
    const int saved = errno;
    error = "getifaddrs: " + std::string(std::strerror(saved));
    return -saved;
  }
  std::map<std::uint32_t, NetworkInterface> interfaces;
  for (const ifaddrs *entry = addresses; entry != nullptr;
       entry = entry->ifa_next) {
    if (entry->ifa_name == nullptr)
      continue;
    const std::uint32_t index = if_nametoindex(entry->ifa_name);
    if (index == 0)
      continue;
    NetworkInterface &interface = interfaces[index];
    interface.index = index;
    interface.name = entry->ifa_name;
    interface.up = (entry->ifa_flags & IFF_UP) != 0;
    interface.running = (entry->ifa_flags & IFF_RUNNING) != 0;
    interface.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
    if (entry->ifa_addr == nullptr)
      continue;
    InterfaceAddress address;
    if (entry->ifa_addr->sa_family == AF_INET) {
      address.family = AddressFamily::IPv4;
      address.address = addressText(
          AF_INET, &reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)
                        ->sin_addr);
    } else if (entry->ifa_addr->sa_family == AF_INET6) {
      address.family = AddressFamily::IPv6;
      address.address = addressText(
          AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(entry->ifa_addr)
                         ->sin6_addr);
    }
    if (!address.address.empty())
      interface.addresses.push_back(std::move(address));
  }
  freeifaddrs(addresses);
  output.clear();
  for (auto &entry : interfaces)
    output.push_back(std::move(entry.second));
  error.clear();
  return 0;
}

} // namespace darkos::network
