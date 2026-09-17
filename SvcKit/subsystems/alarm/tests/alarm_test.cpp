#include <alarm_manager.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("darkos-alarm-test-" + std::to_string(getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root, ignored);

  std::unique_ptr<darkos::EventLoop> loop(darkos::EventLoop::create());
  if (!loop)
    return 1;
  darkos::alarm::AlarmConfig config{root / "alarms.journal", 10};
  std::string error;
  auto manager = darkos::alarm::AlarmManager::create(*loop, config, error);
  if (!manager)
    return 1;
  auto rule = darkos::alarm::createAlarmRule(
      "network", "link_down", darkos::alarm::AlarmSeverity::Warning,
      1'000'000'000ULL);
  std::atomic<unsigned> actions{0};
  auto action = darkos::alarm::createAlarmAction(
      [&](const darkos::alarm::AlarmEvent &, std::string &) {
        ++actions;
        return 0;
      });
  if (manager->addRule(rule) != 0 || manager->addAction(action) != 0)
    return 1;
  std::thread worker([&] { loop->run(); });

  darkos::alarm::AlarmEvent event;
  event.source = "network";
  event.type = "link_down";
  event.message = "eth0 is down";
  event.severity = darkos::alarm::AlarmSeverity::Critical;
  event.timestampNs = 100;
  if (manager->publish(event) != 0 || manager->publish(event) != 0 ||
      manager->waitForHandled(2, 1000) != 0) {
    loop->quit();
    worker.join();
    return 1;
  }
  const auto records = manager->query();
  const auto stats = manager->stats();
  if (records.size() != 1 || actions.load() != 1 || stats.received != 2 ||
      stats.suppressed != 1 || manager->acknowledge(records.front().event.id) != 0) {
    loop->quit();
    worker.join();
    return 1;
  }
  loop->quit();
  worker.join();
  manager.reset();

  std::unique_ptr<darkos::EventLoop> secondLoop(darkos::EventLoop::create());
  manager = darkos::alarm::AlarmManager::create(*secondLoop, config, error);
  const auto restored = manager ? manager->query() : std::vector<darkos::alarm::AlarmRecord>{};
  const bool valid = restored.size() == 1 &&
                     restored.front().state == darkos::alarm::AlarmState::Acknowledged;
  std::filesystem::remove_all(root, ignored);
  return valid ? 0 : 1;
}
