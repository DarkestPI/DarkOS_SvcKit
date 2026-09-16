#ifndef DARKOS_HARDWARE_CODEC_TYPES_H
#define DARKOS_HARDWARE_CODEC_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * codec 接口共享类型
 *
 * 帧传递优先使用 fd（dma-buf）；fd 无效时退回进程内虚拟地址 data。
 * 所有类型都是进程内 HAL SPI，不承诺跨进程 ABI。
 * ------------------------------------------------------------------------- */

typedef enum codec_id {
    CODEC_ID_H264 = 0,
    CODEC_ID_H265 = 1,
    CODEC_ID_MJPEG = 2,
    /* 透传伪编码：输入原样拷到输出。仅供 host_x86 参考实现验证管道语义
     * （buffer 流转、时间戳、keyframe 标记），不产生真实码流。 */
    CODEC_ID_RAW = 3,
} codec_id_t;

/* 编码器能力描述 */
typedef struct codec_caps {
    uint32_t supported_codecs; /* 位图，见 CODEC_CAPS_* */
    uint32_t min_width;
    uint32_t min_height;
    uint32_t max_width;
    uint32_t max_height;
} codec_caps_t;

#define CODEC_CAPS_H264 (1u << 0)
#define CODEC_CAPS_H265 (1u << 1)
#define CODEC_CAPS_MJPEG (1u << 2)
#define CODEC_CAPS_RAW (1u << 3)

/* 编码参数 */
typedef struct codec_format {
    uint32_t codec; /* codec_id_t */
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format; /* 输入原始帧格式，取 V4L2 fourcc（同 camera_pixel_format_t） */
    uint32_t bitrate_bps;
    uint32_t fps;
    uint32_t gop; /* I 帧间隔 */
} codec_format_t;

/* 通用媒体 buffer：编码时为原始帧输入 / 码流包输出；fd 优先（dma-buf
 * 零拷贝），fd < 0 时用 data */
typedef struct codec_buffer {
    int fd;
    void *data;
    /* 作为输入 buffer 时是有效字节数；作为输出 buffer 时，调用前是容量，
     * 调用成功后改写为实际字节数。下一版统一 buffer 将拆分这两个字段。 */
    uint32_t size;
    uint32_t offset;
    uint64_t timestamp_ns;
    uint32_t flags; /* 见 CODEC_BUFFER_FLAG_* */
    void *priv;
} codec_buffer_t;

#define CODEC_BUFFER_FLAG_KEYFRAME (1u << 0)
#define CODEC_BUFFER_FLAG_EOS (1u << 1)

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_CODEC_TYPES_H */
