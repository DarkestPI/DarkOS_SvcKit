/**
 * @file cpu_engine.cpp
 * @brief CPU 推理引擎
 * @author your_name
 * @date 2026-09-16
 */

#include "iva_inference.h"

#include <cerrno>

namespace iva {

std::unique_ptr<InferenceEngine>
createCpuInferenceEngine(std::string &error) {
  error = "CPU inference backend is reserved for a future software model runtime";
  return nullptr;
}

} // namespace iva
