#include "base/EventLoop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

int main() {
  std::unique_ptr<darkos::EventLoop> loop(darkos::EventLoop::create());
  if (!loop) {
    std::fprintf(stderr, "EventLoop::create failed\n");
    return 1;
  }

  int pair[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                 pair) != 0)
    return 1;

  std::atomic<int> fdEvents{0};
  std::atomic<int> tasks{0};
  std::atomic<int> ticks{0};

  if (!loop->watchFd(pair[1], EPOLLIN, [&](std::uint32_t events) {
        if ((events & EPOLLIN) != 0) {
          char value = 0;
          if (read(pair[1], &value, 1) == 1)
            ++fdEvents;
        }
      }))
    return 1;

  const auto timer = loop->scheduleEvery(1000000ULL, 1000000ULL, [&] {
    if (++ticks >= 3)
      loop->quit();
  });
  if (timer == 0)
    return 1;

  std::thread runner([&] { loop->run(); });
  loop->post([&] { ++tasks; });
  const char value = 'x';
  if (write(pair[0], &value, 1) != 1)
    return 1;
  runner.join();

  close(pair[0]);
  close(pair[1]);
  if (fdEvents != 1 || tasks != 1 || ticks < 3) {
    std::fprintf(stderr, "fd=%d tasks=%d ticks=%d\n", fdEvents.load(),
                 tasks.load(), ticks.load());
    return 1;
  }
  return 0;
}
