#include "network_socket.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using darkos::network::Address;
using darkos::network::AddressFamily;
using darkos::network::Socket;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__,         \
                   #condition);                                                \
      return 1;                                                                \
    }                                                                          \
  } while (false)

int main() {
  Address loopback;
  CHECK(Address::fromIp("127.0.0.1", 0, loopback) == 0);

  Socket server = Socket::tcp();
  if (!server.valid())
    std::perror("Socket::tcp");
  CHECK(server.valid());
  CHECK(server.setReuseAddress() == 0);
  CHECK(server.bind(loopback) == 0);
  CHECK(server.listen() == 0);
  Address listening;
  CHECK(server.localAddress(listening) == 0);
  CHECK(listening.port() != 0);

  std::atomic<int> clientResult{0};
  std::thread client([port = listening.port(), &clientResult] {
    Address target;
    if (Address::fromIp("127.0.0.1", port, target) != 0)
      return clientResult.store(1);
    Socket socket = Socket::tcp();
    if (socket.connect(target) != 0)
      return clientResult.store(2);
    const char request[] = "ping";
    if (socket.send(request, sizeof(request)) !=
        static_cast<std::ptrdiff_t>(sizeof(request)))
      return clientResult.store(3);
    char response[5]{};
    if (socket.receive(response, sizeof(response)) !=
            static_cast<std::ptrdiff_t>(sizeof(response)) ||
        std::strcmp(response, "ping") != 0)
      return clientResult.store(4);
  });

  Address peer;
  Socket connection = server.accept(&peer);
  CHECK(connection.valid());
  CHECK(peer.ip() == "127.0.0.1");
  char request[5]{};
  CHECK(connection.receive(request, sizeof(request)) ==
        static_cast<std::ptrdiff_t>(sizeof(request)));
  CHECK(std::strcmp(request, "ping") == 0);
  CHECK(connection.send(request, sizeof(request)) ==
        static_cast<std::ptrdiff_t>(sizeof(request)));
  client.join();
  CHECK(clientResult == 0);

  std::vector<Address> resolved;
  std::string error;
  CHECK(Address::resolve("localhost", listening.port(),
                         AddressFamily::Unspecified, SOCK_STREAM, resolved,
                         error) == 0);
  CHECK(!resolved.empty());
  return 0;
}
