#include "MediaSettings.h"

namespace common_ipc {

darkos::media::MediaPipelineConfig defaultMediaPipelineConfig() {
    darkos::media::MediaPipelineConfig config;
#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    // RV1126B 的 ISP/VPSS/VENC 与本地 VO 共用内存带宽；4K 编码会让
    // VI chn 5 的显示通道掉帧。先用 1080p 保证 RTSP 与本地预览同时流畅，
    // 如需 4K 可再单独增加应用配置项做带宽评估。
    config.video.capture.width = 1920;
    config.video.capture.height = 1080;
#else
    config.video.capture.width = 640;
    config.video.capture.height = 512;
#endif
    config.video.capture.fps = 30;
    config.video.encoder.codec = darkos::media::VideoCodec::H264;
#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    config.video.encoder.bitrateBps = 4'000'000;
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

} // namespace common_ipc
