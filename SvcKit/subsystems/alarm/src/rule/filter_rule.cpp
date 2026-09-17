#include "alarm_rule.h"

#include <mutex>
#include <utility>

namespace darkos::alarm {
namespace {

class MatchRule final : public AlarmRule {
public:
  MatchRule(std::string source, std::string type, AlarmSeverity minimum,
            std::uint64_t cooldownNs)
      : source_(std::move(source)), type_(std::move(type)), minimum_(minimum),
        cooldownNs_(cooldownNs) {}

  bool accept(const AlarmEvent &event) override {
    if ((!source_.empty() && event.source != source_) ||
        (!type_.empty() && event.type != type_) ||
        static_cast<unsigned>(event.severity) < static_cast<unsigned>(minimum_))
      return false;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (lastAcceptedNs_ != 0 && event.timestampNs >= lastAcceptedNs_ &&
        event.timestampNs - lastAcceptedNs_ < cooldownNs_)
      return false;
    lastAcceptedNs_ = event.timestampNs;
    return true;
  }

private:
  std::string source_;
  std::string type_;
  AlarmSeverity minimum_;
  std::uint64_t cooldownNs_;
  std::mutex mutex_;
  std::uint64_t lastAcceptedNs_{0};
};

} // namespace

std::shared_ptr<AlarmRule>
createAlarmRule(std::string source, std::string type,
                AlarmSeverity minimumSeverity, std::uint64_t cooldownNs) {
  return std::make_shared<MatchRule>(std::move(source), std::move(type),
                                     minimumSeverity, cooldownNs);
}

} // namespace darkos::alarm
