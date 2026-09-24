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
constexpr char kTag[] = "vs816_ipc";
}

int main(int argc, char **argv) {
    svc_log_set_default_level(SVC_LOG_VERBOSE);
    SVC_LOGI(kTag, "application started");

    common_ipc::AppOptions options = common_ipc::defaultAppOptions();
    if (!common_ipc::parseAppOptions(argc, argv, options))
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
            : (app.iva().enabled ? std::optional<std::string>(app.iva().modelPath)
                                 : std::nullopt);
    if (ivaModelPath && !common_ipc::runIvaExample(*ivaModelPath))
        return EXIT_FAILURE;

    SVC_LOGI(kTag, "board=%s soc=%s", board.boardId().c_str(),
             board.compatibleSoc().c_str());
    if (!options.serve) {
        SVC_LOGI(kTag, "configuration validated; use --serve to start camera/RTSP service");
        return EXIT_SUCCESS;
    }
    return common_ipc::runCaptureService(options, app) ? EXIT_SUCCESS : EXIT_FAILURE;
}
