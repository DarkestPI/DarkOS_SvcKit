#pragma once

#include <camera/ICameraDevice.h>
#include <media_buffer.h>
#include <media_types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace common_ipc {

class CameraEncodedVideo final {
public:
    using PacketHandler = std::function<void(darkos::media::VideoPacketPtr)>;
    using ErrorHandler = std::function<void(int, const std::string &)>;

    ~CameraEncodedVideo();

    CameraEncodedVideo(const CameraEncodedVideo &) = delete;
    CameraEncodedVideo &operator=(const CameraEncodedVideo &) = delete;

    int start(PacketHandler packetHandler, ErrorHandler errorHandler,
              std::string &error);
    int stop();

private:
    friend std::unique_ptr<CameraEncodedVideo>
    createCameraEncodedVideo(const darkos::media::VideoPipelineConfig &config,
                             std::string &error);

    CameraEncodedVideo(camera_device_t *device,
                       darkos::media::VideoPipelineConfig config);
    void captureLoop() noexcept;

    camera_device_t *device_;
    darkos::media::VideoPipelineConfig config_;
    std::vector<std::uint8_t> output_;
    PacketHandler packetHandler_;
    ErrorHandler errorHandler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::uint64_t lastTimestampNs_{0};
    bool timestampInitialized_{false};
};

std::unique_ptr<CameraEncodedVideo>
createCameraEncodedVideo(const darkos::media::VideoPipelineConfig &config,
                         std::string &error);

} // namespace common_ipc
