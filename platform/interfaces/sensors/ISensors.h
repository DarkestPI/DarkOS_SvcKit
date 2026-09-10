#ifndef DARKOS_HARDWARE_SENSORS_ISENSORS_H
#define DARKOS_HARDWARE_SENSORS_ISENSORS_H

#include <hardware/hardware.h>
#include <sensors/types.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 传感器设备接口（对应 Android 的 ISensors，轮询模型）
 *
 * 操作模型对齐 Android sensors_poll_device_t：枚举 → 激活 → 阻塞 poll。
 * 相机上传感器数量少、采样率低，poll 足够；FIFO batch 模式暂不需要。
 * ------------------------------------------------------------------------- */

#define SENSORS_HARDWARE_MODULE_ID "sensors"
#define SENSORS_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct sensors_device sensors_device_t;

typedef struct sensors_device_ops {
    /* 枚举传感器：list 为 NULL 时仅在 *count 返回总数；
     * 否则 *count 入参为数组容量、返回实际写入个数 */
    int (*get_sensors_list)(sensors_device_t *dev, sensor_info_t *list, uint32_t *count);

    /* 激活/停用（enabled 0/1）；激活后事件经 poll 获取 */
    int (*activate)(sensors_device_t *dev, int32_t handle, int enabled);

    /* 设置采样间隔（不小于 sensor_info_t.min_delay_us） */
    int (*set_delay)(sensors_device_t *dev, int32_t handle, uint32_t delay_us);

    /* 阻塞读取事件：events 容量 max_count，返回实际事件数（>= 0），
     * 超时返回 0；负 errno 失败。timeout_ms < 0 表示无限等待 */
    int (*poll)(sensors_device_t *dev, sensor_event_t *events, uint32_t max_count,
                int timeout_ms);
} sensors_device_ops_t;

struct sensors_device {
    hw_device_t common;
    const sensors_device_ops_t *ops;
    void *priv;
};

static inline int sensors_open(const hw_module_t *module, sensors_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, SENSORS_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    *device = (sensors_device_t *)hwdev;
    return 0;
}

static inline int sensors_close(sensors_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_SENSORS_ISENSORS_H */
