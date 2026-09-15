#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* O_CLOEXEC（严格 C 模式下需特性宏，见 serial_termios.c 同款） */
#endif

#include "gpio_sysfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int write_attribute(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t written;

    if (fd < 0)
        return -errno;
    written = write(fd, value, strlen(value));
    if (written < 0) {
        const int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }
    if (close(fd) != 0)
        return -errno;
    return written > 0 ? 0 : -EIO;
}

int linux_gpio_sysfs_prepare_output(int pin) {
    char gpio_path[128];
    char pin_text[16];
    int rc;

    if (pin < 0)
        return -EINVAL;

    snprintf(gpio_path, sizeof(gpio_path), "/sys/class/gpio/gpio%d", pin);
    if (access(gpio_path, F_OK) != 0) {
        snprintf(pin_text, sizeof(pin_text), "%d", pin);
        rc = write_attribute("/sys/class/gpio/export", pin_text);
        if (rc != 0 && rc != -EBUSY)
            return rc;
    }

    snprintf(gpio_path, sizeof(gpio_path),
             "/sys/class/gpio/gpio%d/direction", pin);
    return write_attribute(gpio_path, "out");
}

int linux_gpio_sysfs_write_value(int pin, int value) {
    char path[128];

    if (pin < 0)
        return -EINVAL;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    return write_attribute(path, value ? "1" : "0");
}
