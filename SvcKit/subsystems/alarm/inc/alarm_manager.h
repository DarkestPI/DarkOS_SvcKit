/**
 * @file alarm_manager.h
 * @brief 报警管理器门面
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "alarm_action.h"
#include "alarm_rule.h"
#include "alarm_store.h"
#include "alarm_types.h"

#include <base/EventLoop.h>

#include <memory>
#include <string>

namespace darkos::alarm {

class AlarmManager : public AlarmStore {
public:
  static std::unique_ptr<AlarmManager>
  create(EventLoop &eventLoop, const AlarmConfig &config, std::string &error);
  ~AlarmManager() override = default;

  AlarmManager(const AlarmManager &) = delete;
  AlarmManager &operator=(const AlarmManager &) = delete;

  virtual int addRule(std::shared_ptr<AlarmRule> rule) = 0;
  virtual int addAction(std::shared_ptr<AlarmAction> action) = 0;
  virtual int publish(AlarmEvent event) = 0;
  virtual int waitForHandled(std::uint64_t minimum, int timeoutMs) = 0;
  virtual AlarmStats stats() const noexcept = 0;

protected:
  AlarmManager() = default;
};

} // namespace darkos::alarm
