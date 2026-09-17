#include "network_connection.h"

#include <sys/epoll.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using darkos::EventLoop;
using darkos::network::Address;
using darkos::network::Socket;
using darkos::network::TcpServer;

int main() {
  std::unique_ptr<EventLoop> loop(EventLoop::create());
  if (!loop)
    return 1;
  Address requested;
  if (Address::fromIp("127.0.0.1", 0, requested) != 0)
    return 1;
  std::string error;
  auto server = TcpServer::create(*loop, requested, error);
  if (!server)
    return 1;

  std::atomic<int> result{0};
  if (server->start([&](Socket socket, const Address &) {
        auto connection = std::make_shared<Socket>(std::move(socket));
        const int fd = connection->nativeHandle();
        if (!loop->watchFd(fd, EPOLLIN,
                           [&, connection, fd](std::uint32_t events) {
                             if ((events & EPOLLIN) == 0) {
                               result = 2;
                               loop->quit();
                               return;
                             }
                             char request[5]{};
                             if (connection->receive(request, sizeof(request)) !=
                                     static_cast<std::ptrdiff_t>(sizeof(request)) ||
                                 std::strcmp(request, "ping") != 0 ||
                                 connection->send(request, sizeof(request)) !=
                                     static_cast<std::ptrdiff_t>(sizeof(request)))
                               result = 3;
                             loop->unwatchFd(fd);
                             loop->quit();
                           })) {
          result = 4;
          loop->quit();
        }
      }) != 0)
    return 1;

  std::thread client([port = server->localAddress().port(), &result] {
    Address target;
    if (Address::fromIp("127.0.0.1", port, target) != 0) {
      result = 5;
      return;
    }
    Socket socket = Socket::tcp();
    if (!socket.valid() || socket.connect(target) != 0) {
      result = 6;
      return;
    }
    const char request[] = "ping";
    char response[5]{};
    if (socket.send(request, sizeof(request)) !=
            static_cast<std::ptrdiff_t>(sizeof(request)) ||
        socket.receive(response, sizeof(response)) !=
            static_cast<std::ptrdiff_t>(sizeof(response)) ||
        std::strcmp(response, "ping") != 0)
      result = 7;
  });

  loop->scheduleEvery(2000000000ULL, 0, [&] {
    result = 8;
    loop->quit();
  });
  loop->run();
  client.join();
  server->stop();
  return result.load();
}
