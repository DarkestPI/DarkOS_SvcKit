#pragma once

namespace darkos {
class AppConfig;
}

namespace generic_ipc {

struct AppOptions;

bool runCaptureService(const AppOptions &options, const darkos::AppConfig &app);

} // namespace generic_ipc
