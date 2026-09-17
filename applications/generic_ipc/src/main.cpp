#include <base/EventLoop.h>
#include <alarm_manager.h>
#include <media_pipeline.h>
#include <network_manager.h>
#include <RtspServer.h>
#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>
#include <svc_log.h>
#include <storage_manager.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr char kTag[] = "generic_ipc";

std::filesystem::path defaultConfigDirectory() {
    if (const char *configured = std::getenv("DARKOS_CONFIG_DIR"); configured && configured[0] != '\0')
        return configured;

    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error)
        return executable.parent_path().parent_path() / "etc";

    return "/etc";
}

struct Options {
    explicit Options(const std::filesystem::path &configDirectory)
        : boardConfig((configDirectory / "board.json").string()), appConfig((configDirectory / "app.json").string()),
          storageDirectory(configDirectory.parent_path() / "data") {}

    std::string boardConfig;
    std::string appConfig;
    std::filesystem::path storageDirectory;
    bool serve{false};
    std::uint16_t rtspPort{8554};
};

bool parseOptions(int argc, char **argv, Options &options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--board-config" || argument == "--app-config") && index + 1 < argc) {
            std::string &destination = argument == "--board-config" ? options.boardConfig : options.appConfig;
            destination = argv[++index];
        } else if (argument == "--serve") {
            options.serve = true;
        } else if (argument == "--storage-dir" && index + 1 < argc) {
            options.storageDirectory = argv[++index];
        } else if (argument == "--rtsp-port" && index + 1 < argc) {
            char *end = nullptr;
            const unsigned long port = std::strtoul(argv[++index], &end, 10);
            if (end == nullptr || *end != '\0' || port == 0 || port > 65535) {
                SVC_LOGE(kTag, "invalid RTSP port");
                return false;
            }
            options.rtspPort = static_cast<std::uint16_t>(port);
        } else {
            SVC_LOGE(kTag,
                     "usage: %s [--board-config path] [--app-config path] "
                     "[--storage-dir path] [--serve] [--rtsp-port port]",
                     argv[0]);
            return false;
        }
    }
    return true;
}

bool runMediaPipelineProbe(const std::filesystem::path &storageDirectory) {
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

    std::string error;

    // 创建媒体管线
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

    // 添加音视频探针 Sink；队列满时丢弃最旧数据，以免阻塞媒体管线。
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

    // 启动管线
    const int startResult = pipeline->start();
    if (startResult != 0) {
        SVC_LOGE(kTag, "start media pipeline failed: %d", startResult);
        return false;
    }

    // 同时收到至少 3 个 H.264 和 3 个 G.711A 编码包，或超时 5 秒。
    const bool receivedEnoughPackets =
        videoSink->waitForPackets(3, 5'000) == 0 && audioSink->waitForPackets(3, 5'000) == 0;

    // 停止管线
    const int stopResult = pipeline->stop();
    if (stopResult != 0) {
        SVC_LOGE(kTag, "stop media pipeline failed: %d", stopResult);
        return false;
    }

    // 消费控制面事件，验证生命周期事件没有停留在无人读取的内部队列。
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

    // 销毁管线
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

bool probeNetworkState() {
    std::vector<darkos::network::NetworkInterface> interfaces;
    std::string error;
    const int result = darkos::network::NetworkManager::snapshot(interfaces, error);
    if (result != 0) {
        // Network availability is a degraded state for this host probe, not a
        // reason to suppress local media service startup (restricted
        // containers may forbid the netlink socket used by getifaddrs).
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

bool runCaptureService(const std::filesystem::path &storageDirectory, std::uint16_t rtspPort) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        SVC_LOGE(kTag, "failed to block termination signals");
        return false;
    }

    std::unique_ptr<darkos::EventLoop> loop(darkos::EventLoop::create());
    if (loop == nullptr) {
        SVC_LOGE(kTag, "create EventLoop failed");
        return false;
    }
    const int signalFd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signalFd < 0 ||
        !loop->watchFd(signalFd, EPOLLIN, [&]([[maybe_unused]] std::uint32_t events) {
            signalfd_siginfo info{};
            if (read(signalFd, &info, sizeof(info)) == sizeof(info))
                SVC_LOGI(kTag, "signal %u received, stopping", info.ssi_signo);
            loop->quit();
        })) {
        SVC_LOGE(kTag, "install signal watcher failed: %s", std::strerror(errno));
        if (signalFd >= 0)
            close(signalFd);
        return false;
    }

    std::string error;
    darkos::alarm::AlarmConfig alarmConfig{storageDirectory / "alarms.journal", 1000};
    auto alarmManager = darkos::alarm::AlarmManager::create(*loop, alarmConfig, error);
    if (alarmManager == nullptr ||
        alarmManager->addRule(darkos::alarm::createAlarmRule(
            "network", "", darkos::alarm::AlarmSeverity::Warning, 5'000'000'000ULL)) != 0 ||
        alarmManager->addRule(darkos::alarm::createAlarmRule(
            "media", "", darkos::alarm::AlarmSeverity::Warning, 1'000'000'000ULL)) != 0 ||
        alarmManager->addRule(darkos::alarm::createAlarmRule(
            "system", "", darkos::alarm::AlarmSeverity::Info, 0)) != 0 ||
        alarmManager->addAction(darkos::alarm::createAlarmAction(
            [](const darkos::alarm::AlarmEvent &event, std::string &) {
                SVC_LOGW(kTag, "alarm: id=%s source=%s type=%s severity=%u message=%s", event.id.c_str(),
                         event.source.c_str(), event.type.c_str(), static_cast<unsigned>(event.severity),
                         event.message.c_str());
                return 0;
            })) != 0) {
        SVC_LOGE(kTag, "create alarm service failed: %s", error.c_str());
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }

    auto networkManager = darkos::network::NetworkManager::create(*loop, error);
    if (networkManager != nullptr) {
        const int result = networkManager->start([&](const darkos::network::NetworkEvent &event) {
            SVC_LOGI(kTag, "network event: type=%u interface=%s address=%s up=%d running=%d code=%d",
                     static_cast<unsigned>(event.type), event.interfaceName.c_str(), event.address.c_str(), event.up,
                     event.running, event.code);
            if ((event.type == darkos::network::NetworkEventType::LinkChanged && !event.up) ||
                event.type == darkos::network::NetworkEventType::Error) {
                darkos::alarm::AlarmEvent alarm;
                alarm.source = "network";
                alarm.type = event.type == darkos::network::NetworkEventType::Error ? "monitor_error" : "link_down";
                alarm.message = event.interfaceName.empty() ? "network monitor failure"
                                                            : event.interfaceName + " link is down";
                alarm.severity = event.type == darkos::network::NetworkEventType::Error
                                     ? darkos::alarm::AlarmSeverity::Critical
                                     : darkos::alarm::AlarmSeverity::Warning;
                alarm.timestampNs = event.timestampNs;
                alarmManager->publish(std::move(alarm));
            }
        });
        if (result != 0)
            SVC_LOGW(kTag, "network event monitor start failed: %d", result);
    } else {
        SVC_LOGW(kTag, "network event monitor unavailable: %s", error.c_str());
    }

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

    auto pipeline = darkos::media::createMediaPipeline(config, error);
    darkos::storage::StorageConfig storageConfig;
    storageConfig.root = storageDirectory / "recordings";
    storageConfig.maxBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    storageConfig.minimumFreeBytes = 128ULL * 1024ULL * 1024ULL;
    auto storage = darkos::storage::StorageManager::create(storageConfig, error);
    auto recorder = storage != nullptr ? storage->createRecorder("continuous", "", error) : nullptr;
    darkos::protocols::rtsp::RtspServerOptions rtspOptions;
    rtspOptions.port = rtspPort;
    rtspOptions.mountPath = "live";
    auto rtsp = darkos::protocols::rtsp::RtspServer::create(*loop, rtspOptions, error);
    if (pipeline == nullptr || storage == nullptr || recorder == nullptr || rtsp == nullptr) {
        SVC_LOGE(kTag, "create capture/storage graph failed: %s", error.c_str());
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }
    if (alarmManager->addAction(darkos::alarm::createAlarmAction(
            [&](const darkos::alarm::AlarmEvent &event, std::string &actionError) {
                if (event.source == "system")
                    return 0;
                auto alarmRecorder = storage->createRecorder("alarm", event.id, actionError);
                if (alarmRecorder == nullptr)
                    return -EIO;
                darkos::media::SinkId alarmRecorderId = 0;
                const int result = pipeline->addVideoSink(
                    alarmRecorder, {32, darkos::media::BackpressurePolicy::DropOldest}, alarmRecorderId);
                if (result != 0) {
                    actionError = "attach alarm recorder failed";
                    return result;
                }
                if (loop->scheduleEvery(10'000'000'000ULL, 0,
                                        [pipelinePtr = pipeline.get(), alarmRecorderId] {
                                            pipelinePtr->removeVideoSink(alarmRecorderId);
                                        }) == 0) {
                    pipeline->removeVideoSink(alarmRecorderId);
                    actionError = "schedule alarm recording stop failed";
                    return -EAGAIN;
                }
                return 0;
            })) != 0) {
        SVC_LOGE(kTag, "register alarm recording action failed");
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }
    darkos::media::SinkId recorderId = 0;
    darkos::media::SinkId rtspId = 0;
    if (pipeline->addVideoSink(recorder, {32, darkos::media::BackpressurePolicy::DropOldest}, recorderId) != 0 ||
        pipeline->addVideoSink(rtsp, {32, darkos::media::BackpressurePolicy::DropOldest}, rtspId) != 0) {
        SVC_LOGE(kTag, "attach storage/RTSP sink failed");
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }

    if (pipeline->start() != 0) {
        SVC_LOGE(kTag, "start media pipeline failed");
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }

    loop->scheduleEvery(100'000'000ULL, 100'000'000ULL, [&] {
        darkos::media::MediaEvent event;
        while (pipeline->waitEvent(event, 0) == 0) {
            SVC_LOGI(kTag, "media event: component=%s state=%u code=%d message=%s", event.component.c_str(),
                     static_cast<unsigned>(event.state), event.code, event.message.c_str());
            if (event.state == darkos::media::PipelineState::Failed) {
                darkos::alarm::AlarmEvent alarm;
                alarm.source = "media";
                alarm.type = "pipeline_failed";
                alarm.message = event.message;
                alarm.severity = darkos::alarm::AlarmSeverity::Critical;
                alarm.timestampNs = event.timestampNs;
                alarmManager->publish(std::move(alarm));
                loop->post([&] { loop->quit(); });
            }
        }
    });

    alarmManager->publish({"", "system", "service_started", "capture service started",
                           darkos::alarm::AlarmSeverity::Info, 0});
    SVC_LOGI(kTag, "capture/storage/RTSP service ready: rtsp://0.0.0.0:%u/live", rtsp->listeningPort());
    loop->run();

    const int stopResult = pipeline->stop();
    if (networkManager != nullptr)
        networkManager->stop();
    loop->unwatchFd(signalFd);
    close(signalFd);
    const auto storageStats = storage->stats();
    const auto alarmStats = alarmManager->stats();
    const auto rtspStats = rtsp->stats();
    SVC_LOGI(kTag, "Storage/Alarm stopped: recordings=%llu bytes=%llu alarms=%llu suppressed=%llu",
             static_cast<unsigned long long>(storageStats.recordingCount),
             static_cast<unsigned long long>(storageStats.managedBytes),
             static_cast<unsigned long long>(alarmStats.accepted),
             static_cast<unsigned long long>(alarmStats.suppressed));
    SVC_LOGI(kTag, "RTSP stopped: connections=%llu requests=%llu video_packets=%llu rtp_packets=%llu",
             static_cast<unsigned long long>(rtspStats.acceptedConnections),
             static_cast<unsigned long long>(rtspStats.requests),
             static_cast<unsigned long long>(rtspStats.videoPackets),
             static_cast<unsigned long long>(rtspStats.rtpPackets));
    return stopResult == 0;
}

} // namespace

int main(int argc, char **argv) {
    // 1. 设置日志
    svc_log_set_default_level(SVC_LOG_VERBOSE);
    SVC_LOGI(kTag, "application started");

    // 2. 解析命令行参数
    Options options(defaultConfigDirectory());
    if (!parseOptions(argc, argv, options))
        return EXIT_FAILURE;

    darkos::BoardConfig board;
    darkos::AppConfig app;
    std::string error;

    if (!darkos::BoardConfig::load(options.boardConfig, board, error)) {
        SVC_LOGE(kTag, "board configuration error: %s", error.c_str());
        return EXIT_FAILURE;
    }
    if (!darkos::AppConfig::load(options.appConfig, app, error)) {
        SVC_LOGE(kTag, "application configuration error: %s", error.c_str());
        return EXIT_FAILURE;
    }
    if (!app.validate(board, error)) {
        SVC_LOGE(kTag, "configuration binding error: %s", error.c_str());
        return EXIT_FAILURE;
    }

    SVC_LOGI(kTag, "board=%s soc=%s serial_ports=%zu", board.boardId().c_str(), board.compatibleSoc().c_str(),
             board.serialPorts().size());

    for (const auto &entry : app.serialBindings()) {
        const darkos::SerialBinding &binding = entry.second;
        const darkos::BoardSerialPort *port = board.findSerialPort(binding.resource);
        SVC_LOGI(kTag, "binding %s -> %s (%s, %s, %u baud)", binding.service.c_str(), binding.resource.c_str(),
                 port->device.c_str(), darkos::serialElectricalName(port->electrical), binding.baud);
    }

    // 3. 只通过 SvcKit Network 门面读取系统网络状态。
    if (!probeNetworkState())
        return EXIT_FAILURE;

    // 4. 默认执行有限探针；--serve 启动常驻采集/录像服务并等待 SIGINT/SIGTERM。
    if (options.serve ? !runCaptureService(options.storageDirectory, options.rtspPort)
                      : !runMediaPipelineProbe(options.storageDirectory))
        return EXIT_FAILURE;

    SVC_LOGI(kTag, "configuration, Network, Media, Storage and Alarm completed cleanly");

    return 0;
}
