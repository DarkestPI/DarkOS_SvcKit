#ifndef DARKOS_HARDWARE_MEDIA_TYPES_H
#define DARKOS_HARDWARE_MEDIA_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * media 接口共享类型（编解码，对应 Android 的 media/omx）
 * Rockchip 实现基于 MPP（mpp_create / mpp_init / mpp_start）。
 *
 * 帧传递优先使用 fd（dma-buf），实现跨进程零拷贝；fd 无效时退回进程内
 * 虚拟地址 data（与 camera/types.h 的约定一致）。
 * ------------------------------------------------------------------------- */

typedef enum media_codec_id {
    MEDIA_CODEC_H264 = 0,
    MEDIA_CODEC_H265 = 1,
    MEDIA_CODEC_MJPEG = 2,
    /* 透传伪编码：输入原样拷到输出。仅供 host_x86 参考实现验证管道语义
     * （buffer 流转、时间戳、keyframe 标记），不产生真实码流。 */
    MEDIA_CODEC_RAW = 3,
} media_codec_id_t;

/* 编码器能力描述 */
typedef struct media_codec_caps {
    uint32_t supported_codecs; /* 位图，见 MEDIA_CAPS_CODEC_* */
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
} media_codec_caps_t;

#define MEDIA_CAPS_CODEC_H264 (1u << 0)
#define MEDIA_CAPS_CODEC_H265 (1u << 1)
#define MEDIA_CAPS_CODEC_MJPEG (1u << 2)
#define MEDIA_CAPS_CODEC_RAW (1u << 3)

/* 编码参数 */
typedef struct media_codec_format {
    uint32_t codec; /* media_codec_id_t */
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format; /* 输入原始帧格式，取 V4L2 fourcc（同 camera_pixel_format_t） */
    uint32_t bitrate;      /* bps */
    uint32_t fps;
    uint32_t gop; /* I 帧间隔 */
} media_codec_format_t;

/* 通用媒体 buffer：编码时为原始帧输入 / 码流包输出；fd 优先（dma-buf
 * 零拷贝），fd < 0 时用 data */
typedef struct media_buffer {
    int fd;
    void *data;
    uint32_t size;    /* 输入：有效字节数；输出：实际写入字节数 */
    uint32_t offset;
    uint64_t timestamp_ns;
    uint32_t flags; /* 见 MEDIA_BUF_FLAG_* */
    void *priv;
} media_buffer_t;

#define MEDIA_BUF_FLAG_KEYFRAME (1u << 0)
#define MEDIA_BUF_FLAG_EOS (1u << 1)

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_MEDIA_TYPES_H */
