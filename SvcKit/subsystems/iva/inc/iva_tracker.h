/**
 * @file iva_tracker.h
 * @brief 目标跟踪接口
 */
#pragma once

#include "iva_types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace iva {

struct TrackerConfig {
  float minimumIou{0.30F}; ///< 两个框达到该 IoU 才认为是同一目标。
  std::uint32_t maximumMissedFrames{5}; ///< 目标最多允许丢失的连续帧数。
};

/** 将相邻帧检测框关联为稳定目标 ID 的跟踪器。 */
class Tracker {
public:
  virtual ~Tracker() = default;

  Tracker(const Tracker &) = delete;
  Tracker &operator=(const Tracker &) = delete;

  /**
   * 提交一帧检测结果并输出当前活动目标。
   * 未匹配但仍在保留窗口内的目标不会出现在 output 中。
   */
  virtual int update(const std::vector<Detection> &detections,
                     std::vector<Track> &output) = 0;
  /** 清除所有目标状态，并从 ID 1 重新分配。 */
  virtual void reset() noexcept = 0;

protected:
  Tracker() = default;
};

/** 创建基于检测框 IoU 的轻量跟踪器。参数非法时返回 nullptr。 */
std::unique_ptr<Tracker> createIouTracker(const TrackerConfig &config = {});

} // namespace iva
