#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>
#include <svc_log.h>

#include <cstdlib>
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

} // namespace

int main(int argc, char **argv) {
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

    SVC_LOGI(kTag, "configuration validated; service wiring is ready");
    return 0;
}
