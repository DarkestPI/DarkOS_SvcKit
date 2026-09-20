#include "iva_manager.h"

#include <cerrno>
#include <chrono>
#include <exception>
#include <mutex>
#include <utility>

namespace iva {
namespace {

std::uint64_t nowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class ManagerImpl final : public Manager {
public:
  explicit ManagerImpl(const ManagerConfig &config)
      : tracker_(createIouTracker(config.tracker)) {}

  int setAlgorithm(std::shared_ptr<Algorithm> algorithm) override {
    if (!algorithm)
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(mutex_);
    algorithm_ = std::move(algorithm);
    return 0;
  }

  int addRule(std::shared_ptr<Rule> rule) override {
    if (!rule || rule->id().empty())
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &existing : rules_) {
      if (existing->id() == rule->id())
        return -EEXIST;
    }
    rules_.push_back(std::move(rule));
    return 0;
  }

  int removeRule(const std::string &id) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto iterator = rules_.begin(); iterator != rules_.end(); ++iterator) {
      if ((*iterator)->id() == id) {
        rules_.erase(iterator);
        return 0;
      }
    }
    return -ENOENT;
  }

  int process(const FrameView &frame, std::vector<Event> &events) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    events.clear();
    if (!algorithm_) {
      ++stats_.failedFrames;
      return -ENODEV;
    }

    std::vector<Detection> detections;
    std::string error;
    int result = 0;
    try {
      result = algorithm_->process(frame, detections, error);
    } catch (const std::exception &) {
      result = -EFAULT;
    } catch (...) {
      result = -EFAULT;
    }
    if (result != 0) {
      ++stats_.failedFrames;
      return result;
    }
    return processDetectionsLocked(frame.timestampNs, detections, events);
  }

  int processDetections(std::uint64_t timestampNs,
                        const std::vector<Detection> &detections,
                        std::vector<Event> &events) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    events.clear();
    return processDetectionsLocked(timestampNs, detections, events);
  }

  IvaStats stats() const noexcept override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  void reset() noexcept override {
    const std::lock_guard<std::mutex> lock(mutex_);
    tracker_->reset();
    for (const auto &rule : rules_)
      rule->reset();
    stats_ = {};
  }

private:
  int processDetectionsLocked(std::uint64_t timestampNs,
                              const std::vector<Detection> &detections,
                              std::vector<Event> &events) {
    if (timestampNs == 0)
      timestampNs = nowNs();
    ++stats_.processedFrames;
    stats_.detections += detections.size();

    // Manager 不关心检测来自 Platform、Software 还是外部进程，只在这里统一进入
    // 跟踪和规则阶段，保证两种入口的事件语义一致。
    std::vector<Track> tracks;
    const int trackerResult = tracker_->update(detections, tracks);
    if (trackerResult != 0) {
      ++stats_.failedFrames;
      return trackerResult;
    }
    stats_.activeTracks = tracks.size();

    try {
      for (const auto &rule : rules_) {
        const int result = rule->evaluate(timestampNs, tracks, events);
        if (result != 0) {
          ++stats_.failedFrames;
          return result;
        }
      }
    } catch (...) {
      ++stats_.failedFrames;
      return -EFAULT;
    }
    stats_.emittedEvents += events.size();
    return 0;
  }

  mutable std::mutex mutex_;
  std::shared_ptr<Algorithm> algorithm_;
  std::unique_ptr<Tracker> tracker_;
  std::vector<std::shared_ptr<Rule>> rules_;
  IvaStats stats_;
};

} // namespace

std::unique_ptr<Manager> Manager::create(const ManagerConfig &config) {
  auto tracker = createIouTracker(config.tracker);
  if (!tracker)
    return nullptr;
  return std::make_unique<ManagerImpl>(config);
}

} // namespace iva
