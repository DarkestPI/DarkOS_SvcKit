/**
 * @file iva_manager.h
 * @brief IVA 管理器门面
 */
#pragma once

#include "iva_algorithm.h"
#include "iva_perimeter.h"
#include "iva_tracker.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iva {

struct ManagerConfig {
  TrackerConfig tracker; ///< Manager 使用的目标关联参数。
};

/** IVA 门面：算法 -> 跟踪 -> 规则 -> 事件。 */
class Manager {
public:
  /** 创建 Manager；跟踪参数非法时返回 nullptr。 */
  static std::unique_ptr<Manager> create(const ManagerConfig &config = {});
  virtual ~Manager() = default;

  Manager(const Manager &) = delete;
  Manager &operator=(const Manager &) = delete;

  /** 设置检测算法；不能为空，重复设置表示替换算法。 */
  virtual int setAlgorithm(std::shared_ptr<Algorithm> algorithm) = 0;
  /** 添加规则；同 ID 规则不能重复。 */
  virtual int addRule(std::shared_ptr<Rule> rule) = 0;
  /** 按规则 ID 删除规则。 */
  virtual int removeRule(const std::string &id) = 0;

  /** 运行算法、跟踪和规则，events 只包含当前帧产生的事件。 */
  virtual int process(const FrameView &frame, std::vector<Event> &events) = 0;

  /** 供已接入外部推理引擎的路径直接提交检测结果。 */
  virtual int processDetections(std::uint64_t timestampNs,
                                const std::vector<Detection> &detections,
                                std::vector<Event> &events) = 0;
  /** 获取统计快照，不清除内部计数。 */
  virtual IvaStats stats() const noexcept = 0;
  /** 清除跟踪、规则状态和统计，不删除算法及规则配置。 */
  virtual void reset() noexcept = 0;

protected:
  Manager() = default;
};

} // namespace iva
