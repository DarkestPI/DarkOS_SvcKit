#pragma once

#include "media_buffer.h"

namespace darkos::media {

/** PCM 处理节点，用于重采样、AEC、降噪等。 */
class AudioFilter {
public:
  virtual ~AudioFilter() = default;
  virtual int process(AudioFramePtr input, AudioFramePtr &output) = 0;

protected:
  AudioFilter() = default;
};

} // namespace darkos::media
