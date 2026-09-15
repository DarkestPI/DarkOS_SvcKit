/*
 * SPDX-FileCopyrightText: 2026 DarkOS contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <svc_log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

#include <unistd.h>

namespace {

std::atomic<svc_log_level_t> g_defaultLevel{SVC_LOG_INFO};
std::mutex g_logMutex;
std::unordered_map<std::string, svc_log_level_t> g_tagLevels;
svc_log_vprintf_t g_output = nullptr;
const auto g_startTime = std::chrono::steady_clock::now();

svc_log_level_t normalizeLevel(svc_log_level_t level) noexcept {
    const auto value = std::clamp(static_cast<int>(level),
                                  static_cast<int>(SVC_LOG_NONE),
                                  static_cast<int>(SVC_LOG_VERBOSE));
    return static_cast<svc_log_level_t>(value);
}

char levelLetter(svc_log_level_t level) noexcept {
    switch (level) {
    case SVC_LOG_ERROR:
        return 'E';
    case SVC_LOG_WARN:
        return 'W';
    case SVC_LOG_INFO:
        return 'I';
    case SVC_LOG_DEBUG:
        return 'D';
    case SVC_LOG_VERBOSE:
        return 'V';
    default:
        return 'N';
    }
}

const char *levelColor(svc_log_level_t level) noexcept {
    switch (level) {
    case SVC_LOG_ERROR:
        return "\033[1;31m";
    case SVC_LOG_WARN:
        return "\033[1;33m";
    case SVC_LOG_INFO:
        return "\033[1;32m";
    case SVC_LOG_DEBUG:
        return "\033[1;36m";
    case SVC_LOG_VERBOSE:
        return "\033[0;90m";
    default:
        return "";
    }
}

void formatWallClock(char *buffer, std::size_t size) noexcept {
    if (size < 24)
        return;

    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&time, &localTime);
    if (std::strftime(buffer, size, "%Y-%m-%d %H:%M:%S.000", &localTime) == 0)
        return;

    buffer[20] = static_cast<char>('0' + milliseconds / 100);
    buffer[21] = static_cast<char>('0' + (milliseconds / 10) % 10);
    buffer[22] = static_cast<char>('0' + milliseconds % 10);
}

int stderrVprintf(const char *format, va_list args) {
    return std::vfprintf(stderr, format, args);
}

int outputPrintf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const auto output = g_output != nullptr ? g_output : stderrVprintf;
    const int result = output(format, args);
    va_end(args);
    return result;
}

} // namespace

extern "C" {

void svc_log_set_default_level(svc_log_level_t level) {
    g_defaultLevel.store(normalizeLevel(level), std::memory_order_relaxed);
}

svc_log_level_t svc_log_get_default_level(void) {
    return g_defaultLevel.load(std::memory_order_relaxed);
}

void svc_log_level_set(const char *tag, svc_log_level_t level) {
    if (tag == nullptr) {
        return;
    }

    level = normalizeLevel(level);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (std::string(tag) == "*") {
        g_defaultLevel.store(level, std::memory_order_relaxed);
        g_tagLevels.clear();
    } else {
        g_tagLevels[tag] = level;
    }
}

svc_log_level_t svc_log_level_get(const char *tag) {
    if (tag == nullptr) {
        return svc_log_get_default_level();
    }

    std::lock_guard<std::mutex> lock(g_logMutex);
    const auto entry = g_tagLevels.find(tag);
    return entry != g_tagLevels.end() ? entry->second
                                      : svc_log_get_default_level();
}

int svc_log_is_enabled(svc_log_level_t level, const char *tag) {
    return level != SVC_LOG_NONE && level <= svc_log_level_get(tag);
}

svc_log_vprintf_t svc_log_set_vprintf(svc_log_vprintf_t func) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    const auto oldOutput = g_output;
    g_output = func;
    return oldOutput;
}

uint64_t svc_log_timestamp(void) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_startTime)
            .count());
}

void svc_log_writev(svc_log_level_t level, const char *tag, const char *format,
                    va_list args) {
    if (format == nullptr || !svc_log_is_enabled(level, tag)) {
        return;
    }

    char message[1024];
    va_list copy;
    va_copy(copy, args);
    std::vsnprintf(message, sizeof(message), format, copy);
    va_end(copy);

    const char *safeTag = tag != nullptr ? tag : "SvcKit";
    std::lock_guard<std::mutex> lock(g_logMutex);
    char timestamp[32];
    formatWallClock(timestamp, sizeof(timestamp));

    const bool useColor = g_output == nullptr && ::isatty(STDERR_FILENO) != 0 &&
                          std::getenv("NO_COLOR") == nullptr;
    if (useColor) {
        outputPrintf("%s %s%c %s: %s\033[0m\n", timestamp, levelColor(level),
                     levelLetter(level), safeTag, message);
    } else {
        outputPrintf("%s %c %s: %s\n", timestamp, levelLetter(level), safeTag,
                     message);
    }
    if (g_output == nullptr) {
        std::fflush(stderr);
    }
}

void svc_log_write(svc_log_level_t level, const char *tag, const char *format,
                   ...) {
    va_list args;
    va_start(args, format);
    svc_log_writev(level, tag, format, args);
    va_end(args);
}

} // extern "C"
