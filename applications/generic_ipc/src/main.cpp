#include <hardware/hardware.h>
#include <media_pipeline.h>
#include <serial/ISerial.h>
#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>
#include <svc_log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

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
        : boardConfig((configDirectory / "board.json").string()), appConfig((configDirectory / "app.json").string()) {}

    std::string boardConfig;
    std::string appConfig;
};

bool parseOptions(int argc, char **argv, Options &options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--board-config" || argument == "--app-config") && index + 1 < argc) {
            std::string &destination = argument == "--board-config" ? options.boardConfig : options.appConfig;
            destination = argv[++index];
        } else {
            SVC_LOGE(kTag, "usage: %s [--board-config path] [--app-config path]", argv[0]);
            return false;
        }
    }
    return true;
}

bool loadRequiredHalModules() {
    // Camera/Codec/Audio 由 SvcKit Media 内部加载；Application 只直接验证
    // 尚未服务化的 Serial，避免越过媒体门面持有 HAL 对象。
    constexpr std::array<const char *, 1> requiredModules = {
        SERIAL_HARDWARE_MODULE_ID,
    };

    for (const char *id : requiredModules) {
        const hw_module_t *module = nullptr;
        const int result = hw_get_module(id, &module);
        if (result != 0) {
            const int errorNumber = result < 0 ? -result : result;
            SVC_LOGE(kTag, "HAL module load failed: id=%s error=%s (%d)", id, std::strerror(errorNumber), result);
            return false;
        }

        SVC_LOGI(
            kTag, "HAL module loaded: id=%s name=\"%s\" module_api=%u.%u hal_api=%u.%u", module->id,
            module->name != nullptr ? module->name : "unknown", static_cast<unsigned>(module->module_api_version >> 8),
            static_cast<unsigned>(module->module_api_version & 0xff),
            static_cast<unsigned>(module->hal_api_version >> 8), static_cast<unsigned>(module->hal_api_version & 0xff));
    }
    return true;
}

bool runMediaPipelineProbe() {
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

    // 添加音视频探针 Sink；队列满时丢弃最旧数据，以免阻塞媒体管线。
    auto videoSink = darkos::media::createVideoProbeSink();
    auto audioSink = darkos::media::createAudioProbeSink();
    darkos::media::SinkId videoSinkId = 0;
    darkos::media::SinkId audioSinkId = 0;
    const darkos::media::MediaQueueConfig sinkQueue{8, darkos::media::BackpressurePolicy::DropOldest};
    if (pipeline->addVideoSink(videoSink, sinkQueue, videoSinkId) != 0 ||
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

    // 销毁管线
    if (!receivedEnoughPackets) {
        SVC_LOGE(kTag, "media pipeline timed out waiting for audio/video data");
        return false;
    }

    const darkos::media::VideoProbeStats videoStats = videoSink->snapshot();
    const darkos::media::AudioProbeStats audioStats = audioSink->snapshot();
    SVC_LOGI(kTag,
             "SvcKit Media A/V pipeline OK: video_packets=%zu video_bytes=%zu keyframes=%zu "
             "audio_packets=%zu audio_bytes=%zu",
             static_cast<std::size_t>(videoStats.packetCount), static_cast<std::size_t>(videoStats.byteCount),
             static_cast<std::size_t>(videoStats.keyframeCount), static_cast<std::size_t>(audioStats.packetCount),
             static_cast<std::size_t>(audioStats.byteCount));
    return true;
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

    // 3. 加载尚未服务化的 HAL 模块
    if (!loadRequiredHalModules())
        return EXIT_FAILURE;

    // 4. 只通过 SvcKit Media 门面验证 Camera→H.264 + 麦克风→G.711A 管线。
    if (!runMediaPipelineProbe())
        return EXIT_FAILURE;

    SVC_LOGI(kTag, "configuration, HAL and SvcKit Media validated; service wiring is ready");

    printf("\n");
    SVC_LOGE(kTag, "this is an SVC_LOGE log for testing");
    SVC_LOGW(kTag, "this is an SVC_LOGW log for testing");
    SVC_LOGI(kTag, "this is an SVC_LOGI log for testing");
    SVC_LOGD(kTag, "this is an SVC_LOGD log for testing");
    SVC_LOGV(kTag, "this is an SVC_LOGV log for testing");

    return 0;
}
