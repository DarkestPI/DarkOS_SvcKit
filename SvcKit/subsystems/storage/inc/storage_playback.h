/**
 * @file storage_playback.h
 * @brief 回放读取接口
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace darkos::storage {

class PlaybackReader {
public:
  virtual ~PlaybackReader() = default;
  virtual int read(const std::string &recordingId, std::uint64_t offset,
                   std::size_t maximumBytes,
                   std::vector<std::uint8_t> &output) const = 0;

protected:
  PlaybackReader() = default;
};

} // namespace darkos::storage
