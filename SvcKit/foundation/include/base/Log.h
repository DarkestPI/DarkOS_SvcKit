#ifndef DARKOS_BASE_LOG_H
#define DARKOS_BASE_LOG_H

#include <cstdint>

namespace darkos {

/** 日志级别。数值越大，严重程度越高。 */
enum class LogLevel : std::uint8_t {
  Debug = 0,
  Info,
  Warning,
  Error,
  Off,
};

/** 设置进程级最小输出级别，默认是 LogLevel::Info。 */
void setLogLevel(LogLevel level) noexcept;

/** 获取当前进程级最小输出级别。 */
LogLevel getLogLevel() noexcept;

/** 判断指定级别当前是否会被输出。 */
bool isLogEnabled(LogLevel level) noexcept;

/**
 * 输出一条 printf 风格日志。
 *
 * @param level 日志级别
 * @param tag   模块名；传入 nullptr 时显示为 DarkOS
 * @param format printf 风格格式串
 */
void logPrint(LogLevel level, const char *tag, const char *format, ...) noexcept
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

} // namespace darkos

// format 是可变参数的一部分，因此 LOGI(tag, "message") 不依赖非标准的
// ##__VA_ARGS__。
#define LOGD(tag, ...)                                                         \
  ::darkos::logPrint(::darkos::LogLevel::Debug, (tag), __VA_ARGS__)
#define LOGI(tag, ...)                                                         \
  ::darkos::logPrint(::darkos::LogLevel::Info, (tag), __VA_ARGS__)
#define LOGW(tag, ...)                                                         \
  ::darkos::logPrint(::darkos::LogLevel::Warning, (tag), __VA_ARGS__)
#define LOGE(tag, ...)                                                         \
  ::darkos::logPrint(::darkos::LogLevel::Error, (tag), __VA_ARGS__)

#endif // DARKOS_BASE_LOG_H
