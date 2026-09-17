/**
 * @file storage_types.h
 * @brief 存储基础类型定义（配置、信息、事件、枚举）
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <media_types.h>

namespace darkos::storage {

struct StorageConfig {
  std::filesystem::path root;
  std::uint64_t maxBytes{0};
  std::uint64_t minimumFreeBytes{0};
  std::uint64_t segmentMaxBytes{64ULL * 1024ULL * 1024ULL};
};

struct RecordingInfo {
  std::string id;
  std::string label;
  std::string alarmId;
  std::filesystem::path path;
  media::VideoCodec codec{media::VideoCodec::H264};
  std::uint64_t startTimestampNs{0};
  std::uint64_t endTimestampNs{0};
  std::uint64_t bytes{0};
  std::uint64_t packets{0};
};

struct StorageStats {
  std::uint64_t recordingCount{0};
  std::uint64_t managedBytes{0};
  std::uint64_t deletedRecordings{0};
  std::uint64_t writeErrors{0};
};

struct RecordingQuery {
  std::string label;
  std::string alarmId;
  std::uint64_t fromTimestampNs{0};
  std::uint64_t toTimestampNs{0};
  std::size_t limit{100};
};

} // namespace darkos::storage
