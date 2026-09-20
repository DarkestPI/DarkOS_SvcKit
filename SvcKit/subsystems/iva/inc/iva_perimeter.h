/**
 * @file iva_perimeter.h
 * @brief 周界检测规则（入侵、越界、徘徊）
 */
#pragma once

#include "iva_rule.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iva {

/** 多边形区域规则的触发模式。 */
/** 区域进入/离开规则共用的多边形配置。 */
struct RegionConfig {
  std::string id; ///< 规则 ID，不能为空且在同一个 Manager 内必须唯一。
  std::vector<Point> polygon; ///< 多边形顶点，按顺时针或逆时针排列。
};

/** 区域入侵规则配置；目标进入区域后停留达到阈值才触发。 */
struct RegionIntrusionConfig {
  RegionConfig region; ///< 区域 ID 和多边形。
  std::uint64_t dwellTimeNs{0}; ///< 停留阈值，单位为单调时钟纳秒。
};

/** 创建“区域进入”规则。 */
std::shared_ptr<Rule> createRegionEnterRule(const RegionConfig &config,
                                            std::string &error);

/** 创建“区域离开”规则。 */
std::shared_ptr<Rule> createRegionLeaveRule(const RegionConfig &config,
                                            std::string &error);

/** 创建“区域入侵”规则。dwellTimeNs 必须大于 0。 */
std::shared_ptr<Rule>
createRegionIntrusionRule(const RegionIntrusionConfig &config,
                          std::string &error);

/** 越界方向，相对于 start -> end 的有向线段定义。 */
enum class LineDirection : std::uint32_t {
  Any = 0,      ///< 任意方向越线都触发。
  Positive = 1, ///< 从线段左侧/负侧到右侧/正侧，按叉积定义。
  Negative = 2, ///< 从线段右侧/正侧到左侧/负侧，按叉积定义。
};

struct LineRuleConfig {
  std::string id; ///< 规则 ID，不能为空且在同一个 Manager 内必须唯一。
  Point start; ///< 有向检测线起点。
  Point end; ///< 有向检测线终点，不能与 start 相同。
  LineDirection direction{LineDirection::Any}; ///< 越线方向限制。
};

/** 创建“越界检测”规则；目标中心点必须在相邻帧位于检测线两侧。 */
std::shared_ptr<Rule> createLineCrossRule(const LineRuleConfig &config,
                                          std::string &error);

} // namespace iva
