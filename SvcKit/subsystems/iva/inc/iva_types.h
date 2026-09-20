/**
 * @file iva_types.h
 * @brief IVA 基础类型定义（帧、目标、事件、枚举）
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace iva {

/** 原始视频帧的像素排列；数值用于配置/序列化，禁止随意调整。 */
enum class PixelFormat : std::uint32_t {
  Nv12 = 0,  ///< Y 平面 + UV 交错平面，常见于摄像头/NPU 输入。
  Nv21 = 1,  ///< Y 平面 + VU 交错平面。
  Yuyv = 2,  ///< YUV 4:2:2，每两个像素排列为 YUYV。
  Rgb24 = 3, ///< 每个像素 3 字节，顺序为 R、G、B。
  Gray8 = 4, ///< 单通道 8 bit 灰度图。
};

/** 检测目标类别。Unknown 表示后端无法映射到已知类别。 */
enum class ObjectClass : std::uint32_t {
  Unknown = 0, ///< 未知或未分类目标。
  Person = 1,  ///< 人。
  Vehicle = 2, ///< 车辆。
  Pet = 3,     ///< 宠物或其他动物。
  Face = 4,    ///< 人脸。
};

/** 图像坐标，原点在左上角，x 向右、y 向下，单位为像素。 */
struct Point {
  float x{0.0F}; ///< 水平坐标。
  float y{0.0F}; ///< 垂直坐标。
};

/** 轴对齐矩形，x/y 是左上角，width/height 必须大于 0 才是有效框。 */
struct Rect {
  float x{0.0F};      ///< 左上角水平坐标。
  float y{0.0F};      ///< 左上角垂直坐标。
  float width{0.0F};  ///< 矩形宽度。
  float height{0.0F}; ///< 矩形高度。

  /** 判断矩形是否可用于 IoU、中心点等几何计算。 */
  bool valid() const noexcept { return width > 0.0F && height > 0.0F; }
  /** 返回有效矩形面积；无效矩形返回 0。 */
  float area() const noexcept {
    return valid() ? width * height : 0.0F;
  }
  /** 返回矩形中心点，规则默认使用中心点判断区域归属。 */
  Point center() const noexcept { return {x + width * 0.5F, y + height * 0.5F}; }
};

/**
 * 借用的视频帧视图；调用方必须保证 data 在算法调用期间有效。
 * timestampNs 使用 CLOCK_MONOTONIC 纳秒；data 允许为空，便于只提交外部
 * Detection 的测试或已经完成预处理的算法链路。
 */
struct FrameView {
  const std::uint8_t *data{nullptr}; ///< 帧数据，只读借用指针。
  std::size_t size{0};                ///< data 指向的字节数。
  std::uint64_t timestampNs{0};       ///< 单调时钟纳秒时间戳。
  std::uint32_t width{0};             ///< 图像宽度，单位为像素。
  std::uint32_t height{0};            ///< 图像高度，单位为像素。
  std::uint32_t stride{0};            ///< 每行字节数，可能大于 width。
  PixelFormat pixelFormat{PixelFormat::Nv12}; ///< data 的像素格式。
};

struct Detection {
  ObjectClass objectClass{ObjectClass::Unknown}; ///< 检测类别。
  float confidence{0.0F};                        ///< [0, 1] 置信度。
  Rect box;                                      ///< 目标在当前帧中的位置。
  std::string label;                             ///< 后端提供的可选原始标签。
};

/**
 * 跟踪器输出的目标状态；规则通过 previousCenter 判断运动方向。
 * missed 不为 0 的内部保留目标不会交给规则，避免目标丢失时误触发事件。
 */
struct Track {
  std::uint64_t id{0};       ///< Manager 生命周期内单调递增的目标 ID。
  Detection detection;       ///< 当前帧检测结果。
  Point previousCenter;      ///< 上一帧匹配到的中心点。
  Point center;               ///< 当前帧中心点。
  std::uint32_t age{0};       ///< 连续匹配到的帧数。
  std::uint32_t missed{0};    ///< 连续未匹配帧数。
  bool hasPrevious{false};    ///< 是否存在可用于运动判断的上一中心点。
};

/** IVA 规则在当前帧产生的事件类型。 */
enum class EventType : std::uint32_t {
  RegionEntered = 0, ///< 目标从区域外进入区域内。
  RegionLeft = 1,    ///< 目标从区域内离开区域外。
  Intrusion = 2,     ///< 目标在区域内持续停留达到设定时间。
  LineCrossed = 3,   ///< 目标中心点按指定方向越过检测线。
};

struct Event {
  std::string ruleId; ///< 产生事件的规则 ID。
  EventType type{EventType::RegionEntered}; ///< 事件类型。
  std::uint64_t targetId{0}; ///< 触发事件的跟踪目标 ID。
  std::uint64_t timestampNs{0}; ///< 事件对应帧的单调时钟时间戳。
  ObjectClass objectClass{ObjectClass::Unknown}; ///< 触发目标类别。
  Rect box; ///< 触发时目标位置。
  std::string message; ///< 可选的人类可读描述。
};

struct IvaStats {
  std::uint64_t processedFrames{0}; ///< 成功进入跟踪流程的帧数。
  std::uint64_t failedFrames{0};    ///< 算法、跟踪或规则处理失败的帧数。
  std::uint64_t detections{0};      ///< 累计输入检测框数。
  std::uint64_t activeTracks{0};    ///< 最近一次处理后的活动目标数。
  std::uint64_t emittedEvents{0};   ///< 累计输出事件数。
};

} // namespace iva
