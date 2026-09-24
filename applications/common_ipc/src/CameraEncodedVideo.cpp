#include "CameraEncodedVideo.h"

#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>
#include <svc_log.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

namespace common_ipc {
namespace {

constexpr char kTag[] = "camera_encoded_video";

int cameraPixelFormat(darkos::media::PixelFormat format) {
    return format == darkos::media::PixelFormat::Nv12 ? CAMERA_PIX_FMT_NV12 : 0;
}

struct FileFrame {
    std::vector<std::uint8_t> bytes;
    bool keyframe{false};
};

std::size_t startCodeSize(const std::vector<std::uint8_t> &data,
                          std::size_t offset) {
    if (offset + 4 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1)
        return 4;
    if (offset + 3 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1)
        return 3;
    return 0;
}

std::size_t findStartCode(const std::vector<std::uint8_t> &data,
                          std::size_t offset) {
    for (std::size_t i = offset; i + 3 <= data.size(); ++i) {
        if (startCodeSize(data, i) != 0)
            return i;
    }
    return data.size();
}

bool containsNalType(const std::vector<std::uint8_t> &data, std::size_t begin,
                     std::size_t end, std::uint8_t wanted) {
    for (std::size_t pos = findStartCode(data, begin); pos < end;) {
        const std::size_t prefix = startCodeSize(data, pos);
        if (pos + prefix < end && (data[pos + prefix] & 0x1fU) == wanted)
            return true;
        pos = findStartCode(data, pos + prefix + 1);
    }
    return false;
}

class FileEncodedVideo final : public EncodedVideoSource {
public:
    FileEncodedVideo(std::string path, std::uint32_t fps)
        : path_(std::move(path)), fps_(fps == 0 ? 30 : fps) {}
    ~FileEncodedVideo() override { stop(); }

    int start(PacketHandler packetHandler, ErrorHandler errorHandler,
              std::string &error) override {
        if (!packetHandler)
            return -EINVAL;
        if (running_.load())
            return -EALREADY;
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            error = "open virtual H.264 file failed: " + path_;
            return -ENOENT;
        }
        const std::vector<std::uint8_t> data(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::vector<std::size_t> accessUnits;
        for (std::size_t pos = findStartCode(data, 0); pos < data.size();) {
            const std::size_t prefix = startCodeSize(data, pos);
            if (pos + prefix < data.size() && (data[pos + prefix] & 0x1fU) == 9)
                accessUnits.push_back(pos);
            pos = findStartCode(data, pos + prefix + 1);
        }
        if (accessUnits.empty()) {
            error = "virtual H.264 file has no access-unit delimiters";
            return -EINVAL;
        }
        frames_.clear();
        frames_.reserve(accessUnits.size());
        for (std::size_t i = 0; i < accessUnits.size(); ++i) {
            const std::size_t begin = i == 0 ? 0 : accessUnits[i];
            const std::size_t end = i + 1 < accessUnits.size()
                                        ? accessUnits[i + 1]
                                        : data.size();
            if (end <= begin)
                continue;
            FileFrame frame;
            frame.bytes.assign(data.begin() + begin, data.begin() + end);
            frame.keyframe = containsNalType(data, begin, end, 5);
            frames_.push_back(std::move(frame));
        }
        if (frames_.empty()) {
            error = "virtual H.264 file contains no frames";
            return -EINVAL;
        }
        packetHandler_ = std::move(packetHandler);
        errorHandler_ = std::move(errorHandler);
        running_.store(true);
        try {
            thread_ = std::thread(&FileEncodedVideo::run, this);
        } catch (...) {
            running_.store(false);
            return -EAGAIN;
        }
        SVC_LOGI(kTag, "virtual H.264 source ready: file=%s frames=%zu fps=%u",
                 path_.c_str(), frames_.size(), fps_);
        return 0;
    }

    int startPreview(std::string &error) override {
        error = "virtual H.264 source does not support local preview";
        return -ENOTSUP;
    }

    int stop() override {
        running_.store(false);
        if (thread_.joinable())
            thread_.join();
        frames_.clear();
        packetHandler_ = {};
        errorHandler_ = {};
        return 0;
    }

private:
    void run() noexcept {
        pthread_setname_np(pthread_self(), "file-encoded");
        const auto duration = std::chrono::nanoseconds(1'000'000'000ULL / fps_);
        auto deadline = std::chrono::steady_clock::now();
        std::uint64_t timestampNs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline.time_since_epoch()).count());
        while (running_.load()) {
            for (const auto &frame : frames_) {
                if (!running_.load())
                    return;
                auto buffer = darkos::media::copyMediaBuffer(frame.bytes.data(),
                                                              frame.bytes.size());
                if (buffer == nullptr) {
                    if (errorHandler_)
                        errorHandler_(-ENOMEM, "copy virtual H.264 frame failed");
                    running_.store(false);
                    return;
                }
                try {
                    packetHandler_(std::make_shared<const darkos::media::VideoPacket>(
                        darkos::media::VideoPacket{std::move(buffer), timestampNs,
                                                   darkos::media::VideoCodec::H264,
                                                   frame.keyframe}));
                } catch (...) {
                    if (errorHandler_)
                        errorHandler_(-ENOMEM, "dispatch virtual H.264 frame failed");
                    running_.store(false);
                    return;
                }
                timestampNs += static_cast<std::uint64_t>(duration.count());
                deadline += duration;
                std::this_thread::sleep_until(deadline);
            }
        }
    }

    std::string path_;
    std::uint32_t fps_;
    std::vector<FileFrame> frames_;
    PacketHandler packetHandler_;
    ErrorHandler errorHandler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

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

int CameraEncodedVideo::startPreview(std::string &error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (previewStarted_)
        return 0;
    if (!running_.load()) {
        error = "encoded camera output is not running";
        return -EINVAL;
    }
    if (device_->ops->preview_start == nullptr ||
        device_->ops->preview_stop == nullptr) {
        error = "camera HAL does not provide local preview output";
        return -ENOTSUP;
    }

    const hw_module_t *module = nullptr;
    int rc = hw_get_module(DISPLAY_HARDWARE_MODULE_ID, &module);
    if (rc != 0) {
        error = "load display HAL failed (" + std::to_string(rc) + ")";
        return rc;
    }

    display_device_t *display = nullptr;
    rc = display_open(module, &display);
    if (rc != 0) {
        error = "open display failed (" + std::to_string(rc) + ")";
        return rc;
    }

    display_format_t requested{};
    requested.pixel_format = DISPLAY_FMT_NV12;
    requested.intf = DISPLAY_INTF_MIPI;
    requested.fps = config_.capture.fps;
    rc = display->ops->set_format(display, &requested);
    if (rc == 0)
        rc = display->ops->start(display);
    display_format_t panel{};
    if (rc == 0)
        rc = display->ops->get_format(display, &panel);
    if (rc != 0 || panel.width == 0 || panel.height == 0) {
        if (rc == 0)
            rc = -EIO;
        error = "start display output failed (" + std::to_string(rc) + ")";
        if (display->ops->stop != nullptr)
            display->ops->stop(display);
        display_close(display);
        return rc;
    }

    rc = device_->ops->preview_start(device_, panel.width, panel.height);
    if (rc != 0) {
        error = "start camera preview output failed (" + std::to_string(rc) + ")";
        display->ops->stop(display);
        display_close(display);
        return rc;
    }

    displayDevice_ = display;
    previewStarted_ = true;
    SVC_LOGI(kTag, "local preview started: panel=%ux%u", panel.width, panel.height);
    return 0;
}

int CameraEncodedVideo::stop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
    if (previewStarted_) {
        device_->ops->preview_stop(device_);
        displayDevice_->ops->stop(displayDevice_);
        display_close(displayDevice_);
        displayDevice_ = nullptr;
        previewStarted_ = false;
    }
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

std::unique_ptr<EncodedVideoSource>
createCameraEncodedVideo(const darkos::media::VideoPipelineConfig &config,
                         std::string &error) {
    const char *file = std::getenv("DARKOS_H264_FILE");
    if (file != nullptr && file[0] != '\0') {
        std::uint32_t fps = config.capture.fps;
        if (const char *value = std::getenv("DARKOS_H264_FPS")) {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(value, &end, 10);
            if (end != value && *end == '\0' && parsed > 0 && parsed <= 240)
                fps = static_cast<std::uint32_t>(parsed);
        }
        return std::unique_ptr<EncodedVideoSource>(new FileEncodedVideo(file, fps));
    }
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
    return std::unique_ptr<EncodedVideoSource>(
        new CameraEncodedVideo(device, config));
}

} // namespace common_ipc
