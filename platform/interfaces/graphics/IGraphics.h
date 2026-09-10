#ifndef DARKOS_HARDWARE_GRAPHICS_IGRAPHICS_H
#define DARKOS_HARDWARE_GRAPHICS_IGRAPHICS_H

#include <graphics/types.h>
#include <hardware/hardware.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 2D 加速/合成设备接口（对应 Android graphics/；初稿，待细化）
 * ------------------------------------------------------------------------- */

#define GRAPHICS_HARDWARE_MODULE_ID "graphics"
#define GRAPHICS_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct graphics_device graphics_device_t;

typedef struct graphics_device_ops {
    /* 缩放/裁剪/旋转/格式转换（RGA 核心 blit） */
    int (*blit)(graphics_device_t *dev, const graphics_buffer_t *src,
                const graphics_rect_t *src_rect, const graphics_buffer_t *dst,
                const graphics_rect_t *dst_rect, uint32_t transform);

    /* 填充纯色 */
    int (*fill)(graphics_device_t *dev, graphics_buffer_t *dst, const graphics_rect_t *rect,
                uint32_t color);

    /* ---- OSD 叠加（未实现的返回 -ENOTSUP）----
     * osd_create：按 cfg 创建 OSD 叠加并透出可直写的 canvas（ARGB8888，
     * data/width/height/stride 填好；canvas 所有权归设备，osd_destroy 收回）。
     * osd_flush：dirty 区域画完后调用，生效到编码/显示链路（NULL = 整幅）。
     * 同一设备同时只支持一个 OSD 实例（v1）。 */
    int (*osd_create)(graphics_device_t *dev, const osd_config_t *cfg,
                      graphics_buffer_t *canvas_out);
    int (*osd_flush)(graphics_device_t *dev, const graphics_rect_t *dirty);
    int (*osd_destroy)(graphics_device_t *dev);
} graphics_device_ops_t;

struct graphics_device {
    hw_device_t common;
    const graphics_device_ops_t *ops;
    void *priv;
};

static inline int graphics_open(const hw_module_t *module, graphics_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, GRAPHICS_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    *device = (graphics_device_t *)hwdev;
    return 0;
}

static inline int graphics_close(graphics_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_GRAPHICS_IGRAPHICS_H */
