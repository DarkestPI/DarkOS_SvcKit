/*
 * SPDX-FileCopyrightText: 2026 DarkOS contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SvcKit 通用日志接口，可用于 Linux 宿主机与交叉编译环境。
 */

#ifndef SVC_LOG_H
#define SVC_LOG_H

#include <stdarg.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 日志级别。数值越大，输出越详细。 */
typedef enum {
    SVC_LOG_NONE = 0,
    SVC_LOG_ERROR = 1,
    SVC_LOG_WARN = 2,
    SVC_LOG_INFO = 3,
    SVC_LOG_DEBUG = 4,
    SVC_LOG_VERBOSE = 5,
    SVC_LOG_MAX = 6,
} svc_log_level_t;

/** 与 vprintf 相同签名的日志输出回调。 */
typedef int (*svc_log_vprintf_t)(const char *format, va_list args);

/** 设置默认日志级别。默认是 SVC_LOG_INFO。 */
void svc_log_set_default_level(svc_log_level_t level);

/** 获取默认日志级别。 */
svc_log_level_t svc_log_get_default_level(void);

/**
 * 设置指定 tag 的日志级别。
 * tag 为 "*" 时修改默认级别并清除所有 tag 的单独配置；tag 为 NULL 时不操作。
 */
void svc_log_level_set(const char *tag, svc_log_level_t level);

/** 获取指定 tag 当前生效的日志级别；tag 为 NULL 时返回默认级别。 */
svc_log_level_t svc_log_level_get(const char *tag);

/** 判断指定 tag 和级别当前是否允许输出。 */
int svc_log_is_enabled(svc_log_level_t level, const char *tag);

/** 设置日志输出回调；func 为 NULL 时恢复输出到 stderr，并返回旧回调。 */
svc_log_vprintf_t svc_log_set_vprintf(svc_log_vprintf_t func);

/** 返回进程启动后的毫秒数。 */
uint64_t svc_log_timestamp(void);

/** 输出 printf 风格日志。通常应使用 SVC_LOGx 宏。 */
void svc_log_write(svc_log_level_t level, const char *tag, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/** svc_log_write 的 va_list 版本。 */
void svc_log_writev(svc_log_level_t level, const char *tag, const char *format,
                    va_list args);

#ifdef __cplusplus
}
#endif

/*
 * 单个源文件可在包含本头文件前定义 SVC_LOG_LOCAL_LEVEL，裁剪更详细的日志。
 * 例如定义为 SVC_LOG_WARN 后，Info/Debug/Verbose 日志不会进入目标文件。
 */
#ifndef SVC_LOG_LOCAL_LEVEL
#define SVC_LOG_LOCAL_LEVEL SVC_LOG_VERBOSE
#endif

#define SVC_LOG_ENABLED(level)                                                 \
    ((level) != SVC_LOG_NONE && (level) <= SVC_LOG_LOCAL_LEVEL)

/* format 包含在 __VA_ARGS__ 中，因此只有格式串、没有附加参数时同样符合 C17。 */
#define SVC_LOG_LEVEL(level, tag, ...)                                         \
    do {                                                                       \
        if (SVC_LOG_ENABLED(level) && svc_log_is_enabled((level), (tag))) {    \
            svc_log_write((level), (tag), __VA_ARGS__);                       \
        }                                                                      \
    } while (0)

#define SVC_LOGE(tag, ...) SVC_LOG_LEVEL(SVC_LOG_ERROR, (tag), __VA_ARGS__)
#define SVC_LOGW(tag, ...) SVC_LOG_LEVEL(SVC_LOG_WARN, (tag), __VA_ARGS__)
#define SVC_LOGI(tag, ...) SVC_LOG_LEVEL(SVC_LOG_INFO, (tag), __VA_ARGS__)
#define SVC_LOGD(tag, ...) SVC_LOG_LEVEL(SVC_LOG_DEBUG, (tag), __VA_ARGS__)
#define SVC_LOGV(tag, ...) SVC_LOG_LEVEL(SVC_LOG_VERBOSE, (tag), __VA_ARGS__)

#endif /* SVC_LOG_H */
