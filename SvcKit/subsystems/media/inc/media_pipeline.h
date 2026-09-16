#pragma once

#include "media_buffer.h"
#include "media_types.h"

#include <memory>
#include <string>

namespace darkos::media {

/**
 * 一路采集编码视频和可选麦克风 PCM 采集的上层门面。
 *
 * 实现只通过 Platform Camera/Codec SPI 访问硬件，不包含任何 Rockchip、V4L2
 * 或 x264 类型。PacketCallback 和 AudioFrameCallback 分别在视频采集线程和
 * 音频采集线程同步执行，不能阻塞、不能保存 view.data，也不能从回调内调用
 * start()/stop()；stop() 返回后不会再产生回调。
 */
class MediaPipeline {
public:
  virtual ~MediaPipeline() = default;

  MediaPipeline(const MediaPipeline &) = delete;
  MediaPipeline &operator=(const MediaPipeline &) = delete;

  /** 启动编码管线。重复启动返回 -EALREADY。 */
  virtual int start() = 0;

  /** 停止管线。未启动时幂等成功。 */
  virtual int stop() = 0;

  virtual bool running() const noexcept = 0;

protected:
  MediaPipeline() = default;
};

/**
 * 使用当前 Platform HAL 创建 Camera→Codec 管线。
 *
 * 创建阶段完成模块加载、设备打开与格式协商；失败返回 nullptr，并把可读原因
 * 写入 error。返回对象独占相应 HAL device，析构时会先 stop 再 close。
 */
std::unique_ptr<MediaPipeline>
createMediaPipeline(const VideoPipelineConfig &config, PacketCallback callback,
                    std::string &error);

/**
 * 创建 Camera→Codec + 麦克风 PCM 采集音视频管线。
 *
 * videoCallback 接收编码视频，audioCallback 接收尚未压缩的 PCM。AAC、G.711
 * 等音频编码属于后续 SvcKit Media 编码节点，不由 Platform Audio HAL 假装完成。
 */
std::unique_ptr<MediaPipeline>
createMediaPipeline(const MediaPipelineConfig &config,
                    PacketCallback videoCallback,
                    AudioFrameCallback audioCallback, std::string &error);

} // namespace darkos::media
