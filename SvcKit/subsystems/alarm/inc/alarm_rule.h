/**
 * @file alarm_rule.h
 * @brief 报警规则接口（布防、计划、去重、升级）
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "alarm_types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace darkos::alarm {

class AlarmRule {
public:
  virtual ~AlarmRule() = default;
  virtual bool accept(const AlarmEvent &event) = 0;

protected:
  AlarmRule() = default;
};

std::shared_ptr<AlarmRule>
createAlarmRule(std::string source, std::string type,
                AlarmSeverity minimumSeverity = AlarmSeverity::Info,
                std::uint64_t cooldownNs = 0);

} // namespace darkos::alarm
