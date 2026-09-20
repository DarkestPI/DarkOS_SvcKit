/**
 * @file iva_factory.cpp
 * @brief IVA 工厂，桥接推理引擎与算法
 * @author your_name
 * @date 2026-09-16
 */

#include "iva_inference.h"

namespace iva {

std::unique_ptr<InferenceEngine> createCpuInferenceEngine(std::string &error);
std::unique_ptr<InferenceEngine>
createPlatformInferenceEngine(const InferenceEngineConfig &config,
                              std::string &error);

std::unique_ptr<InferenceEngine>
createInferenceEngine(const InferenceEngineConfig &config, std::string &error) {
  switch (config.backend) {
  case InferenceBackend::Software:
    return createCpuInferenceEngine(error);
  case InferenceBackend::Platform:
    return createPlatformInferenceEngine(config, error);
  }
  error = "unknown IVA inference backend";
  return nullptr;
}

} // namespace iva
