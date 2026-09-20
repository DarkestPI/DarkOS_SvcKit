/**
 * @file iva_inference.h
 * @brief IVA 推理引擎抽象接口
 */
#pragma once

#include "iva_model.h"
#include "iva_types.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace iva {

/**
 * 推理执行位置。
 *
 * IVA 不直接认识 RKNN、NNIE 或联咏 SDK；Platform 表示由当前设备的推理
 * HAL 选择实际硬件，Software 保留给未来的纯软件实现。
 */
enum class InferenceBackend : std::uint32_t {
  Platform = 0, ///< 当前 Platform 推理 HAL，具体硬件由平台实现决定。
  Software = 1, ///< 纯软件推理；当前仅保留接口骨架。
};

/**
 * Platform 推理输出张量的统一快照。
 * 厂商 HAL 的输出缓冲区会在推理适配器内部拷贝成 float，模型解码器不依赖
 * Rockchip、HiSilicon 或其他厂商缓冲区的生命周期。
 */
struct InferenceTensor {
  std::vector<float> values; ///< 已转换为 float 的张量数据。
  std::vector<std::uint32_t> shape; ///< 张量形状，例如 [1, 25200, 6]。
};

/**
 * 模型专属输出解码器：负责把原始输出转换成 Detection。
 * 不同模型版本、分类数量和输出布局不同，不能由通用 Runtime 自动推断。
 */
using InferenceOutputDecoder = std::function<int(
    const std::vector<InferenceTensor> &outputs, const FrameView &frame,
    std::vector<Detection> &detections, std::string &error)>;

/** 与图像格式无关的输入预处理参数；不包含任何厂商 SDK 类型。 */
struct InferencePreprocessConfig {
  bool swapRedBlue{false}; ///< 输入为 RGB 时是否交换 R/B 通道。
  bool normalizeToUnit{false}; ///< 是否把 8 bit 输入先缩放到 [0, 1]。
  std::array<float, 3> mean{{0.0F, 0.0F, 0.0F}}; ///< 每通道减均值。
  std::array<float, 3> standardDeviation{{1.0F, 1.0F, 1.0F}}; ///< 每通道除标准差。
};

/** 推理引擎创建参数；厂商 SDK 配置由 Platform HAL 管理。 */
struct InferenceEngineConfig {
  InferenceBackend backend{InferenceBackend::Platform}; ///< 推理执行位置。
  std::string deviceId{"inference"}; ///< Platform 推理设备实例 ID。
  InferencePreprocessConfig preprocess; ///< 通用输入预处理参数。
  InferenceOutputDecoder decoder; ///< 模型输出解码器，不能为空。
};

/** 将模型加载和单帧推理统一为可替换的后端接口。 */
class InferenceEngine {
public:
  virtual ~InferenceEngine() = default;

  InferenceEngine(const InferenceEngine &) = delete;
  InferenceEngine &operator=(const InferenceEngine &) = delete;

  /** 加载模型并准备推理资源。 */
  virtual int loadModel(const ModelInfo &model, std::string &error) = 0;
  /** 释放当前模型和后端资源。 */
  virtual int unloadModel() noexcept = 0;
  /** 判断当前是否有可推理模型。 */
  virtual bool loaded() const noexcept = 0;
  /** 执行一帧推理并输出已解码的 Detection。 */
  virtual int infer(const FrameView &frame, std::vector<Detection> &output,
                    std::string &error) = 0;

protected:
  InferenceEngine() = default;
};

/** 创建已编译进 SDK 的推理后端；未启用对应厂商 SDK 时返回 nullptr。 */
std::unique_ptr<InferenceEngine>
createInferenceEngine(const InferenceEngineConfig &config, std::string &error);

} // namespace iva
