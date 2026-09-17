#include "base/EventLoop.h"

#include "base/TimeUtil.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace darkos {
namespace {

constexpr std::uint64_t kWakeToken = std::numeric_limits<std::uint64_t>::max();

struct Timer {
  EventLoop::TimerId id{0};
  std::uint64_t deadlineNs{0};
  std::uint64_t intervalNs{0};
  EventLoop::Task task;
};

class EpollEventLoop final : public EventLoop {
public:
  EpollEventLoop() {
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0)
      return;
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0)
      return;
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = kWakeToken;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeFd_, &event) != 0)
      return;
    valid_ = true;
  }

  ~EpollEventLoop() override {
    quit();
    if (wakeFd_ >= 0)
      close(wakeFd_);
    if (epollFd_ >= 0)
      close(epollFd_);
  }

  bool valid() const noexcept { return valid_; }

  bool watchFd(int fd, std::uint32_t events, FdCallback callback) override {
    if (!valid_ || fd < 0 || fd == wakeFd_ || !callback)
      return false;

    std::lock_guard<std::mutex> lock(mutex_);
    epoll_event event{};
    event.events = events | EPOLLONESHOT;
    event.data.u64 = static_cast<std::uint64_t>(fd);
    const int operation = callbacks_.count(fd) == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (epoll_ctl(epollFd_, operation, fd, &event) != 0)
      return false;
    callbacks_[fd] = std::move(callback);
    return true;
  }

  void unwatchFd(int fd) noexcept override {
    if (!valid_ || fd < 0)
      return;
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.erase(fd);
    if (epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr) != 0 &&
        errno != ENOENT && errno != EBADF) {
      // Best effort during shutdown; callers own the fd.
    }
  }

  bool post(Task task) override {
    if (!valid_ || !task)
      return false;
    try {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
      }
      wake();
      return true;
    } catch (...) {
      return false;
    }
  }

  TimerId scheduleEvery(std::uint64_t initialDelayNs,
                        std::uint64_t intervalNs, Task task) override {
    if (!valid_ || !task)
      return 0;
    try {
      const TimerId id = nextTimerId_.fetch_add(1);
      Timer timer{id, saturatingAdd(monoNowNs(), initialDelayNs), intervalNs,
                  std::move(task)};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        timers_.emplace(id, std::move(timer));
      }
      wake();
      return id;
    } catch (...) {
      return 0;
    }
  }

  bool cancel(TimerId id) noexcept override {
    if (id == 0)
      return false;
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      removed = timers_.erase(id) != 0;
    }
    if (removed)
      wake();
    return removed;
  }

  void run() override {
    if (!valid_)
      return;
    epoll_event events[32]{};
    while (!quitting_.load()) {
      runTasks();
      runDueTimers();
      if (quitting_.load())
        break;

      const int count = epoll_wait(epollFd_, events, 32, waitTimeoutMs());
      if (count < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      for (int index = 0; index < count; ++index) {
        if (events[index].data.u64 == kWakeToken) {
          drainWake();
          continue;
        }
        const int fd = static_cast<int>(events[index].data.u64);
        FdCallback callback;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          const auto entry = callbacks_.find(fd);
          if (entry != callbacks_.end()) {
            callback = entry->second;
            callbacks_.erase(entry);
            // Make the documented one-shot contract literal. Removing before
            // invocation also prevents a just-closed fd number from colliding
            // with a newly accepted connection that reused the same number.
            epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
          }
        }
        if (callback) {
          try {
            callback(events[index].events);
          } catch (...) {
            // One component must not terminate the shared reactor.
          }
        }
      }
    }
    runTasks();
  }

  void quit() noexcept override {
    quitting_.store(true);
    wake();
  }

private:
  static std::uint64_t saturatingAdd(std::uint64_t left,
                                     std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
      return std::numeric_limits<std::uint64_t>::max();
    return left + right;
  }

  void wake() noexcept {
    if (wakeFd_ < 0)
      return;
    const std::uint64_t value = 1;
    const ssize_t result = write(wakeFd_, &value, sizeof(value));
    (void)result;
  }

  void drainWake() noexcept {
    std::uint64_t value = 0;
    while (read(wakeFd_, &value, sizeof(value)) == sizeof(value)) {
    }
  }

  void runTasks() {
    std::deque<Task> ready;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready.swap(tasks_);
    }
    for (Task &task : ready) {
      try {
        task();
      } catch (...) {
      }
    }
  }

  void runDueTimers() {
    std::vector<std::pair<TimerId, Task>> ready;
    const std::uint64_t now = monoNowNs();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto entry = timers_.begin(); entry != timers_.end();) {
        Timer &timer = entry->second;
        if (timer.deadlineNs > now) {
          ++entry;
          continue;
        }
        ready.emplace_back(timer.id, timer.task);
        if (timer.intervalNs == 0) {
          entry = timers_.erase(entry);
        } else {
          timer.deadlineNs = saturatingAdd(now, timer.intervalNs);
          ++entry;
        }
      }
    }
    for (auto &entry : ready) {
      try {
        entry.second();
      } catch (...) {
      }
    }
  }

  int waitTimeoutMs() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tasks_.empty())
      return 0;
    if (timers_.empty())
      return -1;
    std::uint64_t earliest = std::numeric_limits<std::uint64_t>::max();
    for (const auto &entry : timers_)
      earliest = std::min(earliest, entry.second.deadlineNs);
    const std::uint64_t now = monoNowNs();
    if (earliest <= now)
      return 0;
    const std::uint64_t remainingNs = earliest - now;
    const std::uint64_t roundedMs = (remainingNs + 999999ULL) / 1000000ULL;
    return static_cast<int>(std::min<std::uint64_t>(
        roundedMs, static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
  }

  int epollFd_{-1};
  int wakeFd_{-1};
  bool valid_{false};
  std::atomic<bool> quitting_{false};
  std::atomic<TimerId> nextTimerId_{1};
  mutable std::mutex mutex_;
  std::unordered_map<int, FdCallback> callbacks_;
  std::deque<Task> tasks_;
  std::unordered_map<TimerId, Timer> timers_;
};

} // namespace

EventLoop *EventLoop::create() noexcept {
  EpollEventLoop *loop = new (std::nothrow) EpollEventLoop();
  if (loop == nullptr || !loop->valid()) {
    delete loop;
    return nullptr;
  }
  return loop;
}

} // namespace darkos
