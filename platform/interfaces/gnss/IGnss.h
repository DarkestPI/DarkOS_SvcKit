#ifndef DARKOS_HARDWARE_GNSS_IGNSS_H
#define DARKOS_HARDWARE_GNSS_IGNSS_H

#include <gnss/types.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * GNSS 设备接口（对应 Android 的 IGnss）
 *
 * 推送模型：start 后定位结果经 set_location_callback 注册的回调持续上报，
 * stop 停止。AGPS/星历注入等辅助能力待实际需要时追加。
 * ------------------------------------------------------------------------- */

#define GNSS_HARDWARE_MODULE_ID "gnss"
#define GNSS_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)
#define GNSS_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct gnss_device gnss_device_t;

typedef struct gnss_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(gnss_device_t *dev, gnss_caps_t *caps);

    /* 注册定位回调（start 前调用；cb 为 NULL 表示注销） */
    int (*set_location_callback)(gnss_device_t *dev, gnss_location_cb cb, void *ctx);

    /* 开始/停止定位 */
    int (*start)(gnss_device_t *dev);
    int (*stop)(gnss_device_t *dev);
} gnss_device_ops_t;

struct gnss_device {
    hw_device_t common;
    const gnss_device_ops_t *ops;
    void *priv;
};

static inline int gnss_open(const hw_module_t *module, gnss_device_t **device) {
    hw_device_t *hwdev = NULL;
    gnss_device_t *dev;
    int rc;

    /* 接口版本不匹配直接拒绝，避免拿到不兼容的 ops 表 */
    if (!hw_module_supports(module, GNSS_MODULE_API_VERSION_1_0))
        return -EPROTONOSUPPORT;
    rc = module->methods->open(module, GNSS_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    dev = (gnss_device_t *)hwdev;
    if (dev->ops == NULL || !hw_device_supports(&dev->common, GNSS_DEVICE_API_VERSION_1_0)) {
        dev->common.close(&dev->common);
        return -EPROTONOSUPPORT;
    }
    *device = dev;
    return 0;
}

static inline int gnss_close(gnss_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_GNSS_IGNSS_H */
