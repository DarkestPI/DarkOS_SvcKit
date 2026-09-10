#ifndef DARKOS_HARDWARE_AUDIO_IAUDIO_H
#define DARKOS_HARDWARE_AUDIO_IAUDIO_H

#include <audio/types.h>
#include <hardware/hardware.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 音频设备接口（对应 Android 的 IAudioDevice，采集/播放合一）
 *
 * 操作模型与 camera 对齐：能力查询 + 格式协商 + 启停 + 阻塞读写。
 * 音量等控制项用 set_control/get_control（id 见下方 AUDIO_CTRL_*）。
 * 回声消除/降噪等对讲相关能力后续按需追加。
 * ------------------------------------------------------------------------- */

#define AUDIO_HARDWARE_MODULE_ID "audio"
#define AUDIO_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

/* 通用控制项 id */
#define AUDIO_CTRL_VOLUME 1   /* 播放音量 0-100 */
#define AUDIO_CTRL_MUTE 2     /* 静音 0/1 */
#define AUDIO_CTRL_MIC_GAIN 3 /* 采集增益 0-100 */

typedef struct audio_device audio_device_t;

typedef struct audio_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(audio_device_t *dev, audio_caps_t *caps);

    /* 格式协商（按方向分别设置） */
    int (*set_format)(audio_device_t *dev, audio_direction_t dir, const audio_format_t *fmt);
    int (*get_format)(audio_device_t *dev, audio_direction_t dir, audio_format_t *fmt);

    /* 流控制（按方向分别启停；输入输出可同时运行，支持全双工对讲） */
    int (*start)(audio_device_t *dev, audio_direction_t dir);
    int (*stop)(audio_device_t *dev, audio_direction_t dir);

    /* 阻塞读写：timeout_ms < 0 表示无限等待。返回 0 成功；负 errno 失败 */
    int (*read)(audio_device_t *dev, audio_buffer_t *buf, int timeout_ms);
    int (*write)(audio_device_t *dev, const audio_buffer_t *buf, int timeout_ms);

    /* 控制项（音量/静音/增益，id 见 AUDIO_CTRL_*） */
    int (*set_control)(audio_device_t *dev, uint32_t id, int32_t value);
    int (*get_control)(audio_device_t *dev, uint32_t id, int32_t *value);
} audio_device_ops_t;

struct audio_device {
    hw_device_t common;
    const audio_device_ops_t *ops;
    void *priv;
};

static inline int audio_open(const hw_module_t *module, audio_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, AUDIO_HARDWARE_MODULE_ID, &hwdev);
    if (rc != 0)
        return rc;
    *device = (audio_device_t *)hwdev;
    return 0;
}

static inline int audio_close(audio_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_AUDIO_IAUDIO_H */
