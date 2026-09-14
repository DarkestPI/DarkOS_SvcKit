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
#include <mutex>
#include <string>
#include <unordered_map>

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
    outputPrintf("%c (%llu) %s: %s\n", levelLetter(level),
                 static_cast<unsigned long long>(svc_log_timestamp()), safeTag,
                 message);
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
