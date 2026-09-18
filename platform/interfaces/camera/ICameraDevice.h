#ifndef DARKOS_HARDWARE_CAMERA_ICAMERADEVICE_H
#define DARKOS_HARDWARE_CAMERA_ICAMERADEVICE_H

#include <camera/types.h>
#include <codec/types.h>
#include <hardware/hardware.h>

#include <errno.h>
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
#define CAMERA_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)
#define CAMERA_DEVICE_API_VERSION_1_1 HARDWARE_MAKE_API_VERSION(1, 1)

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

    /* DEPRECATED：预览是 Camera→Display 的管线编排职责，新代码必须通过
     * SvcKit Media 使用；保留到通用 Platform MediaLink 接住现有 RV1126B
     * RK_MPI_SYS_Bind 实现后删除。width/height = panel。 */
    int (*preview_start)(camera_device_t *dev, uint32_t width, uint32_t height);
    int (*preview_stop)(camera_device_t *dev);

    /*
     * 可选：摄像头直接输出编码访问单元。
     * 后端可以在内部使用硬件直连、软件编码或其他实现；不支持时为 NULL。
     * config/packet 复用 codec SPI 的通用数据契约。
     * 需要 CAMERA_DEVICE_API_VERSION_1_1。
     */
    int (*encoded_start)(camera_device_t *dev,
                         const codec_format_t *config);
    int (*encoded_get_packet)(camera_device_t *dev,
                              codec_buffer_t *packet, int timeout_ms);
    int (*encoded_stop)(camera_device_t *dev);

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
    camera_device_t *dev;
    int rc;

    /* 接口版本不匹配直接拒绝，避免拿到不兼容的 ops 表 */
    if (!hw_module_supports(module, CAMERA_MODULE_API_VERSION_1_0))
        return -EPROTONOSUPPORT;
    rc = module->methods->open(module, id, &hwdev);
    if (rc != 0)
        return rc;
    dev = (camera_device_t *)hwdev;
    if (dev->ops == NULL ||
        !hw_device_supports(&dev->common, CAMERA_DEVICE_API_VERSION_1_0)) {
        dev->common.close(&dev->common);
        return -EPROTONOSUPPORT;
    }
    *device = dev;
    return 0;
}

/* 便捷：从 module 打开 camera 设备（等价 camera_open_by_id(..., "camera", ...)） */
static inline int camera_open(const hw_module_t *module, camera_device_t **device) {
    return camera_open_by_id(module, CAMERA_HARDWARE_MODULE_ID, device);
}

/* 可选能力查询：旧版 1.0 Camera HAL 不会访问新增 ops 表成员。 */
static inline int camera_supports_encoded_output(const camera_device_t *device) {
    return device != NULL &&
           hw_device_supports(&device->common, CAMERA_DEVICE_API_VERSION_1_1) &&
           device->ops != NULL && device->ops->encoded_start != NULL &&
           device->ops->encoded_get_packet != NULL &&
           device->ops->encoded_stop != NULL;
}

/* 便捷：关闭设备 */
static inline int camera_close(camera_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_CAMERA_ICAMERADEVICE_H */
