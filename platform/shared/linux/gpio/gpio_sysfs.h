#ifndef DARKOS_SHARED_LINUX_GPIO_SYSFS_H
#define DARKOS_SHARED_LINUX_GPIO_SYSFS_H

#ifdef __cplusplus
extern "C" {
#endif

/* 兼容旧版 vendor kernel 的 /sys/class/gpio 接口。返回 0 或负 errno。 */
int linux_gpio_sysfs_prepare_output(int pin);
int linux_gpio_sysfs_write_value(int pin, int value);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_SHARED_LINUX_GPIO_SYSFS_H */
