#include "CaptureService.h"

#include "AppOptions.h"
#include "MediaSettings.h"

#ifdef DARKOS_CAMERA_ENCODED_MEDIA
#include "CameraEncodedVideo.h"
#endif

#include <RtspServer.h>
#include <alarm_manager.h>
#include <base/EventLoop.h>
#include <media_pipeline.h>
#include <network_manager.h>
#include <storage_manager.h>
#include <svc_board/AppConfig.h>
#include <svc_log.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace common_ipc {

namespace {

constexpr char kTag[] = "ipc_service";

const char *networkEventTypeName(darkos::network::NetworkEventType type) {
    switch (type) {
    case darkos::network::NetworkEventType::LinkChanged:
        return "LinkChanged";
    case darkos::network::NetworkEventType::InterfaceAdded:
        return "InterfaceAdded";
    case darkos::network::NetworkEventType::InterfaceRemoved:
        return "InterfaceRemoved";
    case darkos::network::NetworkEventType::AddressAdded:
        return "AddressAdded";
    case darkos::network::NetworkEventType::AddressRemoved:
        return "AddressRemoved";
    case darkos::network::NetworkEventType::Error:
        return "Error";
    }
    return "Unknown";
}

} // namespace

bool runCaptureService(const AppOptions &options, const darkos::AppConfig &app) {
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
    darkos::alarm::AlarmConfig alarmConfig{options.storageDirectory / "alarms.journal", 1000};
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
            SVC_LOGI(kTag,
                     "network event: type=%s interface=%s address=%s admin_up=%d link_running=%d code=%d",
                     networkEventTypeName(event.type), event.interfaceName.c_str(), event.address.c_str(), event.up,
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

    const darkos::media::MediaPipelineConfig config = defaultMediaPipelineConfig();
#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    auto encodedVideo = createCameraEncodedVideo(config.video, error);
    auto pipeline = darkos::media::createPacketMediaPipeline(config.audio, error);
#else
    auto pipeline = darkos::media::createMediaPipeline(config, error);
#endif
    darkos::storage::StorageConfig storageConfig;
    storageConfig.root = options.storageDirectory / "recordings";
    storageConfig.maxBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    storageConfig.minimumFreeBytes = 128ULL * 1024ULL * 1024ULL;
    auto storage = darkos::storage::StorageManager::create(storageConfig, error);
    auto recorder = storage != nullptr ? storage->createRecorder("continuous", "", error) : nullptr;

    std::shared_ptr<darkos::protocols::rtsp::RtspServer> rtsp;
    const auto &configuredRtsp = app.rtsp();
    if (configuredRtsp.enabled) {
        darkos::protocols::rtsp::RtspServerOptions rtspOptions;
        rtspOptions.bindAddress = configuredRtsp.bindAddress;
        rtspOptions.port = options.rtspPortOverride.value_or(configuredRtsp.port);
        rtspOptions.mountPath = configuredRtsp.mountPath;
        rtspOptions.maximumRtpPayloadBytes = configuredRtsp.maximumRtpPayloadBytes;
        rtspOptions.maximumClientBacklogBytes = configuredRtsp.maximumClientBacklogBytes;
        rtspOptions.sessionTimeoutSeconds = configuredRtsp.sessionTimeoutSeconds;
        rtspOptions.rtcpReportIntervalMs = configuredRtsp.rtcpReportIntervalMs;
        rtspOptions.username = options.rtspUsernameOverride.value_or(configuredRtsp.authentication.username);
        if (!rtspOptions.username.empty()) {
            if (options.rtspPasswordOverride) {
                rtspOptions.password = *options.rtspPasswordOverride;
            } else if (!configuredRtsp.authentication.password.empty()) {
                rtspOptions.password = configuredRtsp.authentication.password;
            } else {
                const char *password = std::getenv(configuredRtsp.authentication.passwordEnvironment.c_str());
                if (password == nullptr || password[0] == '\0') {
                    SVC_LOGE(kTag, "RTSP authentication password environment '%s' is not set",
                             configuredRtsp.authentication.passwordEnvironment.c_str());
                    loop->unwatchFd(signalFd);
                    close(signalFd);
                    return false;
                }
                rtspOptions.password = password;
            }
        }
        rtspOptions.audioCodec = config.audio.encoder.codec;
        rtspOptions.audioSampleRate = config.audio.capture.sampleRate;
        rtspOptions.audioChannelCount = config.audio.capture.channelCount;
        rtspOptions.enableMulticast = configuredRtsp.multicast.enabled ||
                                      options.rtspMulticastAddressOverride.has_value();
        rtspOptions.multicastAddress =
            options.rtspMulticastAddressOverride.value_or(configuredRtsp.multicast.address);
        rtspOptions.multicastVideoPort = configuredRtsp.multicast.videoPort;
        rtspOptions.multicastAudioPort = configuredRtsp.multicast.audioPort;
        rtspOptions.multicastTtl = configuredRtsp.multicast.ttl;
        rtsp = darkos::protocols::rtsp::RtspServer::create(*loop, rtspOptions, error);
    }
#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    if (encodedVideo == nullptr || pipeline == nullptr || storage == nullptr ||
        recorder == nullptr ||
#else
    if (pipeline == nullptr || storage == nullptr || recorder == nullptr ||
#endif
        (configuredRtsp.enabled && rtsp == nullptr)) {
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
    if (pipeline->addVideoSink(recorder, {32, darkos::media::BackpressurePolicy::DropOldest}, recorderId) != 0) {
        SVC_LOGE(kTag, "attach storage sink failed");
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }
    if (rtsp != nullptr) {
        darkos::media::SinkId rtspVideoId = 0;
        darkos::media::SinkId rtspAudioId = 0;
        if (pipeline->addVideoSink(rtsp, {32, darkos::media::BackpressurePolicy::DropOldest}, rtspVideoId) != 0 ||
            pipeline->addAudioSink(rtsp, {64, darkos::media::BackpressurePolicy::DropOldest}, rtspAudioId) != 0) {
            SVC_LOGE(kTag, "attach RTSP audio/video sink failed");
            loop->unwatchFd(signalFd);
            close(signalFd);
            return false;
        }
    }

    if (pipeline->start() != 0) {
        SVC_LOGE(kTag, "start media pipeline failed");
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }

#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    if (encodedVideo->start(
            [pipelinePtr = pipeline.get()](darkos::media::VideoPacketPtr packet) {
                const int rc = pipelinePtr->pushVideoPacket(std::move(packet));
                if (rc != 0 && rc != -EPIPE)
                    SVC_LOGW(kTag, "push encoded video packet failed: %d", rc);
            },
            [&loop](int code, const std::string &message) {
                SVC_LOGE(kTag, "encoded video error: %d %s", code, message.c_str());
                loop->post([&loop] { loop->quit(); });
            },
            error) != 0) {
        SVC_LOGE(kTag, "start encoded video output failed: %s", error.c_str());
        pipeline->stop();
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }
    if (app.display().enabled && encodedVideo->startPreview(error) != 0) {
        SVC_LOGE(kTag, "start local display preview failed: %s", error.c_str());
        encodedVideo->stop();
        pipeline->stop();
        loop->unwatchFd(signalFd);
        close(signalFd);
        return false;
    }
#endif

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
    if (rtsp != nullptr)
        SVC_LOGI(kTag, "capture/storage/RTSP service ready: rtsp://%s:%u/%s",
                 configuredRtsp.bindAddress.c_str(), rtsp->listeningPort(), configuredRtsp.mountPath.c_str());
    else
        SVC_LOGI(kTag, "capture/storage service ready (RTSP disabled by app.json)");
    loop->run();

#ifdef DARKOS_CAMERA_ENCODED_MEDIA
    const int encodedStopResult = encodedVideo->stop();
#else
    const int encodedStopResult = 0;
#endif
    const int pipelineStopResult = pipeline->stop();
    const int stopResult = encodedStopResult != 0 ? encodedStopResult : pipelineStopResult;
    if (networkManager != nullptr)
        networkManager->stop();
    loop->unwatchFd(signalFd);
    close(signalFd);
    const auto storageStats = storage->stats();
    const auto alarmStats = alarmManager->stats();
    SVC_LOGI(kTag, "Storage/Alarm stopped: recordings=%llu bytes=%llu alarms=%llu suppressed=%llu",
             static_cast<unsigned long long>(storageStats.recordingCount),
             static_cast<unsigned long long>(storageStats.managedBytes),
             static_cast<unsigned long long>(alarmStats.accepted),
             static_cast<unsigned long long>(alarmStats.suppressed));
    if (rtsp != nullptr) {
        const auto rtspStats = rtsp->stats();
        SVC_LOGI(kTag,
                 "RTSP stopped: connections=%llu requests=%llu video=%llu audio=%llu rtp=%llu rtcp=%llu "
                 "auth_failures=%llu expired=%llu",
                 static_cast<unsigned long long>(rtspStats.acceptedConnections),
                 static_cast<unsigned long long>(rtspStats.requests),
                 static_cast<unsigned long long>(rtspStats.videoPackets),
                 static_cast<unsigned long long>(rtspStats.audioPackets),
                 static_cast<unsigned long long>(rtspStats.rtpPackets),
                 static_cast<unsigned long long>(rtspStats.rtcpReports),
                 static_cast<unsigned long long>(rtspStats.authenticationFailures),
                 static_cast<unsigned long long>(rtspStats.expiredSessions));
    }
    return stopResult == 0;
}

} // namespace common_ipc
