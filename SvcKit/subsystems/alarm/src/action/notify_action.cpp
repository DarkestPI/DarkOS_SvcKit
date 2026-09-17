#include "alarm_action.h"

#include <cerrno>
#include <utility>

namespace darkos::alarm {
namespace {

class CallbackAction final : public AlarmAction {
public:
  explicit CallbackAction(AlarmActionCallback callback)
      : callback_(std::move(callback)) {}

  int execute(const AlarmEvent &event, std::string &error) override {
    if (!callback_) {
      error = "alarm action callback is empty";
      return -EINVAL;
    }
    return callback_(event, error);
  }

private:
  AlarmActionCallback callback_;
};

} // namespace

std::shared_ptr<AlarmAction> createAlarmAction(AlarmActionCallback callback) {
  if (!callback)
    return nullptr;
  return std::make_shared<CallbackAction>(std::move(callback));
}

} // namespace darkos::alarm
