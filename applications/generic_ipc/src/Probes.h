#pragma once

#include <filesystem>

namespace generic_ipc {

bool probeNetworkState();
bool runMediaPipelineProbe(const std::filesystem::path &storageDirectory);

} // namespace generic_ipc
