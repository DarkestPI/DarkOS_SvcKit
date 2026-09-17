#include "Probes.h"

#include "MediaSettings.h"

#include <media_pipeline.h>
#include <network_manager.h>
#include <storage_manager.h>
#include <svc_log.h>

#include <cstddef>
#include <string>
#include <vector>

namespace generic_ipc {

namespace {

constexpr char kTag[] = "generic_ipc";

} // namespace

bool probeNetworkState() {
    std::vector<darkos::network::NetworkInterface> interfaces;
    std::string error;
    const int result = darkos::network::NetworkManager::snapshot(interfaces, error);
    if (result != 0) {
        // Restricted containers may forbid the netlink socket used by getifaddrs.
        SVC_LOGW(kTag, "network snapshot unavailable: %s (%d)", error.c_str(), result);
        return true;
    }
    for (const auto &interface : interfaces) {
        SVC_LOGI(kTag, "network interface: name=%s index=%u up=%d running=%d addresses=%zu",
                 interface.name.c_str(), interface.index, interface.up, interface.running,
                 interface.addresses.size());
    }
    return !interfaces.empty();
}

bool runMediaPipelineProbe(const std::filesystem::path &storageDirectory) {
    const darkos::media::MediaPipelineConfig config = defaultMediaPipelineConfig();
    std::string error;
    auto pipeline = darkos::media::createMediaPipeline(config, error);
    if (pipeline == nullptr) {
        SVC_LOGE(kTag, "create media pipeline failed: %s", error.c_str());
        return false;
    }

    darkos::storage::StorageConfig storageConfig;
    storageConfig.root = storageDirectory / "recordings";
    storageConfig.maxBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    storageConfig.minimumFreeBytes = 128ULL * 1024ULL * 1024ULL;
    auto storage = darkos::storage::StorageManager::create(storageConfig, error);
    auto recorder = storage != nullptr ? storage->createRecorder("probe", "", error) : nullptr;
    if (storage == nullptr || recorder == nullptr) {
        SVC_LOGE(kTag, "create storage recorder failed: %s", error.c_str());
        return false;
    }

    auto videoSink = darkos::media::createVideoProbeSink();
    auto audioSink = darkos::media::createAudioProbeSink();
    darkos::media::SinkId videoSinkId = 0;
    darkos::media::SinkId audioSinkId = 0;
    darkos::media::SinkId recorderSinkId = 0;
    const darkos::media::MediaQueueConfig sinkQueue{8, darkos::media::BackpressurePolicy::DropOldest};
    if (pipeline->addVideoSink(videoSink, sinkQueue, videoSinkId) != 0 ||
        pipeline->addVideoSink(recorder, sinkQueue, recorderSinkId) != 0 ||
        pipeline->addAudioSink(audioSink, sinkQueue, audioSinkId) != 0) {
        SVC_LOGE(kTag, "add media sink failed");
        return false;
    }

    const int startResult = pipeline->start();
    if (startResult != 0) {
        SVC_LOGE(kTag, "start media pipeline failed: %d", startResult);
        return false;
    }
    const bool receivedEnoughPackets =
        videoSink->waitForPackets(3, 5'000) == 0 && audioSink->waitForPackets(3, 5'000) == 0;
    const int stopResult = pipeline->stop();
    if (stopResult != 0) {
        SVC_LOGE(kTag, "stop media pipeline failed: %d", stopResult);
        return false;
    }

    bool sawRunning = false;
    bool sawStopped = false;
    darkos::media::MediaEvent event;
    while (pipeline->waitEvent(event, 0) == 0) {
        sawRunning = sawRunning || event.state == darkos::media::PipelineState::Running;
        sawStopped = sawStopped || event.state == darkos::media::PipelineState::Stopped;
        SVC_LOGI(kTag, "media event: component=%s state=%u code=%d message=%s", event.component.c_str(),
                 static_cast<unsigned>(event.state), event.code, event.message.c_str());
    }
    if (!sawRunning || !sawStopped) {
        SVC_LOGE(kTag, "media lifecycle event sequence incomplete");
        return false;
    }
    if (!receivedEnoughPackets) {
        SVC_LOGE(kTag, "media pipeline timed out waiting for audio/video data");
        return false;
    }

    const darkos::media::VideoProbeStats videoStats = videoSink->snapshot();
    const darkos::media::AudioProbeStats audioStats = audioSink->snapshot();
    const darkos::storage::StorageStats storageStats = storage->stats();
    if (storageStats.recordingCount == 0 || recorder->bytesWritten() == 0) {
        SVC_LOGE(kTag, "storage recorder did not persist encoded video");
        return false;
    }
    SVC_LOGI(kTag,
             "SvcKit Media A/V pipeline OK: video_packets=%zu video_bytes=%zu keyframes=%zu "
             "audio_packets=%zu audio_bytes=%zu",
             static_cast<std::size_t>(videoStats.packetCount), static_cast<std::size_t>(videoStats.byteCount),
             static_cast<std::size_t>(videoStats.keyframeCount), static_cast<std::size_t>(audioStats.packetCount),
             static_cast<std::size_t>(audioStats.byteCount));
    SVC_LOGI(kTag, "SvcKit Storage OK: recordings=%llu managed_bytes=%llu",
             static_cast<unsigned long long>(storageStats.recordingCount),
             static_cast<unsigned long long>(storageStats.managedBytes));
    return true;
}

} // namespace generic_ipc
