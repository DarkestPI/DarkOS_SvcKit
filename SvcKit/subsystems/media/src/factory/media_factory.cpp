#include "media_factory.h"

#include <audio/IAudio.h>
#include <camera/ICameraDevice.h>
#include <codec/ICodec.h>
#include <hardware/hardware.h>
#include <svc_log.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace darkos::media {
namespace {

constexpr char kTag[] = "svc_media";

std::string failure(const char *operation, int rc) {
  return std::string(operation) + " failed (" + std::to_string(rc) + ")";
}

std::uint32_t toHalPixelFormat(PixelFormat format) {
  switch (format) {
  case PixelFormat::Nv12:
    return CAMERA_PIX_FMT_NV12;
  case PixelFormat::Nv21:
    return CAMERA_PIX_FMT_NV21;
  case PixelFormat::Yuyv:
    return CAMERA_PIX_FMT_YUYV;
  case PixelFormat::Rgb24:
    return CAMERA_PIX_FMT_RGB24;
  }
  return 0;
}

bool fromHalPixelFormat(std::uint32_t format, PixelFormat &result) {
  switch (format) {
  case CAMERA_PIX_FMT_NV12:
    result = PixelFormat::Nv12;
    return true;
  case CAMERA_PIX_FMT_NV21:
    result = PixelFormat::Nv21;
    return true;
  case CAMERA_PIX_FMT_YUYV:
    result = PixelFormat::Yuyv;
    return true;
  case CAMERA_PIX_FMT_RGB24:
    result = PixelFormat::Rgb24;
    return true;
  default:
    return false;
  }
}

std::uint32_t toHalCodec(VideoCodec codec) {
  switch (codec) {
  case VideoCodec::H264:
    return CODEC_ID_H264;
  case VideoCodec::H265:
    return CODEC_ID_H265;
  case VideoCodec::Mjpeg:
    return CODEC_ID_MJPEG;
  }
  return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t toHalAudioFormat(AudioSampleFormat format) {
  switch (format) {
  case AudioSampleFormat::PcmS16Le:
    return AUDIO_FORMAT_PCM_S16LE;
  }
  return 0;
}

class PlatformVideoSource final : public VideoSource {
public:
  PlatformVideoSource(camera_device_t *device, VideoCaptureConfig format)
      : device_(device), format_(std::move(format)) {}

  ~PlatformVideoSource() override {
    stop();
    camera_close(device_);
  }

  int start(VideoFrameCallback callback) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load())
      return -EALREADY;
    if (!callback)
      return -EINVAL;
    callback_ = std::move(callback);
    running_.store(true);
    int rc = device_->ops->set_frame_callback(
        device_, &PlatformVideoSource::onHalFrame, this);
    if (rc == 0)
      rc = device_->ops->start(device_);
    if (rc != 0) {
      device_->ops->set_frame_callback(device_, nullptr, nullptr);
      callback_ = {};
      running_.store(false);
    }
    return rc;
  }

  int stop() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.exchange(false))
      return 0;
    const int rc = device_->ops->stop(device_);
    device_->ops->set_frame_callback(device_, nullptr, nullptr);
    callback_ = {};
    return rc;
  }

  bool running() const noexcept override { return running_.load(); }
  const VideoCaptureConfig &format() const noexcept override { return format_; }

private:
  static int onHalFrame(void *context, const camera_frame_t *frame) noexcept {
    auto *self = static_cast<PlatformVideoSource *>(context);
    if (self == nullptr || frame == nullptr || !self->running_.load())
      return 0;
    PixelFormat pixelFormat;
    if (!fromHalPixelFormat(frame->pixel_format, pixelFormat)) {
      SVC_LOGE(kTag, "camera returned unsupported pixel format: %u",
               frame->pixel_format);
      return 0;
    }
    const VideoFrameView view{static_cast<const std::uint8_t *>(frame->data),
                              frame->size,
                              frame->fd,
                              frame->timestamp_ns,
                              frame->width,
                              frame->height,
                              frame->stride,
                              pixelFormat,
                              frame->priv};
    try {
      self->callback_(view);
    } catch (const std::exception &exception) {
      SVC_LOGE(kTag, "video source callback threw: %s", exception.what());
    } catch (...) {
      SVC_LOGE(kTag, "video source callback threw an unknown exception");
    }
    return 0;
  }

  camera_device_t *device_;
  VideoCaptureConfig format_;
  VideoFrameCallback callback_;
  std::atomic<bool> running_{false};
  std::mutex mutex_;
};

class PlatformVideoEncoder final : public VideoEncoder {
public:
  PlatformVideoEncoder(codec_device_t *device, VideoEncoderConfig config,
                       std::vector<std::uint8_t> output)
      : device_(device), config_(std::move(config)),
        output_(std::move(output)) {}

  ~PlatformVideoEncoder() override {
    stop();
    codec_close(device_);
  }

  int start() override {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
      return -EALREADY;
    const int rc = device_->ops->start(device_);
    if (rc != 0)
      running_.store(false);
    return rc;
  }

  int stop() override {
    if (!running_.exchange(false))
      return 0;
    return device_->ops->stop(device_);
  }

  bool running() const noexcept override { return running_.load(); }

  int encode(const VideoFrameView &frame, EncodedPacketView &packet) override {
    if (!running_.load())
      return -EPIPE;
    if (frame.size > std::numeric_limits<std::uint32_t>::max())
      return -EOVERFLOW;

    codec_buffer_t input{};
    input.fd = frame.fd;
    input.data = const_cast<std::uint8_t *>(frame.data);
    input.size = static_cast<std::uint32_t>(frame.size);
    input.timestamp_ns = frame.timestampNs;
    input.priv = frame.opaque;
    codec_buffer_t output{};
    output.fd = -1;
    output.data = output_.data();
    output.size = static_cast<std::uint32_t>(output_.size());
    const int rc = device_->ops->encode(device_, &input, &output, 0);
    if (rc != 0)
      return rc;
    packet = EncodedPacketView{
        output_.data(), output.size, output.timestamp_ns, config_.codec,
        (output.flags & CODEC_BUFFER_FLAG_KEYFRAME) != 0};
    return 0;
  }

  const VideoEncoderConfig &config() const noexcept override { return config_; }

private:
  codec_device_t *device_;
  VideoEncoderConfig config_;
  std::vector<std::uint8_t> output_;
  std::atomic<bool> running_{false};
};

class PlatformAudioSource final : public AudioSource {
public:
  PlatformAudioSource(audio_device_t *device, AudioCaptureConfig format,
                      std::size_t bufferCapacity)
      : device_(device), format_(std::move(format)),
        readBuffer_(bufferCapacity) {}

  ~PlatformAudioSource() override {
    stop();
    audio_close(device_);
  }

  int start(AudioFrameCallback callback) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (started_)
      return -EALREADY;
    if (!callback)
      return -EINVAL;
    callback_ = std::move(callback);
    int rc = device_->ops->start(device_, AUDIO_DIRECTION_INPUT);
    if (rc != 0) {
      callback_ = {};
      return rc;
    }
    started_ = true;
    running_.store(true);
    try {
      thread_ = std::thread(&PlatformAudioSource::captureLoop, this);
    } catch (...) {
      running_.store(false);
      started_ = false;
      device_->ops->stop(device_, AUDIO_DIRECTION_INPUT);
      callback_ = {};
      return -EAGAIN;
    }
    return 0;
  }

  int stop() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!started_)
      return 0;
    running_.store(false);
    if (thread_.joinable())
      thread_.join();
    const int rc = device_->ops->stop(device_, AUDIO_DIRECTION_INPUT);
    pending_.clear();
    callback_ = {};
    started_ = false;
    return rc;
  }

  bool running() const noexcept override { return running_.load(); }
  const AudioCaptureConfig &format() const noexcept override { return format_; }

private:
  void captureLoop() noexcept {
    const std::uint32_t bytesPerFrame =
        format_.channelCount * sizeof(std::int16_t);
    const std::size_t callbackBytes =
        static_cast<std::size_t>(format_.framesPerBuffer) * bytesPerFrame;
    while (running_.load()) {
      audio_buffer_t buffer{};
      buffer.data = readBuffer_.data();
      buffer.size = static_cast<std::uint32_t>(readBuffer_.size());
      const int rc = device_->ops->read(device_, &buffer, 100);
      if (!running_.load())
        break;
      if (rc == -EAGAIN || rc == -ETIMEDOUT)
        continue;
      if (rc == -ENOSPC && buffer.size > readBuffer_.size()) {
        readBuffer_.resize(buffer.size);
        continue;
      }
      if (rc != 0) {
        SVC_LOGE(kTag, "audio capture failed: %d", rc);
        running_.store(false);
        break;
      }
      if (buffer.size == 0)
        continue;
      if (buffer.size > readBuffer_.size()) {
        SVC_LOGE(kTag, "audio HAL returned an oversized buffer: %u",
                 buffer.size);
        running_.store(false);
        break;
      }
      if (pending_.empty())
        pendingTimestampNs_ = buffer.timestamp_ns;
      pending_.insert(pending_.end(), readBuffer_.begin(),
                      readBuffer_.begin() + buffer.size);
      while (pending_.size() >= callbackBytes && running_.load()) {
        const AudioFrameView frame{
            pending_.data(),     callbackBytes,        pendingTimestampNs_,
            format_.sampleRate,  format_.channelCount, format_.framesPerBuffer,
            format_.sampleFormat};
        try {
          callback_(frame);
        } catch (const std::exception &exception) {
          SVC_LOGE(kTag, "audio source callback threw: %s", exception.what());
        } catch (...) {
          SVC_LOGE(kTag, "audio source callback threw an unknown exception");
        }
        pending_.erase(pending_.begin(), pending_.begin() + callbackBytes);
        pendingTimestampNs_ +=
            static_cast<std::uint64_t>(format_.framesPerBuffer) *
            1'000'000'000ull / format_.sampleRate;
      }
    }
  }

  audio_device_t *device_;
  AudioCaptureConfig format_;
  AudioFrameCallback callback_;
  std::vector<std::uint8_t> readBuffer_;
  std::vector<std::uint8_t> pending_;
  std::uint64_t pendingTimestampNs_{0};
  std::thread thread_;
  std::atomic<bool> running_{false};
  bool started_{false};
  std::mutex mutex_;
};

} // namespace

std::unique_ptr<VideoSource>
createPlatformVideoSource(const VideoCaptureConfig &config,
                          std::string &error) {
  const hw_module_t *module = nullptr;
  int rc = hw_get_module(CAMERA_HARDWARE_MODULE_ID, &module);
  if (rc != 0) {
    error = failure("load camera HAL", rc);
    return nullptr;
  }
  camera_device_t *device = nullptr;
  rc = camera_open_by_id(module, config.cameraId.c_str(), &device);
  if (rc != 0) {
    error = failure("open camera", rc);
    return nullptr;
  }
  if (device->ops == nullptr || device->ops->set_format == nullptr ||
      device->ops->get_format == nullptr ||
      device->ops->set_frame_callback == nullptr ||
      device->ops->start == nullptr || device->ops->stop == nullptr) {
    error = "camera HAL is missing required source operations";
    camera_close(device);
    return nullptr;
  }
  camera_format_t format{};
  format.width = config.width;
  format.height = config.height;
  format.pixel_format = toHalPixelFormat(config.pixelFormat);
  format.fps = config.fps;
  rc = device->ops->set_format(device, &format);
  if (rc == 0)
    rc = device->ops->get_format(device, &format);
  if (rc != 0) {
    error = failure("negotiate camera format", rc);
    camera_close(device);
    return nullptr;
  }
  VideoCaptureConfig negotiated = config;
  negotiated.width = format.width;
  negotiated.height = format.height;
  negotiated.fps = format.fps;
  if (!fromHalPixelFormat(format.pixel_format, negotiated.pixelFormat)) {
    error = "camera negotiated an unsupported pixel format";
    camera_close(device);
    return nullptr;
  }
  return std::make_unique<PlatformVideoSource>(device, std::move(negotiated));
}

std::unique_ptr<VideoEncoder> createPlatformVideoEncoder(
    const VideoEncoderConfig &config, const VideoCaptureConfig &inputFormat,
    std::size_t outputBufferCapacity, std::string &error) {
  const hw_module_t *module = nullptr;
  int rc = hw_get_module(CODEC_HARDWARE_MODULE_ID, &module);
  if (rc != 0) {
    error = failure("load codec HAL", rc);
    return nullptr;
  }
  codec_device_t *device = nullptr;
  rc = codec_open_by_id(module, config.codecId.c_str(), &device);
  if (rc != 0) {
    error = failure("open codec", rc);
    return nullptr;
  }
  if (device->ops == nullptr || device->ops->set_format == nullptr ||
      device->ops->start == nullptr || device->ops->stop == nullptr ||
      device->ops->encode == nullptr) {
    error = "codec HAL is missing required encoder operations";
    codec_close(device);
    return nullptr;
  }
  codec_format_t format{};
  format.codec = toHalCodec(config.codec);
  format.width = inputFormat.width;
  format.height = inputFormat.height;
  format.pixel_format = toHalPixelFormat(inputFormat.pixelFormat);
  format.bitrate_bps = config.bitrateBps;
  format.fps = inputFormat.fps;
  format.gop = config.gop;
  rc = device->ops->set_format(device, &format);
  if (rc != 0) {
    error = failure("configure video encoder", rc);
    codec_close(device);
    return nullptr;
  }
  if (outputBufferCapacity == 0)
    outputBufferCapacity =
        static_cast<std::size_t>(inputFormat.width) * inputFormat.height * 2u;
  if (outputBufferCapacity == 0 ||
      outputBufferCapacity > std::numeric_limits<std::uint32_t>::max()) {
    error = "video encoder output capacity is invalid";
    codec_close(device);
    return nullptr;
  }
  std::vector<std::uint8_t> output(outputBufferCapacity);
  return std::make_unique<PlatformVideoEncoder>(device, config,
                                                std::move(output));
}

std::unique_ptr<AudioSource>
createPlatformAudioSource(const AudioCaptureConfig &config,
                          std::string &error) {
  const hw_module_t *module = nullptr;
  int rc = hw_get_module(AUDIO_HARDWARE_MODULE_ID, &module);
  if (rc != 0) {
    error = failure("load audio HAL", rc);
    return nullptr;
  }
  audio_device_t *device = nullptr;
  rc = audio_open(module, &device);
  if (rc != 0) {
    error = failure("open audio", rc);
    return nullptr;
  }
  if (device->ops == nullptr || device->ops->set_format == nullptr ||
      device->ops->get_format == nullptr || device->ops->start == nullptr ||
      device->ops->stop == nullptr || device->ops->read == nullptr) {
    error = "audio HAL is missing required source operations";
    audio_close(device);
    return nullptr;
  }
  audio_format_t format{};
  format.sample_rate = config.sampleRate;
  format.channel_count = config.channelCount;
  format.format = toHalAudioFormat(config.sampleFormat);
  rc = device->ops->set_format(device, AUDIO_DIRECTION_INPUT, &format);
  if (rc == 0)
    rc = device->ops->get_format(device, AUDIO_DIRECTION_INPUT, &format);
  if (rc != 0) {
    error = failure("negotiate audio input format", rc);
    audio_close(device);
    return nullptr;
  }
  if (format.format != AUDIO_FORMAT_PCM_S16LE || format.sample_rate == 0 ||
      format.channel_count == 0 || config.framesPerBuffer == 0) {
    error = "audio input negotiated an invalid format";
    audio_close(device);
    return nullptr;
  }
  AudioCaptureConfig negotiated = config;
  negotiated.sampleRate = format.sample_rate;
  negotiated.channelCount = format.channel_count;
  const std::size_t capacity =
      static_cast<std::size_t>(negotiated.framesPerBuffer) *
      negotiated.channelCount * sizeof(std::int16_t);
  if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
    error = "audio input buffer capacity is invalid";
    audio_close(device);
    return nullptr;
  }
  return std::make_unique<PlatformAudioSource>(device, std::move(negotiated),
                                               capacity);
}

} // namespace darkos::media
