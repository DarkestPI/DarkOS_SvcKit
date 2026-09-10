#ifndef DARKOS_HARDWARE_SERIAL_ISERIAL_H
#define DARKOS_HARDWARE_SERIAL_ISERIAL_H

#include <hardware/hardware.h>

#include <serial/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 串口设备接口
 *
 * 无状态透传通道：open 里完成 termios 配置（v1 固定 8N1、无流控），
 * 透出 fd 给上层挂 EventLoop（watchFd EPOLLIN），读写由上层直接走
 * read/write 系统调用——HAL 不做协议解析，也不自建线程。
 *
 * fd 所有权归设备：close 时收回（关闭 fd）。设备路径/波特率的 env 覆盖
 * 归上层适配器，HAL 只认 config。
 * ------------------------------------------------------------------------- */

#define SERIAL_HARDWARE_MODULE_ID "serial"
#define SERIAL_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct serial_device serial_device_t;

typedef struct serial_device_ops {
    /* 打开并按 config 配置串口（termios 8N1）。返回 0 成功；负 errno 失败。 */
    int (*open)(serial_device_t *dev, const serial_config_t *config);

    /* 关闭串口（收回 fd）。幂等。 */
    int (*close)(serial_device_t *dev);

    /* 取串口 fd（open 成功后非负；未打开返回 -1）。fd 所有权归设备。 */
    int (*fd)(serial_device_t *dev);
} serial_device_ops_t;

struct serial_device {
    hw_device_t common;
    const serial_device_ops_t *ops;
    void *priv;
};

static inline int serial_open(const hw_module_t *module, serial_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, SERIAL_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    *device = (serial_device_t *)hwdev;
    return 0;
}

static inline int serial_close(serial_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_SERIAL_ISERIAL_H */
