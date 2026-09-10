#ifndef DARKOS_HARDWARE_DISPLAY_IDISPLAY_H
#define DARKOS_HARDWARE_DISPLAY_IDISPLAY_H

#include <display/types.h>
#include <hardware/hardware.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 显示输出设备接口（对应 Android 的 hwcomposer 极简形态）
 *
 * 操作模型与其他模块对齐：能力查询 + 格式协商（含接口选择）+ 启停 +
 * 阻塞送帧。panel 时序由 vendor 侧回读/兜底（未接屏时 start 可失败，
 * 由上层决定降级策略，如跳过本地预览）。
 *
 * 共享式语义：显示是单板单资源，open/start 可被多个服务各调一次
 * （如 MediaService 预览 + UIService 的 VO 目标 OSD）。实现须引用计数：
 * 重复 start 幂等成功，close/stop 归零才真正关显示（rockchip 实现即如此）。
 * ------------------------------------------------------------------------- */

#define DISPLAY_HARDWARE_MODULE_ID "display"
#define DISPLAY_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct display_device display_device_t;

typedef struct display_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(display_device_t *dev, display_caps_t *caps);

    /* 格式协商：含显示接口选择；宽高为 0 表示跟随 panel 回读 */
    int (*set_format)(display_device_t *dev, const display_format_t *fmt);
    int (*get_format)(display_device_t *dev, display_format_t *fmt);

    /* 启停（start 完成 VO dev/layer/chn 建立；未接屏可失败返回负 errno） */
    int (*start)(display_device_t *dev);
    int (*stop)(display_device_t *dev);

    /* 阻塞送一帧显示：timeout_ms < 0 表示无限等待。返回 0 成功；负 errno 失败。
     * 帧内 fd/priv 仅在调用期间有效（实现须在返回前收下或拷走） */
    int (*show)(display_device_t *dev, const display_frame_t *frame, int timeout_ms);
} display_device_ops_t;

struct display_device {
    hw_device_t common;
    const display_device_ops_t *ops;
    void *priv;
};

/* 便捷：从 module 打开 display 设备 */
static inline int display_open(const hw_module_t *module, display_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, DISPLAY_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    *device = (display_device_t *)hwdev;
    return 0;
}

/* 便捷：关闭设备 */
static inline int display_close(display_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_DISPLAY_IDISPLAY_H */
