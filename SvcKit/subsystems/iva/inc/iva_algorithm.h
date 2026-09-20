/**
 * @file iva_algorithm.h
 * @brief 算法插件统一接口（传统 CV + 深度学习）
 */
#pragma once

#include "iva_types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iva {

class Algorithm {
public:
  virtual ~Algorithm() = default;

  Algorithm(const Algorithm &) = delete;
  Algorithm &operator=(const Algorithm &) = delete;

  /**
   * 对一帧图像执行检测或分类。
   * @param frame 当前帧的只读视图。
   * @param output 输出检测框；实现应覆盖调用方传入的旧内容。
   * @param error 失败时写入诊断信息。
   * @return 0 成功，失败返回负 errno 风格错误码。
   */
  virtual int process(const FrameView &frame, std::vector<Detection> &output,
                      std::string &error) = 0;

protected:
  Algorithm() = default;
};

using AlgorithmCallback = std::function<int(
    const FrameView &, std::vector<Detection> &, std::string &)>;

/**
 * 将已有推理实现适配成 IVA 算法插件，便于先测试规则链路。
 * callback 的参数和返回值语义与 Algorithm::process 相同。
 */
std::shared_ptr<Algorithm> createCallbackAlgorithm(AlgorithmCallback callback);

} // namespace iva
