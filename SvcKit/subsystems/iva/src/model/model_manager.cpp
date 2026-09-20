#include "iva_model.h"

#include <cerrno>
#include <filesystem>
#include <map>
#include <utility>

namespace iva {
namespace {

class DefaultModelManager final : public ModelManager {
public:
  int load(const ModelInfo &model, std::string &error) override {
    if (model.id.empty() || model.path.empty()) {
      error = "model id and path are required";
      return -EINVAL;
    }
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(model.path, filesystemError) ||
        filesystemError) {
      error = "model path is not a regular file";
      return -ENOENT;
    }
    models_[model.id] = model;
    return 0;
  }

  int unload(const std::string &id) override {
    const auto iterator = models_.find(id);
    if (iterator == models_.end())
      return -ENOENT;
    models_.erase(iterator);
    if (activeId_ == id)
      activeId_.clear();
    return 0;
  }

  int activate(const std::string &id) override {
    if (models_.find(id) == models_.end())
      return -ENOENT;
    activeId_ = id;
    return 0;
  }

  bool active(ModelInfo &output) const override {
    const auto iterator = models_.find(activeId_);
    if (activeId_.empty() || iterator == models_.end())
      return false;
    output = iterator->second;
    return true;
  }

  std::vector<ModelInfo> list() const override {
    std::vector<ModelInfo> output;
    output.reserve(models_.size());
    for (const auto &entry : models_)
      output.push_back(entry.second);
    return output;
  }

private:
  std::map<std::string, ModelInfo> models_;
  std::string activeId_;
};

} // namespace

std::unique_ptr<ModelManager> createModelManager() {
  return std::make_unique<DefaultModelManager>();
}

} // namespace iva
