/**
 * @file storage_record.h
 * @brief 录像写入接口
 * @author your_name
 * @date 2026-09-16
 */
#pragma once

#include "storage_types.h"

#include <media_video_sink.h>

#include <string>

namespace darkos::storage {

class Recorder : public media::VideoPacketSink {
public:
  ~Recorder() override = default;
  virtual std::string currentRecordingId() const = 0;
  virtual std::uint64_t bytesWritten() const noexcept = 0;

protected:
  Recorder() = default;
};

} // namespace darkos::storage
