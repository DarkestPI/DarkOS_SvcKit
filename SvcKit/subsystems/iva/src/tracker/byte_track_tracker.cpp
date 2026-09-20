#include "iva_tracker.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace iva {
namespace {

float intersectionOverUnion(const Rect &left, const Rect &right) noexcept {
  if (!left.valid() || !right.valid())
    return 0.0F;
  const float leftEdge = std::max(left.x, right.x);
  const float topEdge = std::max(left.y, right.y);
  const float rightEdge = std::min(left.x + left.width, right.x + right.width);
  const float bottomEdge =
      std::min(left.y + left.height, right.y + right.height);
  const float width = rightEdge - leftEdge;
  const float height = bottomEdge - topEdge;
  if (width <= 0.0F || height <= 0.0F)
    return 0.0F;
  const float intersection = width * height;
  const float unionArea = left.area() + right.area() - intersection;
  return unionArea > 0.0F ? intersection / unionArea : 0.0F;
}

class IouTracker final : public Tracker {
public:
  explicit IouTracker(TrackerConfig config) : config_(config) {}

  int update(const std::vector<Detection> &detections,
             std::vector<Track> &output) override {
    output.clear();
    std::vector<bool> used(detections.size(), false);

    // 先按当前轨迹逐个寻找 IoU 最大的未使用检测框。这里采用贪心匹配，
    // 适合当前轻量规则场景；后续接入 ByteTrack 时可替换为匈牙利匹配。
    for (auto &state : states_) {
      std::size_t bestIndex = detections.size();
      float bestIou = config_.minimumIou;
      for (std::size_t index = 0; index < detections.size(); ++index) {
        if (used[index] ||
            detections[index].objectClass != state.track.detection.objectClass)
          continue;
        const float iou = intersectionOverUnion(state.track.detection.box,
                                                detections[index].box);
        if (iou >= bestIou) {
          bestIou = iou;
          bestIndex = index;
        }
      }

      if (bestIndex == detections.size()) {
        // 暂不立刻删除丢失目标，给下一帧短暂的检测抖动留出恢复机会。
        ++state.track.missed;
        state.track.hasPrevious = false;
        continue;
      }

      state.track.previousCenter = state.track.center;
      state.track.center = detections[bestIndex].box.center();
      state.track.detection = detections[bestIndex];
      state.track.hasPrevious = state.track.age > 0;
      state.track.missed = 0;
      ++state.track.age;
      used[bestIndex] = true;
    }

    // 超过保留窗口的轨迹才真正结束生命周期。
    states_.erase(
        std::remove_if(states_.begin(), states_.end(), [&](const State &state) {
          return state.track.missed > config_.maximumMissedFrames;
        }),
        states_.end());

    for (std::size_t index = 0; index < detections.size(); ++index) {
      if (used[index] || !detections[index].box.valid())
        continue;
      State state;
      state.track.id = nextId_++;
      state.track.detection = detections[index];
      state.track.center = detections[index].box.center();
      state.track.age = 1;
      states_.push_back(std::move(state));
    }

    output.reserve(states_.size());
    // 规则只消费本帧有检测支撑的轨迹，避免丢帧时误判区域和越界。
    for (const auto &state : states_) {
      if (state.track.missed == 0)
        output.push_back(state.track);
    }
    return 0;
  }

  void reset() noexcept override {
    states_.clear();
    nextId_ = 1;
  }

private:
  struct State {
    Track track;
  };

  TrackerConfig config_;
  std::vector<State> states_;
  std::uint64_t nextId_{1};
};

} // namespace

std::unique_ptr<Tracker> createIouTracker(const TrackerConfig &config) {
  if (config.minimumIou <= 0.0F || config.minimumIou > 1.0F)
    return nullptr;
  return std::make_unique<IouTracker>(config);
}

} // namespace iva
