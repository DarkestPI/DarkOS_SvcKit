/**
 * @file storage_manager.h
 * @brief 存储管理器门面
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "storage_playback.h"
#include "storage_record.h"
#include "storage_types.h"

#include <memory>
#include <string>
#include <vector>

namespace darkos::storage {

class StorageManager : public PlaybackReader {
public:
  static std::unique_ptr<StorageManager>
  create(const StorageConfig &config, std::string &error);
  ~StorageManager() override = default;

  StorageManager(const StorageManager &) = delete;
  StorageManager &operator=(const StorageManager &) = delete;

  virtual std::shared_ptr<Recorder>
  createRecorder(const std::string &label, const std::string &alarmId,
                 std::string &error) = 0;
  virtual std::vector<RecordingInfo>
  query(const RecordingQuery &query = {}) const = 0;
  virtual int remove(const std::string &recordingId) = 0;
  virtual int cleanup() = 0;
  virtual StorageStats stats() const noexcept = 0;

protected:
  StorageManager() = default;
};

} // namespace darkos::storage
