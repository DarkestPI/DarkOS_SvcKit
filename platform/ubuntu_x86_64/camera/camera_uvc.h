#ifndef DARKOS_HOST_X86_CAMERA_UVC_H
#define DARKOS_HOST_X86_CAMERA_UVC_H

/* host_x86 内部：UVC（V4L2）相机实现的打开入口，由 camera_host_x86.c 分流调用 */

#include <hardware/hardware.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 V4L2 设备节点（如 /dev/video0），返回 camera_device_t；失败返回负 errno */
int uvc_camera_open(const hw_module_t *module, const char *device_path, hw_device_t **device);

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HOST_X86_CAMERA_UVC_H */
