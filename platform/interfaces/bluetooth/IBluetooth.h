#ifndef DARKOS_HARDWARE_BLUETOOTH_IBLUETOOTH_H
#define DARKOS_HARDWARE_BLUETOOTH_IBLUETOOTH_H

#include <bluetooth/types.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 蓝牙设备接口（对应 Android 的 IBluetoothHci + bt_interface_t 的简化合并）
 *
 * 当前覆盖：适配器开关、名称、设备发现。配对/GATT（BLE 配网的数据通道）
 * 待 frameworks 配网流程明确后追加。
 * ------------------------------------------------------------------------- */

#define BLUETOOTH_HARDWARE_MODULE_ID "bluetooth"
#define BLUETOOTH_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)
#define BLUETOOTH_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct bluetooth_device bluetooth_device_t;

typedef struct bluetooth_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(bluetooth_device_t *dev, bt_caps_t *caps);

    /* 适配器开关 */
    int (*enable)(bluetooth_device_t *dev);
    int (*disable)(bluetooth_device_t *dev);
    int (*get_state)(bluetooth_device_t *dev, uint32_t *state); /* bt_state_t */

    /* 本机名称（对端可见） */
    int (*set_name)(bluetooth_device_t *dev, const char *name);
    int (*get_name)(bluetooth_device_t *dev, char *name, uint32_t size);

    /* 设备发现：每个对端经 cb 上报一次，直至 stop_discovery 或 cb 返回非 0 */
    int (*start_discovery)(bluetooth_device_t *dev, bt_discovery_cb cb, void *ctx);
    int (*stop_discovery)(bluetooth_device_t *dev);
} bluetooth_device_ops_t;

struct bluetooth_device {
    hw_device_t common;
    const bluetooth_device_ops_t *ops;
    void *priv;
};

static inline int bluetooth_open(const hw_module_t *module, bluetooth_device_t **device) {
    hw_device_t *hwdev = NULL;
    bluetooth_device_t *dev;
    int rc;

    /* 接口版本不匹配直接拒绝，避免拿到不兼容的 ops 表 */
    if (!hw_module_supports(module, BLUETOOTH_MODULE_API_VERSION_1_0))
        return -EPROTONOSUPPORT;
    rc = module->methods->open(module, BLUETOOTH_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    dev = (bluetooth_device_t *)hwdev;
    if (dev->ops == NULL ||
        !hw_device_supports(&dev->common, BLUETOOTH_DEVICE_API_VERSION_1_0)) {
        dev->common.close(&dev->common);
        return -EPROTONOSUPPORT;
    }
    *device = dev;
    return 0;
}

static inline int bluetooth_close(bluetooth_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_BLUETOOTH_IBLUETOOTH_H */
