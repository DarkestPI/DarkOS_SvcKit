#pragma once

#include "media_audio_source.h"
#include "media_video_codec.h"
#include "media_video_source.h"

#include <memory>
#include <string>

namespace darkos::media {

/** 创建 Platform Camera 驱动的视频源，并完成格式协商。 */
std::unique_ptr<VideoSource>
createPlatformVideoSource(const VideoCaptureConfig &config, std::string &error);

/** 创建 Platform Codec 驱动的视频编码器。inputFormat 必须是 Source 协商值。 */
std::unique_ptr<VideoEncoder> createPlatformVideoEncoder(
    const VideoEncoderConfig &config, const VideoCaptureConfig &inputFormat,
    std::size_t outputBufferCapacity, std::string &error);

/** 创建 Platform Audio Input 驱动的 PCM 音频源。 */
std::unique_ptr<AudioSource>
createPlatformAudioSource(const AudioCaptureConfig &config, std::string &error);

} // namespace darkos::media
