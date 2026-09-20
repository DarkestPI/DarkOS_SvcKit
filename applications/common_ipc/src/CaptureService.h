#pragma once

namespace darkos {
class AppConfig;
}

namespace common_ipc {

struct AppOptions;

bool runCaptureService(const AppOptions &options, const darkos::AppConfig &app);

} // namespace common_ipc
