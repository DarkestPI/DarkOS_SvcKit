#pragma once

#include <cstdint>
#include <functional>

namespace darkos {

class EventLoop {
public:
  using FdCallback = std::function<void(std::uint32_t)>;
  using Task = std::function<void()>;
  using TimerId = std::uint64_t;

  static EventLoop *create() noexcept;
  virtual ~EventLoop() = default;

  EventLoop(const EventLoop &) = delete;
  EventLoop &operator=(const EventLoop &) = delete;

  /**
   * Register or re-arm an fd. Watches use EPOLLONESHOT semantics: the callback
   * must call watchFd() again when it wants another notification.
   */
  virtual bool watchFd(int fd, std::uint32_t events, FdCallback callback) = 0;
  virtual void unwatchFd(int fd) noexcept = 0;

  /** Queue work from any thread. The task is always executed by run(). */
  virtual bool post(Task task) = 0;

  /** Schedule a repeating timer. A zero interval creates a one-shot timer. */
  virtual TimerId scheduleEvery(std::uint64_t initialDelayNs,
                                std::uint64_t intervalNs, Task task) = 0;
  virtual bool cancel(TimerId id) noexcept = 0;

  virtual void run() = 0;
  virtual void quit() noexcept = 0;

protected:
  EventLoop() = default;
};

} // namespace darkos
