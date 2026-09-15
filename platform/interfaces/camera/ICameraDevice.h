#ifndef DARKOS_HARDWARE_CAMERA_ICAMERADEVICE_H
#define DARKOS_HARDWARE_CAMERA_ICAMERADEVICE_H

#include <camera/types.h>
#include <hardware/hardware.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * camera 设备接口（对应 Android 的 ICameraDevice）
 *
 * 把硬件能力抽象为"格式协商 + 流控制 + 帧获取 + 控制项"四类操作。
 * 厂商/SoC 实现负责把本接口映射到 V4L2、Rockit、RKAIQ 等具体后端。
 * ------------------------------------------------------------------------- */

#define CAMERA_HARDWARE_MODULE_ID "camera"
#define CAMERA_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct camera_device camera_device_t;

typedef struct camera_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(camera_device_t *dev, camera_caps_t *caps);

    /* 格式协商 */
    int (*set_format)(camera_device_t *dev, const camera_format_t *fmt);
    int (*get_format)(camera_device_t *dev, camera_format_t *fmt);

    /* 流控制 */
    int (*start)(camera_device_t *dev);
    int (*stop)(camera_device_t *dev);

    /* 帧获取：回调模型（推荐，适合 IPC 零拷贝）；capture 为阻塞模型（调试用） */
    int (*set_frame_callback)(camera_device_t *dev, camera_frame_cb cb, void *ctx);
    int (*capture)(camera_device_t *dev, camera_frame_t *frame, int timeout_ms);

    /* 控制项（亮度/曝光/白平衡等） */
    int (*set_control)(camera_device_t *dev, uint32_t id, int32_t value);
    int (*get_control)(camera_device_t *dev, uint32_t id, int32_t *value);

    /* 本地预览：把相机画面送显示输出（VO 视频层）。width/height = panel */
    int (*preview_start)(camera_device_t *dev, uint32_t width, uint32_t height);
    int (*preview_stop)(camera_device_t *dev);

} camera_device_ops_t;

/* 具体设备：基类 + 操作表 + 厂商私有上下文 */
struct camera_device {
    hw_device_t common;
    const camera_device_ops_t *ops;
    void *priv;
};

/* 按实例 id 打开 camera 设备：多路摄像头场景使用。
 * id 约定："camera"/"camera0" → 实例 0，"cameraN" → 实例 N。
 * 厂商实现把实例号映射到硬件通道（rockchip：VI dev=pipe=N、chn=0，
 * 可用环境变量 DARKOS_CAMERA{N}_VI_DEV/PIPE/CHN 覆盖）。 
 */
static inline int camera_open_by_id(const hw_module_t *module, const char *id,
                                    camera_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, id, &hwdev);
    if (rc != 0)
        return rc;
    *device = (camera_device_t *)hwdev;
    return 0;
}

/* 便捷：从 module 打开 camera 设备（等价 camera_open_by_id(..., "camera", ...)） */
static inline int camera_open(const hw_module_t *module, camera_device_t **device) {
    return camera_open_by_id(module, CAMERA_HARDWARE_MODULE_ID, device);
}

/* 便捷：关闭设备 */
static inline int camera_close(camera_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_CAMERA_ICAMERADEVICE_H */
