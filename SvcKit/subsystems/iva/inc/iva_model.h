/**
 * @file iva_model.h
 * @brief 模型管理接口（加载、版本、热更新）
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace iva {

struct ModelInfo {
  std::string id; ///< 模型逻辑 ID。
  std::string version; ///< 模型版本字符串。
  std::string path; ///< 模型文件路径；格式由 Platform 推理适配器解释。
};

/** 模型文件的登记、激活和卸载接口；不负责具体张量推理。 */
class ModelManager {
public:
  virtual ~ModelManager() = default;

  ModelManager(const ModelManager &) = delete;
  ModelManager &operator=(const ModelManager &) = delete;

  /** 登记一个模型文件；同 ID 再次 load 表示更新版本。 */
  virtual int load(const ModelInfo &model, std::string &error) = 0;
  /** 卸载指定模型；如果它是当前模型，同时清除激活状态。 */
  virtual int unload(const std::string &id) = 0;
  /** 将已登记模型设为当前模型。 */
  virtual int activate(const std::string &id) = 0;
  /** 查询当前激活模型；没有激活模型时返回 false。 */
  virtual bool active(ModelInfo &output) const = 0;
  /** 返回当前已登记模型快照。 */
  virtual std::vector<ModelInfo> list() const = 0;

protected:
  ModelManager() = default;
};

std::unique_ptr<ModelManager> createModelManager();

} // namespace iva
