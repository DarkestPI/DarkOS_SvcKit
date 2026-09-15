#ifndef DARKOS_HARDWARE_LIGHT_ILIGHT_H
#define DARKOS_HARDWARE_LIGHT_ILIGHT_H

#include <hardware/hardware.h>
#include <light/types.h>

#include <errno.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 灯设备接口（对应 Android 的 ILight）
 *
 * 无状态的简单外设：查能力后直接 set_light。TIMED 闪烁由实现维护
 * （软件定时器），上层只描述目标状态。
 * ------------------------------------------------------------------------- */

#define LIGHT_HARDWARE_MODULE_ID "light"
#define LIGHT_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)
#define LIGHT_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct light_device light_device_t;

typedef struct light_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(light_device_t *dev, light_caps_t *caps);

    /* 设置/读取某盏灯的状态 */
    int (*set_light)(light_device_t *dev, uint32_t id, const light_state_t *state);
    int (*get_light)(light_device_t *dev, uint32_t id, light_state_t *state);
} light_device_ops_t;

struct light_device {
    hw_device_t common;
    const light_device_ops_t *ops;
    void *priv;
};

static inline int light_open(const hw_module_t *module, light_device_t **device) {
    hw_device_t *hwdev = NULL;
    light_device_t *dev;
    int rc;

    /* 接口版本不匹配直接拒绝，避免拿到不兼容的 ops 表 */
    if (!hw_module_supports(module, LIGHT_MODULE_API_VERSION_1_0))
        return -EPROTONOSUPPORT;
    rc = module->methods->open(module, LIGHT_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    dev = (light_device_t *)hwdev;
    if (dev->ops == NULL ||
        !hw_device_supports(&dev->common, LIGHT_DEVICE_API_VERSION_1_0)) {
        dev->common.close(&dev->common);
        return -EPROTONOSUPPORT;
    }
    *device = dev;
    return 0;
}

static inline int light_close(light_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_LIGHT_ILIGHT_H */
