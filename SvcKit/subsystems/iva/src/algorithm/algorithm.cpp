#include "iva_algorithm.h"

#include <cerrno>
#include <exception>
#include <utility>

namespace iva {
namespace {

class CallbackAlgorithm final : public Algorithm {
public:
  explicit CallbackAlgorithm(AlgorithmCallback callback)
      : callback_(std::move(callback)) {}

  int process(const FrameView &frame, std::vector<Detection> &output,
              std::string &error) override {
    if (!callback_)
      return -EINVAL;
    try {
      return callback_(frame, output, error);
    } catch (const std::exception &exception) {
      error = exception.what();
      return -EFAULT;
    } catch (...) {
      error = "algorithm callback threw an unknown exception";
      return -EFAULT;
    }
  }

private:
  AlgorithmCallback callback_;
};

} // namespace

std::shared_ptr<Algorithm> createCallbackAlgorithm(AlgorithmCallback callback) {
  if (!callback)
    return nullptr;
  return std::make_shared<CallbackAlgorithm>(std::move(callback));
}

} // namespace iva
