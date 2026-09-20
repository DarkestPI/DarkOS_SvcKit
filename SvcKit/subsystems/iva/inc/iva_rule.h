/**
 * @file iva_rule.h
 * @brief 通用规则接口与规则引擎
 */
#pragma once

#include "iva_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iva {

class Rule {
public:
  virtual ~Rule() = default;

  Rule(const Rule &) = delete;
  Rule &operator=(const Rule &) = delete;

  /** 返回规则的稳定唯一标识，用于事件关联和 Manager 去重。 */
  virtual const std::string &id() const noexcept = 0;
  /** 使用当前活动目标评估规则，并向 events 追加本帧产生的事件。 */
  virtual int evaluate(std::uint64_t timestampNs,
                       const std::vector<Track> &tracks,
                       std::vector<Event> &events) = 0;
  /** 清除规则内部的目标状态，例如区域内外状态和停留计时。 */
  virtual void reset() noexcept {}

protected:
  Rule() = default;
};

} // namespace iva
