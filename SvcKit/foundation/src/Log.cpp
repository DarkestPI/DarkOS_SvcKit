#include "base/Log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace darkos {
namespace {

std::atomic<LogLevel> g_logLevel{LogLevel::Info};
std::mutex g_logMutex;

const char *levelName(LogLevel level) noexcept {
  switch (level) {
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Info:
    return "INFO ";
  case LogLevel::Warning:
    return "WARN ";
  case LogLevel::Error:
    return "ERROR";
  case LogLevel::Off:
    return "OFF  ";
  }
  return "UNKWN";
}

} // namespace

void setLogLevel(LogLevel level) noexcept {
  g_logLevel.store(level, std::memory_order_relaxed);
}

LogLevel getLogLevel() noexcept {
  return g_logLevel.load(std::memory_order_relaxed);
}

bool isLogEnabled(LogLevel level) noexcept {
  const auto threshold = getLogLevel();
  return threshold != LogLevel::Off &&
         static_cast<unsigned>(level) >= static_cast<unsigned>(threshold);
}

void logPrint(LogLevel level, const char *tag, const char *format,
              ...) noexcept {
  if (!isLogEnabled(level) || format == nullptr) {
    return;
  }

  char message[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);

  const auto now = std::chrono::system_clock::now();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count() %
      1000;
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  std::tm localTime{};
  localtime_r(&seconds, &localTime);

  char timestamp[24];
  std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &localTime);

  const char *safeTag = tag != nullptr ? tag : "DarkOS";
  std::lock_guard<std::mutex> lock(g_logMutex);
  std::fprintf(stderr, "[%s.%03lld][%s][%s] %s\n", timestamp,
               static_cast<long long>(milliseconds), levelName(level), safeTag,
               message);
  std::fflush(stderr);
}

} // namespace darkos
