#ifndef DARKOS_HARDWARE_MEDIA_ICODEC_H
#define DARKOS_HARDWARE_MEDIA_ICODEC_H

#include <hardware/hardware.h>
#include <media/types.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 编解码设备接口（对应 Android media/omx 的 codec）
 *
 * 目录名 media 对齐 Android 的 media/；运行时模块 id 用 "codec"。
 * 编码（camera→码流）与解码（码流→原始帧，本地回放/上屏）共用一个设备，
 * 按 codec 类型各自初始化；用不到的方向可不实现（返回 -ENOTSUP）。
 *
 * 操作模型与 camera 对齐：能力查询 + 格式协商 + 启停 + 帧处理。
 * encode/decode 均为阻塞模型（送一帧/一包，同步取回一个码流包/一帧），
 * 与 rockit MPI 的 SendFrame/GetStream、SendStream/GetFrame 语义对应；
 * 异步回调模型待流水线真正需要时再追加。
 * ------------------------------------------------------------------------- */

#define MEDIA_CODEC_HARDWARE_MODULE_ID "codec"
#define MEDIA_CODEC_MODULE_API_VERSION_1_0 HARDWARE_MAKE_API_VERSION(1, 0)

typedef struct codec_device codec_device_t;

typedef struct codec_device_ops {
    /* 能力查询 */
    int (*get_capabilities)(codec_device_t *dev, media_codec_caps_t *caps);

    /* 格式协商（含输入原始帧的 pixel_format） */
    int (*set_format)(codec_device_t *dev, const media_codec_format_t *fmt);
    int (*get_format)(codec_device_t *dev, media_codec_format_t *fmt);

    /* 启停 */
    int (*start)(codec_device_t *dev);
    int (*stop)(codec_device_t *dev);

    /* 阻塞编码：in 为一帧原始数据，out 预分配缓冲、返回码流包。
     * timeout_ms < 0 表示无限等待。返回 0 成功；负 errno 失败。 */
    int (*encode)(codec_device_t *dev, const media_buffer_t *in, media_buffer_t *out,
                  int timeout_ms);

    /* 阻塞解码：in 为一个码流包（Annex-B AU），out 预分配缓冲、返回一帧原始数据。
     * 解码器内部有缓冲/重排，单包不一定产帧：返回 -EAGAIN 表示"已收下、暂无
     * 可出帧"（调用方继续送下一包），-ETIMEDOUT 表示超时。首包须自带参数集
     * （从 IDR 起送）。不实现解码的实现置 -ENOTSUP。 */
    int (*decode)(codec_device_t *dev, const media_buffer_t *in, media_buffer_t *out,
                  int timeout_ms);

    /* 丢弃内部缓存（GOP 重置等场景） */
    int (*flush)(codec_device_t *dev);

    /* 运行态调整编码码率（bps）：只改码率，fps/分辨率不在此 op 范围
     * （要改需 stop→set_format→start 重建通道）。未实现返回 -ENOTSUP。 */
    int (*set_rc_param)(codec_device_t *dev, uint32_t bitrate_bps);
} codec_device_ops_t;

struct codec_device {
    hw_device_t common;
    const codec_device_ops_t *ops;
    void *priv;
};

/* 按实例 id 打开 codec 设备：多路编码/解码场景使用。
 * id 约定："codec"/"codec0" → 实例 0，"codecN" → 实例 N。
 * 厂商实现把实例号映射到硬件通道（rockchip：VENC/VDEC chn = N）。 */
static inline int codec_open_by_id(const hw_module_t *module, const char *id,
                                   codec_device_t **device) {
    hw_device_t *hwdev = NULL;
    int rc = module->methods->open(module, id, &hwdev);
    if (rc != 0)
        return rc;
    *device = (codec_device_t *)hwdev;
    return 0;
}

static inline int codec_open(const hw_module_t *module, codec_device_t **device) {
    return codec_open_by_id(module, MEDIA_CODEC_HARDWARE_MODULE_ID, device);
}

static inline int codec_close(codec_device_t *device) {
    return device->common.close(&device->common);
}

#ifdef __cplusplus
}
#endif

#endif /* DARKOS_HARDWARE_MEDIA_ICODEC_H */
