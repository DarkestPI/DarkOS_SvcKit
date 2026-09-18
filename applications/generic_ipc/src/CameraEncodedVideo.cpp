#include "CameraEncodedVideo.h"

#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>
#include <svc_log.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

namespace generic_ipc {
namespace {

constexpr char kTag[] = "camera_encoded_video";

int cameraPixelFormat(darkos::media::PixelFormat format) {
    return format == darkos::media::PixelFormat::Nv12 ? CAMERA_PIX_FMT_NV12 : 0;
}

} // namespace

CameraEncodedVideo::CameraEncodedVideo(
    camera_device_t *device, darkos::media::VideoPipelineConfig config)
    : device_(device), config_(std::move(config)) {
    const std::size_t capacity =
        static_cast<std::size_t>(config_.capture.width) * config_.capture.height * 2u;
    output_.resize(capacity);
}

CameraEncodedVideo::~CameraEncodedVideo() {
    stop();
    camera_close(device_);
}

int CameraEncodedVideo::start(PacketHandler packetHandler,
                              ErrorHandler errorHandler, std::string &error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load())
        return -EALREADY;
    if (!camera_supports_encoded_output(device_)) {
        error = "camera HAL does not provide encoded video output";
        return -ENOTSUP;
    }
    if (!packetHandler)
        return -EINVAL;

    codec_format_t encodedConfig{};
    encodedConfig.codec = CODEC_ID_H264;
    encodedConfig.width = config_.capture.width;
    encodedConfig.height = config_.capture.height;
    encodedConfig.fps = config_.capture.fps;
    encodedConfig.pixel_format = cameraPixelFormat(config_.capture.pixelFormat);
    encodedConfig.bitrate_bps = config_.encoder.bitrateBps;
    encodedConfig.gop = config_.encoder.gop;
    if (encodedConfig.pixel_format == 0)
        return -EINVAL;

    const int rc = device_->ops->encoded_start(device_, &encodedConfig);
    if (rc != 0) {
        error = "start encoded camera output failed (" + std::to_string(rc) + ")";
        return rc;
    }
    packetHandler_ = std::move(packetHandler);
    errorHandler_ = std::move(errorHandler);
    lastTimestampNs_ = 0;
    timestampInitialized_ = false;
    running_.store(true);
    try {
        thread_ = std::thread(&CameraEncodedVideo::captureLoop, this);
    } catch (...) {
        running_.store(false);
        device_->ops->encoded_stop(device_);
        packetHandler_ = {};
        errorHandler_ = {};
        return -EAGAIN;
    }
    return 0;
}

int CameraEncodedVideo::stop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
    const int rc = camera_supports_encoded_output(device_)
                       ? device_->ops->encoded_stop(device_)
                       : 0;
    packetHandler_ = {};
    errorHandler_ = {};
    return rc;
}

void CameraEncodedVideo::captureLoop() noexcept {
    pthread_setname_np(pthread_self(), "camera-encoded");
    while (running_.load()) {
        codec_buffer_t encodedPacket{};
        encodedPacket.data = output_.data();
        encodedPacket.size = static_cast<std::uint32_t>(output_.size());
        encodedPacket.fd = -1;
        const int rc = device_->ops->encoded_get_packet(device_, &encodedPacket, 100);
        if (!running_.load())
            break;
        if (rc == -ETIMEDOUT || rc == -EAGAIN)
            continue;
        if (rc != 0) {
            SVC_LOGE(kTag, "encoded camera packet read failed: %d", rc);
            if (errorHandler_)
                errorHandler_(rc, "encoded camera packet read failed");
            running_.store(false);
            break;
        }
        if (encodedPacket.size == 0)
            continue;
        auto buffer = darkos::media::copyMediaBuffer(output_.data(), encodedPacket.size);
        if (buffer == nullptr) {
            SVC_LOGE(kTag, "copy encoded camera packet failed");
            if (errorHandler_)
                errorHandler_(-ENOMEM, "copy encoded camera packet failed");
            running_.store(false);
            break;
        }
        std::uint64_t timestampNs = encodedPacket.timestamp_ns;
        const std::uint64_t frameDurationNs =
            config_.capture.fps == 0
                ? 33'333'333ULL
                : 1'000'000'000ULL / config_.capture.fps;
        if (timestampNs == 0)
            timestampNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        if (timestampInitialized_ && timestampNs <= lastTimestampNs_) {
            timestampNs = lastTimestampNs_ + frameDurationNs;
        }
        lastTimestampNs_ = timestampNs;
        timestampInitialized_ = true;
        try {
            auto packet = std::make_shared<const darkos::media::VideoPacket>(
                darkos::media::VideoPacket{std::move(buffer), timestampNs,
                                           darkos::media::VideoCodec::H264,
                                           (encodedPacket.flags & CODEC_BUFFER_FLAG_KEYFRAME) != 0});
            packetHandler_(std::move(packet));
        } catch (...) {
            SVC_LOGE(kTag, "encoded camera packet dispatch failed");
            if (errorHandler_)
                errorHandler_(-ENOMEM, "encoded camera packet dispatch failed");
            running_.store(false);
            break;
        }
    }
}

std::unique_ptr<CameraEncodedVideo>
createCameraEncodedVideo(const darkos::media::VideoPipelineConfig &config,
                         std::string &error) {
    const hw_module_t *module = nullptr;
    int rc = hw_get_module(CAMERA_HARDWARE_MODULE_ID, &module);
    if (rc != 0) {
        error = "load camera HAL failed (" + std::to_string(rc) + ")";
        return nullptr;
    }
    camera_device_t *device = nullptr;
    rc = camera_open_by_id(module, config.capture.cameraId.c_str(), &device);
    if (rc != 0) {
        error = "open camera failed (" + std::to_string(rc) + ")";
        return nullptr;
    }
    if (config.capture.width == 0 || config.capture.height == 0 ||
        config.capture.width > std::numeric_limits<std::uint32_t>::max() /
                                    config.capture.height / 2u) {
        camera_close(device);
        error = "invalid encoded video dimensions";
        return nullptr;
    }
    return std::unique_ptr<CameraEncodedVideo>(
        new CameraEncodedVideo(device, config));
}

} // namespace generic_ipc
