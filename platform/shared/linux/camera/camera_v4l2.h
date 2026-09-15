#ifndef DARKOS_SHARED_LINUX_CAMERA_V4L2_H
#define DARKOS_SHARED_LINUX_CAMERA_V4L2_H

/* Linux 通用 UVC/V4L2 Camera 后端；由具体 HAL 插件决定是否使用。 */

#include <hardware/hardware.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 V4L2 设备节点（如 /dev/video0），返回 camera_device_t；失败返回负 errno */
int linux_v4l2_camera_open(const hw_module_t *module, const char *device_path,
                           hw_device_t **device);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_SHARED_LINUX_CAMERA_V4L2_H */
