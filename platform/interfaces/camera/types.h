#ifndef DARKOS_HARDWARE_CAMERA_TYPES_H
#define DARKOS_HARDWARE_CAMERA_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * camera 接口共享类型（对应 Android 的 types.hal）
 *
 * 帧传递优先使用 fd（dma-buf），实现跨进程零拷贝；fd 无效时退回进程内
 * 虚拟地址 data。
 * ------------------------------------------------------------------------- */

/* 像素格式：取 V4L2 fourcc 值，便于 vendor 直接映射，无需转换 */
typedef enum camera_pixel_format {
    CAMERA_PIX_FMT_NV12 = 0x3231564e,  /* 'NV12' */
    CAMERA_PIX_FMT_NV21 = 0x3132564e,  /* 'NV21' */
    CAMERA_PIX_FMT_YUYV = 0x56595559,  /* 'YUYV' */
    CAMERA_PIX_FMT_RGB24 = 0x33424752, /* 'RGB3' */
} camera_pixel_format_t;

/* 分辨率/fps 档位（sensor 通常只支持若干离散档位） */
typedef struct camera_res_fps {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
} camera_res_fps_t;

#define CAMERA_CAPS_MAX_RES 16

/* 摄像头类型：红外（夜视）/ 白光（全彩）。产品级属性——同型号 SoC 配哪种
 * 镜头由板级设计决定，厂商实现经环境变量配置后由 caps 透出。 */
typedef enum camera_type {
    CAMERA_TYPE_VISUAL = 0, /* 白光（全彩），默认 */
    CAMERA_TYPE_IR = 1,     /* 红外（夜视） */
} camera_type_t;

/* 能力描述 */
typedef struct camera_caps {
    uint32_t max_width;
    uint32_t max_height;
    uint32_t min_width;
    uint32_t min_height;
    uint32_t supported_formats; /* 位图，见 CAMERA_CAPS_FMT_* */
    char sensor_name[32];       /* sensor 名（如 gc8613），未知填空串 */
    uint32_t camera_type;       /* camera_type_t */
    uint32_t res_count;         /* 离散档位数；0 表示连续范围（用 max/min 表达） */
    camera_res_fps_t res[CAMERA_CAPS_MAX_RES];
} camera_caps_t;

#define CAMERA_CAPS_FMT_NV12 (1u << 0)
#define CAMERA_CAPS_FMT_NV21 (1u << 1)
#define CAMERA_CAPS_FMT_YUYV (1u << 2)
#define CAMERA_CAPS_FMT_RGB24 (1u << 3)

/* 采集格式 */
typedef struct camera_format {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format; /* camera_pixel_format_t */
    uint32_t fps;
} camera_format_t;

/* 单帧描述 */
typedef struct camera_frame {
    uint32_t index;
    int fd;        /* dma-buf fd，< 0 表示无效 */
    void *data;    /* fd 无效时的进程内虚拟地址 */
    uint32_t size; /* 有效字节数 */
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t stride; /* 每行字节数（含对齐） */
    uint64_t timestamp_ns;
    void *priv; /* vendor 私有 */
} camera_frame_t;

/* 帧回调：返回 0 继续；非 0 停止回调（可选语义） */
typedef int (*camera_frame_cb)(void *ctx, const camera_frame_t *frame);

/* 通用控制项 id（对齐 V4L2 CID，便于 vendor 直接映射） */
#define CAMERA_CTRL_BRIGHTNESS 0x00980900    /* V4L2_CID_BRIGHTNESS */
#define CAMERA_CTRL_CONTRAST 0x00980901      /* V4L2_CID_CONTRAST  */
#define CAMERA_CTRL_EXPOSURE 0x00980911      /* V4L2_CID_EXPOSURE  */
#define CAMERA_CTRL_GAIN 0x00980913          /* V4L2_CID_GAIN      */
#define CAMERA_CTRL_WHITE_BALANCE 0x0098091c /* V4L2_CID_WHITE_BALANCE */

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_CAMERA_TYPES_H */
