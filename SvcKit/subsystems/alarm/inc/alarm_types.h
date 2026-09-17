/**
 * @file alarm_types.h
 * @brief 报警基础类型定义（报警、状态、级别、枚举）
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace darkos::alarm {

enum class AlarmSeverity : std::uint32_t { Info, Warning, Critical };
enum class AlarmState : std::uint32_t { Active, Acknowledged };

struct AlarmEvent {
  std::string id;
  std::string source;
  std::string type;
  std::string message;
  AlarmSeverity severity{AlarmSeverity::Warning};
  std::uint64_t timestampNs{0};
};

struct AlarmRecord {
  AlarmEvent event;
  AlarmState state{AlarmState::Active};
  std::uint64_t acknowledgedTimestampNs{0};
  std::uint32_t successfulActions{0};
  std::uint32_t failedActions{0};
};

struct AlarmConfig {
  std::filesystem::path journalPath;
  std::size_t maximumRecords{1000};
};

struct AlarmQuery {
  std::string source;
  std::string type;
  bool activeOnly{false};
  std::size_t limit{100};
};

struct AlarmStats {
  std::uint64_t received{0};
  std::uint64_t accepted{0};
  std::uint64_t suppressed{0};
  std::uint64_t actionFailures{0};
};

} // namespace darkos::alarm
