#pragma once

#include <string>

namespace common_ipc {

/**
 * 运行一次 IVA Platform 推理示例。
 *
 * Ubuntu 使用 Host Mock HAL，RV1126B 使用 Rockchip RKNN HAL。示例故意不
 * 假定 YOLO 输出布局，解码器只验证 Tensor 已经从 Platform 返回；真实项目
 * 应在这里替换为对应模型的输出解码器。
 */
bool runIvaExample(const std::string &modelPath);

} // namespace common_ipc
