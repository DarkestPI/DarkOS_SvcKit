#include "network_manager.h"

#include <memory>
#include <string>
#include <vector>

int main() {
  std::vector<darkos::network::NetworkInterface> interfaces;
  std::string error;
  if (darkos::network::NetworkManager::snapshot(interfaces, error) != 0 ||
      interfaces.empty())
    return 1;
  bool foundLoopback = false;
  for (const auto &interface : interfaces) {
    if (interface.index == 0 || interface.name.empty())
      return 1;
    foundLoopback = foundLoopback || interface.loopback;
  }
  if (!foundLoopback)
    return 1;

  std::unique_ptr<darkos::EventLoop> loop(darkos::EventLoop::create());
  auto manager = darkos::network::NetworkManager::create(*loop, error);
  if (!manager || manager->start([](const auto &) {}) != 0 ||
      !manager->running())
    return 1;
  manager->stop();
  return manager->running() ? 1 : 0;
}
