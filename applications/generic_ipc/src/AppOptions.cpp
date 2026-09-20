#include "AppOptions.h"

#include <svc_log.h>

#include <cstdlib>
#include <system_error>
#include <unistd.h>

namespace generic_ipc {

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

} // namespace

AppOptions defaultAppOptions() {
    const std::filesystem::path configDirectory = defaultConfigDirectory();
    AppOptions options;
    options.boardConfig = (configDirectory / "board.json").string();
    options.appConfig = (configDirectory / "app.json").string();
    options.storageDirectory = configDirectory.parent_path() / "data";
    if (const char *value = std::getenv("DARKOS_RTSP_USERNAME"))
        options.rtspUsernameOverride = value;
    if (const char *value = std::getenv("DARKOS_RTSP_PASSWORD"))
        options.rtspPasswordOverride = value;
    if (const char *value = std::getenv("DARKOS_RTSP_MULTICAST_ADDRESS"); value && value[0] != '\0')
        options.rtspMulticastAddressOverride = value;
    if (const char *value = std::getenv("DARKOS_IVA_MODEL"); value && value[0] != '\0')
        options.ivaModelPath = value;
    return options;
}

bool parseAppOptions(int argc, char **argv, AppOptions &options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if ((argument == "--board-config" || argument == "--app-config") && index + 1 < argc) {
            std::string &destination = argument == "--board-config" ? options.boardConfig : options.appConfig;
            destination = argv[++index];
        } else if (argument == "--iva-model" && index + 1 < argc) {
            options.ivaModelPath = argv[++index];
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
            options.rtspPortOverride = static_cast<std::uint16_t>(port);
        } else {
            SVC_LOGE(kTag,
                     "usage: %s [--board-config path] [--app-config path] "
                     "[--storage-dir path] [--iva-model path] [--serve] "
                     "[--rtsp-port port]",
                     argv[0]);
            return false;
        }
    }
    return true;
}

} // namespace generic_ipc
