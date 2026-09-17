#pragma once

#include <chrono>
#include <cstdint>

namespace darkos {

inline std::uint64_t monoNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

} // namespace darkos
