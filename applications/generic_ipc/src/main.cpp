#include "AppOptions.h"
#include "CaptureService.h"
#include "IvaExample.h"
#include "Probes.h"

#include <svc_board/AppConfig.h>
#include <svc_board/BoardConfig.h>
#include <svc_log.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace {

constexpr char kTag[] = "generic_ipc";

} // namespace

int main(int argc, char **argv) {
    // 1. 初始化应用日志，后续启动错误统一通过 SvcKit Log 输出。
    svc_log_set_default_level(SVC_LOG_VERBOSE);
    SVC_LOGI(kTag, "application started");

    // 2. 建立默认路径和环境变量覆盖，再解析命令行参数。
    common_ipc::AppOptions options = common_ipc::defaultAppOptions();
    if (!common_ipc::parseAppOptions(argc, argv, options))
        return EXIT_FAILURE;

    // 3. 加载板级资源和应用配置，并校验应用绑定的硬件资源。
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
    if (ivaModelPath && !common_ipc::runIvaExample(*ivaModelPath))
        return EXIT_FAILURE;

    // 4. 输出最终生效的板卡信息和串口绑定，便于定位部署配置问题。
    SVC_LOGI(kTag, "board=%s soc=%s serial_ports=%zu", board.boardId().c_str(), board.compatibleSoc().c_str(),
             board.serialPorts().size());
    for (const auto &entry : app.serialBindings()) {
        const darkos::SerialBinding &binding = entry.second;
        const darkos::BoardSerialPort *port = board.findSerialPort(binding.resource);
        SVC_LOGI(kTag, "binding %s -> %s (%s, %s, %u baud)", binding.service.c_str(), binding.resource.c_str(),
                 port->device.c_str(), darkos::serialElectricalName(port->electrical), binding.baud);
    }

    // 5. 启动媒体前读取一次网络状态；受限环境会在探针内部降级处理。
    if (!generic_ipc::probeNetworkState())
        return EXIT_FAILURE;

    // 6. --serve 进入常驻采集服务；默认模式执行一次自检后退出。
    const bool succeeded = options.serve
                               ? common_ipc::runCaptureService(options, app)
                               : generic_ipc::runMediaPipelineProbe(options.storageDirectory);
    if (!succeeded)
        return EXIT_FAILURE;

    // 7. 所有组件已按正常生命周期停止或完成探针。
    SVC_LOGI(kTag, "configuration, Network, Media, Storage and Alarm completed cleanly");
    return EXIT_SUCCESS;
}
