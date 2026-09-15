#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>
#include <svc_log.h>

#include <audio/IAudio.h>
#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>
#include <media/ICodec.h>
#include <serial/ISerial.h>

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
        : boardConfig((configDirectory / "board.json").string()),
          appConfig((configDirectory / "app.json").string()) {}

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
    constexpr std::array<const char *, 4> requiredModules = {
        CAMERA_HARDWARE_MODULE_ID,
        MEDIA_CODEC_HARDWARE_MODULE_ID,
        AUDIO_HARDWARE_MODULE_ID,
        SERIAL_HARDWARE_MODULE_ID,
    };

    for (const char *id : requiredModules) {
        const hw_module_t *module = nullptr;
        const int result = hw_get_module(id, &module);
        if (result != 0) {
            const int errorNumber = result < 0 ? -result : result;
            SVC_LOGE(kTag, "HAL module load failed: id=%s error=%s (%d)", id,
                     std::strerror(errorNumber), result);
            return false;
        }

        SVC_LOGI(kTag,
                 "HAL module loaded: id=%s name=\"%s\" module_api=%u.%u hal_api=%u.%u",
                 module->id, module->name != nullptr ? module->name : "unknown",
                 static_cast<unsigned>(module->module_api_version >> 8),
                 static_cast<unsigned>(module->module_api_version & 0xff),
                 static_cast<unsigned>(module->hal_api_version >> 8),
                 static_cast<unsigned>(module->hal_api_version & 0xff));
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {

    svc_log_set_default_level(SVC_LOG_VERBOSE);
    SVC_LOGI(kTag, "application started");

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

    if (!loadRequiredHalModules())
        return EXIT_FAILURE;

    SVC_LOGI(kTag, "configuration and HAL validated; service wiring is ready");

    printf("\n");
    SVC_LOGE(kTag, "this is an SVC_LOGE log for testing");
    SVC_LOGW(kTag, "this is an SVC_LOGW log for testing");
    SVC_LOGI(kTag, "this is an SVC_LOGI log for testing");
    SVC_LOGD(kTag, "this is an SVC_LOGD log for testing");
    SVC_LOGV(kTag, "this is an SVC_LOGV log for testing");

    return 0;
}
