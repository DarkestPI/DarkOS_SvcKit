#include "alarm_manager.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace darkos::alarm {
namespace {

std::uint64_t nowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct State {
  State(EventLoop &value, AlarmConfig configuration)
      : loop(value), config(std::move(configuration)) {}
  EventLoop &loop;
  AlarmConfig config;
  mutable std::mutex mutex;
  std::condition_variable handled;
  std::vector<std::shared_ptr<AlarmRule>> rules;
  std::vector<std::shared_ptr<AlarmAction>> actions;
  std::vector<AlarmRecord> records;
  AlarmStats stats;
  std::uint64_t nextId{1};
};

bool appendEvent(const State &state, const AlarmRecord &record) {
  std::ofstream output(state.config.journalPath, std::ios::app);
  if (!output)
    return false;
  output << 'E' << ' ' << std::quoted(record.event.id) << ' '
         << std::quoted(record.event.source) << ' '
         << std::quoted(record.event.type) << ' '
         << std::quoted(record.event.message) << ' '
         << static_cast<unsigned>(record.event.severity) << ' '
         << record.event.timestampNs << ' ' << record.successfulActions << ' '
         << record.failedActions << '\n';
  return static_cast<bool>(output);
}

bool appendAcknowledgement(const State &state, const AlarmRecord &record) {
  std::ofstream output(state.config.journalPath, std::ios::app);
  if (!output)
    return false;
  output << 'A' << ' ' << std::quoted(record.event.id) << ' '
         << record.acknowledgedTimestampNs << '\n';
  return static_cast<bool>(output);
}

void loadJournal(State &state) {
  std::ifstream input(state.config.journalPath);
  char kind = 0;
  while (input >> kind) {
    if (kind == 'E') {
      AlarmRecord record;
      unsigned severity = 0;
      if (!(input >> std::quoted(record.event.id) >>
            std::quoted(record.event.source) >> std::quoted(record.event.type) >>
            std::quoted(record.event.message) >> severity >>
            record.event.timestampNs >> record.successfulActions >>
            record.failedActions))
        break;
      record.event.severity = static_cast<AlarmSeverity>(severity);
      state.records.push_back(std::move(record));
    } else if (kind == 'A') {
      std::string id;
      std::uint64_t timestamp = 0;
      if (!(input >> std::quoted(id) >> timestamp))
        break;
      const auto record = std::find_if(
          state.records.begin(), state.records.end(),
          [&](const AlarmRecord &value) { return value.event.id == id; });
      if (record != state.records.end()) {
        record->state = AlarmState::Acknowledged;
        record->acknowledgedTimestampNs = timestamp;
      }
    } else {
      std::string ignored;
      std::getline(input, ignored);
    }
  }
  if (state.records.size() > state.config.maximumRecords)
    state.records.erase(state.records.begin(),
                        state.records.end() - state.config.maximumRecords);
}

void processEvent(const std::shared_ptr<State> &state, AlarmEvent event) {
  std::vector<std::shared_ptr<AlarmAction>> actions;
  bool accepted = false;
  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    ++state->stats.received;
    if (state->rules.empty()) {
      accepted = true;
    } else {
      for (const auto &rule : state->rules) {
        if (rule->accept(event)) {
          accepted = true;
          break;
        }
      }
    }
    if (!accepted) {
      ++state->stats.suppressed;
      state->handled.notify_all();
      return;
    }
    actions = state->actions;
  }

  AlarmRecord record;
  record.event = std::move(event);
  for (const auto &action : actions) {
    std::string error;
    if (action->execute(record.event, error) == 0)
      ++record.successfulActions;
    else
      ++record.failedActions;
  }

  {
    const std::lock_guard<std::mutex> lock(state->mutex);
    ++state->stats.accepted;
    state->stats.actionFailures += record.failedActions;
    state->records.push_back(record);
    if (state->records.size() > state->config.maximumRecords)
      state->records.erase(state->records.begin());
    appendEvent(*state, record);
    state->handled.notify_all();
  }
}

class AlarmManagerImpl final : public AlarmManager {
public:
  explicit AlarmManagerImpl(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  int addRule(std::shared_ptr<AlarmRule> rule) override {
    if (!rule)
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->rules.push_back(std::move(rule));
    return 0;
  }

  int addAction(std::shared_ptr<AlarmAction> action) override {
    if (!action)
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->actions.push_back(std::move(action));
    return 0;
  }

  int publish(AlarmEvent event) override {
    if (event.source.empty() || event.type.empty())
      return -EINVAL;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      if (event.id.empty())
        event.id = std::to_string(nowNs()) + "-" +
                   std::to_string(state_->nextId++);
    }
    if (event.timestampNs == 0)
      event.timestampNs = nowNs();
    return state_->loop.post(
               [state = state_, event = std::move(event)]() mutable {
                 processEvent(state, std::move(event));
               })
               ? 0
               : -ESHUTDOWN;
  }

  int waitForHandled(std::uint64_t minimum, int timeoutMs) override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    const auto ready = [&] {
      return state_->stats.accepted + state_->stats.suppressed >= minimum;
    };
    if (timeoutMs < 0) {
      state_->handled.wait(lock, ready);
      return 0;
    }
    return state_->handled.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                    ready)
               ? 0
               : -ETIMEDOUT;
  }

  std::vector<AlarmRecord>
  query(const AlarmQuery &filter) const override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<AlarmRecord> output;
    for (auto iterator = state_->records.rbegin();
         iterator != state_->records.rend() && output.size() < filter.limit;
         ++iterator) {
      if (!filter.source.empty() && iterator->event.source != filter.source)
        continue;
      if (!filter.type.empty() && iterator->event.type != filter.type)
        continue;
      if (filter.activeOnly && iterator->state != AlarmState::Active)
        continue;
      output.push_back(*iterator);
    }
    return output;
  }

  int acknowledge(const std::string &id) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    const auto record = std::find_if(
        state_->records.begin(), state_->records.end(),
        [&](const AlarmRecord &value) { return value.event.id == id; });
    if (record == state_->records.end())
      return -ENOENT;
    if (record->state == AlarmState::Acknowledged)
      return 0;
    record->state = AlarmState::Acknowledged;
    record->acknowledgedTimestampNs = nowNs();
    return appendAcknowledgement(*state_, *record) ? 0 : -EIO;
  }

  AlarmStats stats() const noexcept override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
  }

private:
  std::shared_ptr<State> state_;
};

} // namespace

std::unique_ptr<AlarmManager>
AlarmManager::create(EventLoop &eventLoop, const AlarmConfig &config,
                     std::string &error) {
  if (config.journalPath.empty() || config.maximumRecords == 0) {
    error = "alarm journal path and maximum records are required";
    return nullptr;
  }
  std::error_code filesystemError;
  const auto parent = config.journalPath.parent_path();
  if (!parent.empty())
    std::filesystem::create_directories(parent, filesystemError);
  if (filesystemError) {
    error = "create alarm journal directory failed: " +
            filesystemError.message();
    return nullptr;
  }
  auto state = std::make_shared<State>(eventLoop, config);
  loadJournal(*state);
  auto manager = std::unique_ptr<AlarmManager>(
      new (std::nothrow) AlarmManagerImpl(std::move(state)));
  if (!manager) {
    error = "out of memory creating alarm manager";
    return nullptr;
  }
  error.clear();
  return manager;
}

} // namespace darkos::alarm
