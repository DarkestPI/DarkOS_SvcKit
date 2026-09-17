#include "MediaSettings.h"

namespace generic_ipc {

darkos::media::MediaPipelineConfig defaultMediaPipelineConfig() {
    darkos::media::MediaPipelineConfig config;
    config.video.capture.width = 640;
    config.video.capture.height = 512;
    config.video.capture.fps = 30;
    config.video.encoder.codec = darkos::media::VideoCodec::H264;
    config.video.encoder.bitrateBps = 1'000'000;
    // 预览优先：每 0.5 秒产生一个 IDR，缩短 RTSP 客户端首帧等待。
    config.video.encoder.gop = 15;
    config.audio.capture.sampleRate = 16'000;
    config.audio.capture.channelCount = 1;
    config.audio.capture.framesPerBuffer = 320;
    config.audio.encoder.codec = darkos::media::AudioCodec::G711A;
    return config;
}

} // namespace generic_ipc
