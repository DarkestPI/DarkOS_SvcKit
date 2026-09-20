#include "iva_perimeter.h"

#include <cerrno>
#include <cmath>
#include <utility>

namespace iva {
namespace {

constexpr float kEpsilon = 0.00001F;

float sideOfLine(const Point &start, const Point &end,
                const Point &point) noexcept {
  return (end.x - start.x) * (point.y - start.y) -
         (end.y - start.y) * (point.x - start.x);
}

class LineCrossRule final : public Rule {
public:
  explicit LineCrossRule(LineRuleConfig config) : config_(std::move(config)) {}

  const std::string &id() const noexcept override { return config_.id; }

  int evaluate(std::uint64_t timestampNs, const std::vector<Track> &tracks,
               std::vector<Event> &events) override {
    for (const auto &track : tracks) {
      if (!track.hasPrevious)
        continue;
      const float previous =
          sideOfLine(config_.start, config_.end, track.previousCenter);
      const float current = sideOfLine(config_.start, config_.end, track.center);
      // 只有前后中心点严格位于检测线两侧，才算一次越线；落在线上时
      // 暂不触发，避免目标贴线抖动导致重复报警。
      if (std::abs(previous) <= kEpsilon || std::abs(current) <= kEpsilon ||
          previous * current >= 0.0F)
        continue;
      const bool positive = previous < 0.0F && current > 0.0F;
      if ((config_.direction == LineDirection::Positive && !positive) ||
          (config_.direction == LineDirection::Negative && positive))
        continue;
      Event event;
      event.ruleId = config_.id;
      event.type = EventType::LineCrossed;
      event.targetId = track.id;
      event.timestampNs = timestampNs;
      event.objectClass = track.detection.objectClass;
      event.box = track.detection.box;
      events.push_back(std::move(event));
    }
    return 0;
  }

private:
  LineRuleConfig config_;
};

} // namespace

std::shared_ptr<Rule> createLineCrossRule(const LineRuleConfig &config,
                                          std::string &error) {
  if (config.id.empty() ||
      (std::abs(config.start.x - config.end.x) <= kEpsilon &&
       std::abs(config.start.y - config.end.y) <= kEpsilon)) {
    error = "line rule requires an id and two distinct points";
    return nullptr;
  }
  return std::make_shared<LineCrossRule>(config);
}

} // namespace iva
