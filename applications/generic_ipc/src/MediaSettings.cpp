#include "MediaSettings.h"

namespace generic_ipc {

darkos::media::MediaPipelineConfig defaultMediaPipelineConfig() {
    darkos::media::MediaPipelineConfig config;
#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    // 4K RTSP 实验模式：GC8613 的 RKISP/VENC 输出 3840x2160@30。
    config.video.capture.width = 3840;
    config.video.capture.height = 2160;
#else
    config.video.capture.width = 640;
    config.video.capture.height = 512;
#endif
    config.video.capture.fps = 30;
    config.video.encoder.codec = darkos::media::VideoCodec::H264;
#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    // 4K 实验先使用 8 Mbps，局域网 RTSP 带宽约 1 MB/s。
    config.video.encoder.bitrateBps = 8'000'000;
    config.video.encoder.gop = 30;
#else
    config.video.encoder.bitrateBps = 1'000'000;
    // 预览优先：每 0.5 秒产生一个 IDR，缩短 RTSP 客户端首帧等待。
    config.video.encoder.gop = 15;
#endif
    // G.711 的硬件 AENC 使用标准 8 kHz 时基；16 kHz 会退回软件路径或被
    // 部分 Rockchip 音频编码器拒绝。
    config.audio.capture.sampleRate = 8'000;
    config.audio.capture.channelCount = 1;
    config.audio.capture.framesPerBuffer = 160;
    config.audio.encoder.codec = darkos::media::AudioCodec::G711A;
    return config;
}

} // namespace generic_ipc
