#ifndef DARKOS_HARDWARE_WIFI_IWIFI_H
#define DARKOS_HARDWARE_WIFI_IWIFI_H

#include <hardware/hardware.h>
#include <wifi/types.h>

#include <errno.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * wifi 设备接口（对应 Android 的 IWifiChip + IWifiStaIface 的简化合并）
 *
 * 当前只覆盖 STA 模式（扫描/连接/状态）；AP 配网模式待 frameworks 的
 * 配网流程明确后追加（set_ap_config 等）。扫描结果用回调逐个上报，
 * 避免上层预分配大数组。
 * ------------------------------------------------------------------------- */

#define WIFI_HARDWARE_MODULE_ID "wifi"
#define WIFI_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)
#define WIFI_DEVICE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct wifi_device wifi_device_t;

typedef struct wifi_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(wifi_device_t *dev, wifi_caps_t *caps);
    /* 打开WIFI */
    int (*wifi_enable)(wifi_device_t *dev);
    /* ABI 保留别名；新代码统一使用末尾的 get_status。 */
    int (*wifi_get_status)(wifi_device_t *dev, wifi_status_t *status);
    /* 关闭WIFI */
    int (*wifi_disable)(wifi_device_t *dev);
    /* 扫描 */
    int (*scan)(wifi_device_t *dev, wifi_scan_cb cb, void *ctx, int timeout_ms);

    /* 连接/断开（STA 模式）；connect 为阻塞调用，成功返回 0 */
    int (*connect)(wifi_device_t *dev, const wifi_config_t *cfg);
    int (*disconnect)(wifi_device_t *dev);

    /* 状态查询 */
    int (*get_status)(wifi_device_t *dev, wifi_status_t *status);
} wifi_device_ops_t;

struct wifi_device {
    hw_device_t common;
    const wifi_device_ops_t *ops;
    void *priv;
};

static inline int wifi_open(const hw_module_t *module, wifi_device_t **device) {
    hw_device_t *hwdev = NULL;
    wifi_device_t *dev;
    int rc;

    /* 接口版本不匹配直接拒绝，避免拿到不兼容的 ops 表 */
    if (!hw_module_supports(module, WIFI_MODULE_API_VERSION_1_0))
        return -EPROTONOSUPPORT;
    rc = module->methods->open(module, WIFI_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    dev = (wifi_device_t *)hwdev;
    if (dev->ops == NULL || !hw_device_supports(&dev->common, WIFI_DEVICE_API_VERSION_1_0)) {
        dev->common.close(&dev->common);
        return -EPROTONOSUPPORT;
    }
    *device = dev;
    return 0;
}

static inline int wifi_close(wifi_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_WIFI_IWIFI_H */
