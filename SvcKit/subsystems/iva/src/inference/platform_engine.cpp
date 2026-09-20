/**
 * @file platform_engine.cpp
 * @brief IVA 到 Platform inference HAL 的适配器
 *
 * 这里不能 include 任何厂商 SDK。实际的 RKNN、NNIE 或联咏调用位于
 * platform/vendors/<vendor>/socs/<soc>，本文件只负责通用 HAL 和 IVA 类型转换。
 */

#include "iva_inference.h"

#include <hardware/hardware.h>
#include <inference/IInferenceDevice.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace iva {
namespace {

constexpr std::uint32_t kDefaultMaxOutputTensors = 16;

std::uint32_t toPlatformPixelFormat(PixelFormat format) noexcept {
  switch (format) {
  case PixelFormat::Nv12:
    return INFERENCE_PIX_FMT_NV12;
  case PixelFormat::Nv21:
    return INFERENCE_PIX_FMT_NV21;
  case PixelFormat::Yuyv:
    return INFERENCE_PIX_FMT_YUYV;
  case PixelFormat::Rgb24:
    return INFERENCE_PIX_FMT_RGB24;
  case PixelFormat::Gray8:
    return INFERENCE_PIX_FMT_GRAY8;
  }
  return 0;
}

std::string failure(const char *operation, int result) {
  return std::string(operation) + " failed (" + std::to_string(result) + ")";
}

int copyTensor(const inference_tensor_t &source, InferenceTensor &destination,
               std::string &error) {
  if (source.data == nullptr && source.size != 0) {
    error = "Platform inference returned an empty tensor buffer";
    return -EIO;
  }
  if (source.dimension_count > INFERENCE_MAX_TENSOR_DIMS) {
    error = "Platform inference returned too many tensor dimensions";
    return -EOVERFLOW;
  }

  destination.shape.assign(source.dimensions,
                           source.dimensions + source.dimension_count);
  switch (source.data_type) {
  case INFERENCE_DATA_FLOAT32: {
    if (source.size % sizeof(float) != 0) {
      error = "float32 tensor size is not aligned";
      return -EINVAL;
    }
    if (source.size == 0) {
      destination.values.clear();
      return 0;
    }
    const auto *values = static_cast<const float *>(source.data);
    destination.values.assign(values, values + source.size / sizeof(float));
    return 0;
  }
  case INFERENCE_DATA_UINT8: {
    const auto *values = static_cast<const std::uint8_t *>(source.data);
    destination.values.resize(source.size);
    for (std::size_t index = 0; index < source.size; ++index) {
      const float value = static_cast<float>(values[index]);
      destination.values[index] = source.quant_scale == 0.0F
                                      ? value
                                      : (value - source.quant_zero_point) *
                                            source.quant_scale;
    }
    return 0;
  }
  case INFERENCE_DATA_INT8: {
    const auto *values = static_cast<const std::int8_t *>(source.data);
    destination.values.resize(source.size);
    for (std::size_t index = 0; index < source.size; ++index) {
      const float value = static_cast<float>(values[index]);
      destination.values[index] = source.quant_scale == 0.0F
                                      ? value
                                      : (value - source.quant_zero_point) *
                                            source.quant_scale;
    }
    return 0;
  }
  default:
    error = "Platform inference returned an unsupported tensor type";
    return -ENOTSUP;
  }
}

class PlatformInferenceEngine final : public InferenceEngine {
public:
  explicit PlatformInferenceEngine(InferenceEngineConfig config)
      : config_(std::move(config)) {}

  ~PlatformInferenceEngine() override { unloadModel(); }

  int loadModel(const ModelInfo &model, std::string &error) override {
    if (model.path.empty()) {
      error = "inference model path is required";
      return -EINVAL;
    }

    const int unloadResult = unloadModel();
    if (unloadResult != 0) {
      error = failure("unload previous inference model", unloadResult);
      return unloadResult;
    }

    const hw_module_t *module = nullptr;
    int result = hw_get_module(INFERENCE_HARDWARE_MODULE_ID, &module);
    if (result != 0) {
      error = failure("load inference HAL", result);
      return result;
    }

    result = inference_open_by_id(module, config_.deviceId.c_str(), &device_);
    if (result != 0) {
      error = failure("open inference HAL", result);
      return result;
    }
    if (device_->ops->get_capabilities == nullptr ||
        device_->ops->load_model == nullptr ||
        device_->ops->unload_model == nullptr ||
        device_->ops->run == nullptr ||
        device_->ops->release_outputs == nullptr) {
      error = "inference HAL is missing required operations";
      inference_close(device_);
      device_ = nullptr;
      return -ENOTSUP;
    }

    inference_preprocess_t preprocess{};
    preprocess.swap_red_blue = config_.preprocess.swapRedBlue ? 1u : 0u;
    preprocess.normalize_to_unit = config_.preprocess.normalizeToUnit ? 1u : 0u;
    std::copy(config_.preprocess.mean.begin(), config_.preprocess.mean.end(),
              preprocess.mean);
    std::copy(config_.preprocess.standardDeviation.begin(),
              config_.preprocess.standardDeviation.end(),
              preprocess.standard_deviation);
    inference_model_t platformModel{};
    platformModel.path = model.path.c_str();
    platformModel.preprocess = &preprocess;
    result = device_->ops->load_model(device_, &platformModel);
    if (result != 0) {
      error = failure("load inference model", result);
      inference_close(device_);
      device_ = nullptr;
      return result;
    }
    inference_caps_t caps{};
    result = device_->ops->get_capabilities(device_, &caps);
    if (result != 0) {
      error = failure("query inference HAL capabilities", result);
      device_->ops->unload_model(device_);
      inference_close(device_);
      device_ = nullptr;
      return result;
    }
    if (caps.max_output_tensors == 0)
      caps.max_output_tensors = kDefaultMaxOutputTensors;
    caps_ = caps;
    model_ = model;
    loaded_ = true;
    return 0;
  }

  int unloadModel() noexcept override {
    if (device_ == nullptr) {
      loaded_ = false;
      model_ = {};
      caps_ = {};
      return 0;
    }

    int result = 0;
    if (loaded_ && device_->ops != nullptr &&
        device_->ops->unload_model != nullptr)
      result = device_->ops->unload_model(device_);
    const int closeResult = inference_close(device_);
    device_ = nullptr;
    loaded_ = false;
    model_ = {};
    caps_ = {};
    return result != 0 ? result : closeResult;
  }

  bool loaded() const noexcept override { return loaded_; }

  int infer(const FrameView &frame, std::vector<Detection> &output,
            std::string &error) override {
    output.clear();
    if (!loaded_ || device_ == nullptr)
      return -EPIPE;
    if (!config_.decoder) {
      error = "inference output decoder is not configured";
      return -ENOTSUP;
    }
    if (frame.data == nullptr || frame.size == 0 || frame.width == 0 ||
        frame.height == 0) {
      error = "inference input frame is invalid";
      return -EINVAL;
    }

    const std::uint32_t pixelFormat = toPlatformPixelFormat(frame.pixelFormat);
    if (pixelFormat == 0) {
      error = "inference input pixel format is unsupported";
      return -ENOTSUP;
    }

    inference_frame_t platformFrame{};
    platformFrame.fd = -1;
    platformFrame.data = frame.data;
    platformFrame.size = frame.size > std::numeric_limits<std::uint32_t>::max()
                             ? 0
                             : static_cast<std::uint32_t>(frame.size);
    if (platformFrame.size == 0) {
      error = "inference input frame is too large";
      return -EOVERFLOW;
    }
    platformFrame.width = frame.width;
    platformFrame.height = frame.height;
    platformFrame.stride = frame.stride;
    platformFrame.pixel_format = pixelFormat;
    platformFrame.timestamp_ns = frame.timestampNs;

    std::vector<inference_tensor_t> rawOutputs(caps_.max_output_tensors);
    inference_output_set_t outputs{};
    outputs.tensors = rawOutputs.data();
    outputs.capacity = static_cast<std::uint32_t>(rawOutputs.size());
    int result = device_->ops->run(device_, &platformFrame, &outputs, 0);
    if (result != 0) {
      error = failure("run inference", result);
      return result;
    }
    if (outputs.count > outputs.capacity) {
      device_->ops->release_outputs(device_, &outputs);
      error = "inference HAL returned too many output tensors";
      return -EOVERFLOW;
    }

    std::vector<InferenceTensor> tensors;
    try {
      tensors.reserve(outputs.count);
      for (std::uint32_t index = 0; index < outputs.count; ++index) {
        InferenceTensor tensor;
        result = copyTensor(outputs.tensors[index], tensor, error);
        if (result != 0) {
          device_->ops->release_outputs(device_, &outputs);
          return result;
        }
        tensors.push_back(std::move(tensor));
      }
    } catch (const std::bad_alloc &) {
      device_->ops->release_outputs(device_, &outputs);
      return -ENOMEM;
    } catch (const std::exception &exception) {
      device_->ops->release_outputs(device_, &outputs);
      error = exception.what();
      return -EFAULT;
    } catch (...) {
      device_->ops->release_outputs(device_, &outputs);
      error = "copy inference outputs failed";
      return -EFAULT;
    }

    result = device_->ops->release_outputs(device_, &outputs);
    if (result != 0) {
      error = failure("release inference outputs", result);
      return result;
    }

    try {
      return config_.decoder(tensors, frame, output, error);
    } catch (const std::exception &exception) {
      error = exception.what();
      return -EFAULT;
    } catch (...) {
      error = "inference output decoder threw an unknown exception";
      return -EFAULT;
    }
  }

private:
  InferenceEngineConfig config_;
  inference_device_t *device_{nullptr};
  inference_caps_t caps_{};
  ModelInfo model_;
  bool loaded_{false};
};

} // namespace

std::unique_ptr<InferenceEngine>
createPlatformInferenceEngine(const InferenceEngineConfig &config,
                              std::string &error) {
  (void)error;
  return std::make_unique<PlatformInferenceEngine>(config);
}

} // namespace iva
