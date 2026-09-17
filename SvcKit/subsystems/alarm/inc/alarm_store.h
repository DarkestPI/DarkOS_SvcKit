/**
 * @file alarm_store.h
 * @brief 报警记录存储接口
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "alarm_types.h"

#include <string>
#include <vector>

namespace darkos::alarm {

class AlarmStore {
public:
  virtual ~AlarmStore() = default;
  virtual std::vector<AlarmRecord>
  query(const AlarmQuery &query = {}) const = 0;
  virtual int acknowledge(const std::string &alarmId) = 0;

protected:
  AlarmStore() = default;
};

} // namespace darkos::alarm
