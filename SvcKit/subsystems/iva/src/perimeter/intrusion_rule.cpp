#include "iva_perimeter.h"

#include <cerrno>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace iva {
namespace {

constexpr float kEpsilon = 0.00001F;

// 这是实现内部的共享状态，公共头文件直接暴露四个具体创建函数，
// 调用者不需要理解这个模式枚举。
enum class RegionRuleMode { Enter, Leave, Intrusion };

float cross(const Point &a, const Point &b, const Point &point) noexcept {
  return (b.x - a.x) * (point.y - a.y) -
         (b.y - a.y) * (point.x - a.x);
}

bool onSegment(const Point &a, const Point &b, const Point &point) noexcept {
  return std::abs(cross(a, b, point)) <= kEpsilon &&
         point.x >= std::min(a.x, b.x) - kEpsilon &&
         point.x <= std::max(a.x, b.x) + kEpsilon &&
         point.y >= std::min(a.y, b.y) - kEpsilon &&
         point.y <= std::max(a.y, b.y) + kEpsilon;
}

bool contains(const std::vector<Point> &polygon, const Point &point) noexcept {
  if (polygon.size() < 3)
    return false;
  // 射线法：从点向右发射水平射线，穿过边界的次数为奇数表示在内部。
  bool inside = false;
  for (std::size_t index = 0, previous = polygon.size() - 1;
       index < polygon.size(); previous = index++) {
    const Point &current = polygon[index];
    const Point &last = polygon[previous];
    if (onSegment(last, current, point))
      return true;
    const bool intersects =
        ((current.y > point.y) != (last.y > point.y)) &&
        (point.x < (last.x - current.x) * (point.y - current.y) /
                           (last.y - current.y) +
                       current.x);
    if (intersects)
      inside = !inside;
  }
  return inside;
}

void appendEvent(const std::string &ruleId, EventType type, std::uint64_t timestamp,
                 const Track &track, std::vector<Event> &events) {
  Event event;
  event.ruleId = ruleId;
  event.type = type;
  event.targetId = track.id;
  event.timestampNs = timestamp;
  event.objectClass = track.detection.objectClass;
  event.box = track.detection.box;
  events.push_back(std::move(event));
}

class RegionRule final : public Rule {
public:
  RegionRule(std::string id, std::vector<Point> polygon, RegionRuleMode mode,
             std::uint64_t dwellTimeNs)
      : id_(std::move(id)), polygon_(std::move(polygon)), mode_(mode),
        dwellTimeNs_(dwellTimeNs) {}

  const std::string &id() const noexcept override { return id_; }

  int evaluate(std::uint64_t timestampNs, const std::vector<Track> &tracks,
               std::vector<Event> &events) override {
    for (const auto &track : tracks) {
      const bool inside = contains(polygon_, track.center);
      auto [iterator, inserted] = states_.try_emplace(track.id);
      State &state = iterator->second;
      if (inserted) {
        // 第一次看到目标时只建立基线，不把“已经在区域内”误报为进入。
        state.inside = inside;
        state.enteredAtNs = inside ? timestampNs : 0;
        continue;
      }

      if (!state.inside && inside) {
        // 外 -> 内：进入规则立即触发；入侵规则从此刻开始计时。
        state.enteredAtNs = timestampNs;
        state.triggered = false;
        if (mode_ == RegionRuleMode::Enter)
          appendEvent(id_, EventType::RegionEntered, timestampNs, track,
                      events);
      } else if (state.inside && !inside) {
        // 内 -> 外：离开规则触发，并清除本次停留计时。
        if (mode_ == RegionRuleMode::Leave)
          appendEvent(id_, EventType::RegionLeft, timestampNs, track,
                      events);
        state.enteredAtNs = 0;
        state.triggered = false;
      }

      if (inside && mode_ == RegionRuleMode::Intrusion &&
          !state.triggered && state.enteredAtNs != 0 &&
          timestampNs >= state.enteredAtNs &&
          timestampNs - state.enteredAtNs >= dwellTimeNs_) {
        // 只触发一次，直到目标离开区域后再次进入才重新计时。
        appendEvent(id_, EventType::Intrusion, timestampNs, track,
                    events);
        state.triggered = true;
      }
      state.inside = inside;
    }
    return 0;
  }

  void reset() noexcept override { states_.clear(); }

private:
  struct State {
    bool inside{false};
    bool triggered{false};
    std::uint64_t enteredAtNs{0};
  };

  std::string id_;
  std::vector<Point> polygon_;
  RegionRuleMode mode_;
  std::uint64_t dwellTimeNs_{0};
  std::unordered_map<std::uint64_t, State> states_;
};

std::shared_ptr<Rule> createRegionRule(const RegionConfig &config,
                                       RegionRuleMode mode,
                                       std::uint64_t dwellTimeNs,
                                       std::string &error) {
  if (config.id.empty() || config.polygon.size() < 3) {
    error = "region rule requires an id and at least three polygon points";
    return nullptr;
  }
  return std::make_shared<RegionRule>(config.id, config.polygon, mode,
                                      dwellTimeNs);
}

} // namespace

std::shared_ptr<Rule> createRegionEnterRule(const RegionConfig &config,
                                            std::string &error) {
  return createRegionRule(config, RegionRuleMode::Enter, 0, error);
}

std::shared_ptr<Rule> createRegionLeaveRule(const RegionConfig &config,
                                            std::string &error) {
  return createRegionRule(config, RegionRuleMode::Leave, 0, error);
}

std::shared_ptr<Rule>
createRegionIntrusionRule(const RegionIntrusionConfig &config,
                          std::string &error) {
  if (config.dwellTimeNs == 0) {
    error = "region intrusion dwell time must be greater than zero";
    return nullptr;
  }
  return createRegionRule(config.region, RegionRuleMode::Intrusion,
                          config.dwellTimeNs, error);
}

} // namespace iva
