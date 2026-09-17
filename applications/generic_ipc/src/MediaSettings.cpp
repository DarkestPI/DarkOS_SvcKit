#include "MediaSettings.h"

namespace generic_ipc {

darkos::media::MediaPipelineConfig defaultMediaPipelineConfig() {
    darkos::media::MediaPipelineConfig config;
    config.video.capture.width = 320;
    config.video.capture.height = 240;
    config.video.capture.fps = 15;
    config.video.encoder.codec = darkos::media::VideoCodec::H264;
    config.video.encoder.bitrateBps = 256'000;
    config.video.encoder.gop = 15;
    config.audio.capture.sampleRate = 16'000;
    config.audio.capture.channelCount = 1;
    config.audio.capture.framesPerBuffer = 320;
    config.audio.encoder.codec = darkos::media::AudioCodec::G711A;
    return config;
}

} // namespace generic_ipc
