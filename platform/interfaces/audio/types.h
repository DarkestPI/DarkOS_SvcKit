#ifndef DARKOS_HARDWARE_AUDIO_TYPES_H
#define DARKOS_HARDWARE_AUDIO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * audio 接口共享类型（对应 Android 的 audio/）
 *
 * IPC 相机的音频场景：麦克风采集 + 扬声器播放（双向语音对讲）。
 * 一个 audio 设备同时管理输入/输出两个方向的流，操作用 direction 区分。
 * Rockchip 实现基于 ALSA。
 * ------------------------------------------------------------------------- */

/* 流方向 */
typedef enum audio_direction {
    AUDIO_DIRECTION_INPUT = 0,  /* 采集（麦克风） */
    AUDIO_DIRECTION_OUTPUT = 1, /* 播放（扬声器） */
} audio_direction_t;

/* 能力位图：方向 */
#define AUDIO_CAPS_DIR_INPUT (1u << 0)
#define AUDIO_CAPS_DIR_OUTPUT (1u << 1)

/* 采样格式位图（取小端，对齐 ALSA SND_PCM_FORMAT_*） */
#define AUDIO_FORMAT_PCM_S16LE (1u << 0)
#define AUDIO_FORMAT_PCM_S24LE (1u << 1)
#define AUDIO_FORMAT_PCM_FLOAT (1u << 2)

/* 采样率位图（Hz） */
#define AUDIO_RATE_8000 (1u << 0)
#define AUDIO_RATE_16000 (1u << 1)
#define AUDIO_RATE_24000 (1u << 2)
#define AUDIO_RATE_32000 (1u << 3)
#define AUDIO_RATE_44100 (1u << 4)
#define AUDIO_RATE_48000 (1u << 5)

/* 能力描述 */
typedef struct audio_caps {
    uint32_t supported_directions; /* AUDIO_CAPS_DIR_* 位图 */
    uint32_t supported_formats;    /* AUDIO_FORMAT_PCM_* 位图 */
    uint32_t supported_rates;      /* AUDIO_RATE_* 位图 */
    uint32_t max_channels;
} audio_caps_t;

/* PCM 流格式 */
typedef struct audio_format {
    uint32_t sample_rate;
    uint32_t channel_count;
    uint32_t format; /* AUDIO_FORMAT_PCM_* 之一 */
} audio_format_t;

/* PCM 数据缓冲 */
typedef struct audio_buffer {
    void *data;
    uint32_t size; /* read：入参为容量、返回实际读取字节数；write：待写入字节数 */
    uint64_t timestamp_ns;
} audio_buffer_t;

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_AUDIO_TYPES_H */
