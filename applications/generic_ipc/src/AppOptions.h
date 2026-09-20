#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace generic_ipc {

struct AppOptions {
    std::string boardConfig;
    std::string appConfig;
    std::filesystem::path storageDirectory;
    std::optional<std::string> ivaModelPath;
    bool serve{false};
    std::optional<std::uint16_t> rtspPortOverride;
    std::optional<std::string> rtspUsernameOverride;
    std::optional<std::string> rtspPasswordOverride;
    std::optional<std::string> rtspMulticastAddressOverride;
};

AppOptions defaultAppOptions();
bool parseAppOptions(int argc, char **argv, AppOptions &options);

} // namespace generic_ipc
