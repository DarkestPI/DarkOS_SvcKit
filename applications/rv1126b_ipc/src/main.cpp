#include "AppOptions.h"
#include "CaptureService.h"
#include "IvaExample.h"

#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>
#include <svc_log.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace {

constexpr char kTag[] = "rv1126b_ipc";

} // namespace

int main(int argc, char **argv) {
    svc_log_set_default_level(SVC_LOG_VERBOSE);
    SVC_LOGI(kTag, "application started");

    generic_ipc::AppOptions options = generic_ipc::defaultAppOptions();
    if (!generic_ipc::parseAppOptions(argc, argv, options))
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

    const std::optional<std::string> ivaModelPath =
        options.ivaModelPath.has_value()
            ? options.ivaModelPath
            : (app.iva().enabled
                   ? std::optional<std::string>(app.iva().modelPath)
                   : std::nullopt);
    if (ivaModelPath && !generic_ipc::runIvaExample(*ivaModelPath))
        return EXIT_FAILURE;

    SVC_LOGI(kTag, "board=%s soc=%s serial_ports=%zu", board.boardId().c_str(), board.compatibleSoc().c_str(),
             board.serialPorts().size());
    for (const auto &entry : app.serialBindings()) {
        const darkos::SerialBinding &binding = entry.second;
        const darkos::BoardSerialPort *port = board.findSerialPort(binding.resource);
        SVC_LOGI(kTag, "binding %s -> %s (%s, %s, %u baud)", binding.service.c_str(), binding.resource.c_str(),
                 port->device.c_str(), darkos::serialElectricalName(port->electrical), binding.baud);
    }

    if (!options.serve) {
        SVC_LOGI(kTag, "configuration validated; use --serve to start camera/RTSP service");
        return EXIT_SUCCESS;
    }

    if (!generic_ipc::runCaptureService(options, app))
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
