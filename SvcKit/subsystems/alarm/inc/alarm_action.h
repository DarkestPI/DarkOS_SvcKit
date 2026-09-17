/**
 * @file alarm_action.h
 * @brief 报警联动动作接口
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "alarm_types.h"

#include <functional>
#include <memory>
#include <string>

namespace darkos::alarm {

class AlarmAction {
public:
  virtual ~AlarmAction() = default;
  virtual int execute(const AlarmEvent &event, std::string &error) = 0;

protected:
  AlarmAction() = default;
};

using AlarmActionCallback = std::function<int(const AlarmEvent &, std::string &)>;
std::shared_ptr<AlarmAction> createAlarmAction(AlarmActionCallback callback);

} // namespace darkos::alarm
