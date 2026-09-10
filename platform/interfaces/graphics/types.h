#ifndef DARKOS_HARDWARE_GRAPHICS_TYPES_H
#define DARKOS_HARDWARE_GRAPHICS_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * graphics 接口共享类型（2D 加速/合成，对应 Android 的 graphics/）
 * Rockchip 实现基于 librga（im2d API）。
 * ------------------------------------------------------------------------- */

typedef struct graphics_rect {
    int32_t x;
    int32_t y;
    uint32_t w;
    uint32_t h;
} graphics_rect_t;

/* 2D buffer 描述（语义与 media_buffer 一致，后续可统一） */
typedef struct graphics_buffer {
    int fd;
    void *data;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format; /* 对齐 V4L2 fourcc / DRM format */
    void *priv;
} graphics_buffer_t;

/* 变换标志（对齐 librga 语义） */
#define GRAPHICS_ROTATE_90 (1u << 0)
#define GRAPHICS_ROTATE_180 (1u << 1)
#define GRAPHICS_ROTATE_270 (1u << 2)
#define GRAPHICS_FLIP_H (1u << 3)
#define GRAPHICS_FLIP_V (1u << 4)

/* ---------------------------------------------------------------------------
 * OSD 叠加（on-screen display）
 *
 * canvas 由实现持有并透出给上层直写（固定 ARGB8888，上层当前为 LVGL 渲染
 * 目标）；flush 把 dirty 区域生效到编码/显示链路。
 * rockchip 实现：VENC 目标基于 rockit RGN（OVERLAY_RGN canvas）；VO 目标
 * 基于 VO 专用 UI 层（CURSOR 模式 + SendFrame，RV1126B 上 RGN 不支持
 * attach VO，对照 rkipc rv1126b_dv ui/rk_ui.c）。
 * ------------------------------------------------------------------------- */

/* OSD 叠加目标 */
typedef enum osd_target {
    OSD_TARGET_VENC = 0, /* 叠加进编码码流（远端可见，本地预览无） */
    OSD_TARGET_VO = 1,   /* 叠加到显示输出（本地预览） */
} osd_target_t;

/* OSD 创建配置 */
typedef struct osd_config {
    osd_target_t target;
    uint32_t chn;          /* VENC 通道号或 VO layer */
    graphics_rect_t rect;  /* 叠加位置与尺寸（实现对齐要求自行就近调整） */
} osd_config_t;

/* canvas 像素格式：固定 ARGB8888 */
#define GRAPHICS_OSD_PIXEL_FORMAT 0x34385241u /* fourcc 'AR24'（ARGB8888） */

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_GRAPHICS_TYPES_H */
