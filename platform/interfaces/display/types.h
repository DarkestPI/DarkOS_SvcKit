#ifndef DARKOS_HARDWARE_DISPLAY_TYPES_H
#define DARKOS_HARDWARE_DISPLAY_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * display 接口共享类型（显示输出，对应 Android 的 hwcomposer 极简形态）
 *
 * 场景：本地预览/回放上屏。帧传递优先 fd（dma-buf）/priv（vendor 句柄）
 * 零拷贝，与 camera/media 的约定一致；fd 与 priv 均无效时用 data。
 * Rockchip 实现基于 rockit MPI VO。
 * ------------------------------------------------------------------------- */

/* 显示接口类型（位图下标，见 DISPLAY_CAPS_INTF_*） */
typedef enum display_intf {
    DISPLAY_INTF_MIPI = 0,
    DISPLAY_INTF_LCD = 1,
    DISPLAY_INTF_BT1120 = 2,
    DISPLAY_INTF_CVBS = 3,
    DISPLAY_INTF_HDMI = 4,
} display_intf_t;

/* 能力位图：接口 */
#define DISPLAY_CAPS_INTF_MIPI (1u << DISPLAY_INTF_MIPI)
#define DISPLAY_CAPS_INTF_LCD (1u << DISPLAY_INTF_LCD)
#define DISPLAY_CAPS_INTF_BT1120 (1u << DISPLAY_INTF_BT1120)
#define DISPLAY_CAPS_INTF_CVBS (1u << DISPLAY_INTF_CVBS)
#define DISPLAY_CAPS_INTF_HDMI (1u << DISPLAY_INTF_HDMI)

/* 能力位图：像素格式 */
#define DISPLAY_FMT_NV12 (1u << 0)
#define DISPLAY_FMT_RGB888 (1u << 1)

/* 能力描述 */
typedef struct display_caps {
    uint32_t supported_intfs;   /* DISPLAY_CAPS_INTF_* 位图 */
    uint32_t supported_formats; /* DISPLAY_FMT_* 位图 */
    uint32_t max_width;
    uint32_t max_height;
} display_caps_t;

/* 显示格式协商 */
typedef struct display_format {
    uint32_t width;        /* 期望显示分辨率；0 = 跟随 panel 时序回读 */
    uint32_t height;
    uint32_t pixel_format; /* DISPLAY_FMT_* 之一 */
    uint32_t intf;         /* display_intf_t */
    uint32_t fps;          /* 显示帧率，0 = 默认 30 */
} display_format_t;

/* 送显帧描述（语义与 camera_frame_t 一致） */
typedef struct display_frame {
    int fd;        /* dma-buf fd，< 0 表示无效 */
    void *data;    /* fd 无效时的进程内虚拟地址 */
    uint32_t size; /* 有效字节数 */
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;  /* DISPLAY_FMT_* */
    uint32_t stride;        /* 每行字节数（含对齐） */
    uint64_t timestamp_ns;
    void *priv; /* vendor 私有（rockchip：MB_BLK，持帧期间有效） */
} display_frame_t;

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_DISPLAY_TYPES_H */
