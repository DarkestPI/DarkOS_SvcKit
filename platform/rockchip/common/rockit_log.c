/* Rockit 启动及运行时日志等级控制，协议来自 librockit 自带帮助信息。 */

#include "rockit_log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROCKIT_LOG_CONTROL_FILE "/tmp/rt_log_level"
#define ROCKIT_LOG_LEVEL_MIN 0
#define ROCKIT_LOG_LEVEL_MAX 6

static int rockit_log_level_valid(int level) {
    return level >= ROCKIT_LOG_LEVEL_MIN && level <= ROCKIT_LOG_LEVEL_MAX;
}

static int rockit_log_module_valid(const char *module) {
    static const char *const kModules[] = {
        "all", "cmpi", "mb",  "sys", "vdec", "venc", "rgn", "vpss", "vgs",
        "tde", "avs",  "wbc", "vo",  "vi",   "ai",   "ao",  "aenc", "adec",
    };
    size_t i;

    if (module == NULL || module[0] == '\0')
        return 0;
    for (i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++) {
        if (strcmp(module, kModules[i]) == 0)
            return 1;
    }
    return 0;
}

static int rockit_log_set_startup_level_internal(int level, int overwrite) {
    char value[2];

    if (!rockit_log_level_valid(level))
        return -EINVAL;
    value[0] = (char)('0' + level);
    value[1] = '\0';
    if (setenv("rt_log_level", value, overwrite) != 0)
        return -errno;
    return 0;
}

int rockit_log_set_startup_level(int level) {
    return rockit_log_set_startup_level_internal(level, 1);
}

int rockit_log_set_default_startup_level(int level) {
    return rockit_log_set_startup_level_internal(level, 0);
}

int rockit_log_set_module_level(const char *module, int level) {
    char command[32];
    size_t written = 0;
    ssize_t count;
    int command_length;
    int saved_errno;
    int fd;

    if (!rockit_log_module_valid(module) || !rockit_log_level_valid(level))
        return -EINVAL;

    command_length = snprintf(command, sizeof(command), "%s=%d\n", module, level);
    if (command_length < 0 || (size_t)command_length >= sizeof(command))
        return -EOVERFLOW;

    do {
        fd = open(ROCKIT_LOG_CONTROL_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -errno;

    while (written < (size_t)command_length) {
        count = write(fd, command + written, (size_t)command_length - written);
        if (count > 0) {
            written += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        saved_errno = count == 0 ? EIO : errno;
        close(fd);
        return -saved_errno;
    }

    if (close(fd) != 0)
        return -errno;
    return 0;
}
