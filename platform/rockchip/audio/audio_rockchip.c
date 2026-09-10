/*
 * Rockchip RV1126B Audio HAL：基于 rockit MPI AI/AO 的采集/播放实现。
 *
 * 把 hardware/interfaces/audio 的抽象接口映射到 rockit MPI 音频通道：
 *   start(INPUT)  → AI SetPubAttr/Enable/EnableChn；read  → AI GetFrame
 *   start(OUTPUT) → AO SetPubAttr/Enable/EnableChn；write → AO SendFrame
 *   控制项 → AI/AO SetVolume、AO SetMute（均为 per-dev 语义，对齐 rkipc）
 *
 * 说明：
 *   - 对上只呈现 PCM S16LE；采样率/声道经 AIO_ATTR_S 协商（声卡侧与流侧
 *     一致，AO 另开 EnableReSmp 容忍上层送异采样率数据）；
 *   - AUDIO_FRAME_S/AUDIO_STREAM_S 不直接含数据指针：读经
 *     RK_MPI_MB_Handle2VirAddr 拷出；写经 RK_MPI_SYS_CreateMB 包用户缓冲
 *     送帧后立即归还（rkipc 同款时序）；
 *   - AENC/ADEC（G.711 等压缩）不在本层：HAL 只出 PCM，压缩归服务/协议层
 *     （要做对讲码流时在 SvcKit 接 RK_MPI_AENC/ADEC，rkipc 参考：
 *     AI --bind--> AENC(G711A) / ADEC(G711A) --bind--> AO）；
 *   - 声卡默认 "default"（rockit 按 dev id 开 ALSA 卡）；USB 声卡等用
 *     环境变量 DARKOS_AUDIO_CARD（如 "hw:1,0"）覆盖；
 *   - rk730 类声卡的功放开关（AMIX "spk switch"）未做，有需要按 rkipc
 *     common/audio/audio.c 的 rkipc_ao_init_ex 补。
 */

#include <audio/IAudio.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rk_comm_aio.h>
#include <rk_comm_mb.h>
#include <rk_mpi_ai.h>
#include <rk_mpi_ao.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>

#include "common/mpi_sys_guard.h"

#define RK_AI_DEV 0
#define RK_AI_CHN 0
#define RK_AO_DEV 0
#define RK_AO_CHN 0
#define RK_AIO_FRM_NUM 4
#define RK_AIO_PT_PER_FRM 1024 /* 每帧采样点（AIO 推荐值之一） */

typedef struct rk_audio_priv {
    audio_format_t fmt_in;
    audio_format_t fmt_out;
    int in_running;
    int out_running;
    int sys_acquired;

    int32_t volume;   /* 播放音量 0-100 */
    int32_t mute;     /* 0/1 */
    int32_t mic_gain; /* 采集增益 0-100 */
} rk_audio_priv_t;

/* 采样率数值 → AUDIO_SAMPLE_RATE_E（枚举值与数值相等，显式映射防错） */
static int rk_audio_rate_enum(uint32_t rate, AUDIO_SAMPLE_RATE_E *out) {
    switch (rate) {
    case 8000:
        *out = AUDIO_SAMPLE_RATE_8000;
        return 0;
    case 16000:
        *out = AUDIO_SAMPLE_RATE_16000;
        return 0;
    case 24000:
        *out = AUDIO_SAMPLE_RATE_24000;
        return 0;
    case 32000:
        *out = AUDIO_SAMPLE_RATE_32000;
        return 0;
    case 44100:
        *out = AUDIO_SAMPLE_RATE_44100;
        return 0;
    case 48000:
        *out = AUDIO_SAMPLE_RATE_48000;
        return 0;
    default:
        return -EINVAL;
    }
}

static AUDIO_SOUND_MODE_E rk_audio_sound_mode(uint32_t channels) {
    return channels >= 2 ? AUDIO_SOUND_MODE_STEREO : AUDIO_SOUND_MODE_MONO;
}

/* 填 AIO 公共属性（AI/AO 共用形态；声卡名可由环境变量覆盖） */
static void rk_audio_fill_aio_attr(const audio_format_t *fmt, AIO_ATTR_S *attr) {
    const char *card = getenv("DARKOS_AUDIO_CARD");

    memset(attr, 0, sizeof(*attr));
    snprintf((char *)attr->u8CardName, sizeof(attr->u8CardName), "%s",
             (card != NULL && card[0] != '\0') ? card : "default");
    attr->soundCard.channels = 2;
    attr->soundCard.sampleRate = fmt->sample_rate;
    attr->soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    rk_audio_rate_enum(fmt->sample_rate, &attr->enSamplerate); /* set_format 已校验 */
    attr->enBitwidth = AUDIO_BIT_WIDTH_16;
    attr->enSoundmode = rk_audio_sound_mode(fmt->channel_count);
    attr->u32EXFlag = 0;
    attr->u32FrmNum = RK_AIO_FRM_NUM;
    attr->u32PtNumPerFrm = RK_AIO_PT_PER_FRM;
    attr->u32ChnCnt = 2; /* FS 上通道数（对齐 rkipc） */
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int rk_audio_get_capabilities(audio_device_t *dev, audio_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_directions = AUDIO_CAPS_DIR_INPUT | AUDIO_CAPS_DIR_OUTPUT;
    caps->supported_formats = AUDIO_FORMAT_PCM_S16LE;
    caps->supported_rates = AUDIO_RATE_8000 | AUDIO_RATE_16000 | AUDIO_RATE_24000 |
                            AUDIO_RATE_32000 | AUDIO_RATE_44100 | AUDIO_RATE_48000;
    caps->max_channels = 2;
    return 0;
}

static int rk_audio_check_format(const audio_format_t *fmt) {
    AUDIO_SAMPLE_RATE_E dummy;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->format != AUDIO_FORMAT_PCM_S16LE)
        return -EINVAL; /* MPI AIO 对上只呈现 S16LE */
    if (fmt->channel_count < 1 || fmt->channel_count > 2)
        return -EINVAL;
    return rk_audio_rate_enum(fmt->sample_rate, &dummy);
}

static int rk_audio_set_format(audio_device_t *dev, audio_direction_t dir,
                               const audio_format_t *fmt) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;
    int rc = rk_audio_check_format(fmt);

    if (rc != 0)
        return rc;
    if (dir == AUDIO_DIRECTION_INPUT) {
        if (priv->in_running)
            return -EBUSY;
        priv->fmt_in = *fmt;
    } else {
        if (priv->out_running)
            return -EBUSY;
        priv->fmt_out = *fmt;
    }
    return 0;
}

static int rk_audio_get_format(audio_device_t *dev, audio_direction_t dir, audio_format_t *fmt) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = (dir == AUDIO_DIRECTION_INPUT) ? priv->fmt_in : priv->fmt_out;
    return 0;
}

static int rk_audio_ai_start(rk_audio_priv_t *priv) {
    AIO_ATTR_S attr;

    rk_audio_fill_aio_attr(&priv->fmt_in, &attr);
    if (RK_MPI_AI_SetPubAttr(RK_AI_DEV, &attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_audio: AI SetPubAttr failed\n");
        return -EIO;
    }
    if (RK_MPI_AI_Enable(RK_AI_DEV) != RK_SUCCESS) {
        fprintf(stderr, "rk_audio: AI Enable failed\n");
        return -EIO;
    }
    if (RK_MPI_AI_EnableChn(RK_AI_DEV, RK_AI_CHN) != RK_SUCCESS) {
        fprintf(stderr, "rk_audio: AI EnableChn failed\n");
        RK_MPI_AI_Disable(RK_AI_DEV);
        return -EIO;
    }
    if (priv->fmt_in.channel_count == 1)
        RK_MPI_AI_SetTrackMode(RK_AI_DEV, AUDIO_TRACK_FRONT_LEFT);
    RK_MPI_AI_SetVolume(RK_AI_DEV, priv->mic_gain);
    priv->in_running = 1;
    return 0;
}

static int rk_audio_ao_start(rk_audio_priv_t *priv) {
    AIO_ATTR_S attr;
    AO_CHN_PARAM_S chn_param;
    AUDIO_SAMPLE_RATE_E rate;

    rk_audio_fill_aio_attr(&priv->fmt_out, &attr);
    if (RK_MPI_AO_SetPubAttr(RK_AO_DEV, &attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_audio: AO SetPubAttr failed\n");
        return -EIO;
    }
    if (RK_MPI_AO_Enable(RK_AO_DEV) != RK_SUCCESS) {
        fprintf(stderr, "rk_audio: AO Enable failed\n");
        return -EIO;
    }
    memset(&chn_param, 0, sizeof(chn_param));
    chn_param.enLoopbackMode = AUDIO_LOOPBACK_NONE;
    RK_MPI_AO_SetChnParams(RK_AO_DEV, RK_AO_CHN, &chn_param);
    /* mono 流复制到左右声道；stereo 保持原样（对齐 rkipc） */
    RK_MPI_AO_SetTrackMode(RK_AO_DEV, priv->fmt_out.channel_count == 1
                                          ? AUDIO_TRACK_OUT_STEREO
                                          : AUDIO_TRACK_NORMAL);
    if (RK_MPI_AO_EnableChn(RK_AO_DEV, RK_AO_CHN) != RK_SUCCESS) {
        fprintf(stderr, "rk_audio: AO EnableChn failed\n");
        RK_MPI_AO_Disable(RK_AO_DEV);
        return -EIO;
    }
    /* 声明输入数据采样率，声卡率不一致时内部重采样 */
    if (rk_audio_rate_enum(priv->fmt_out.sample_rate, &rate) == 0)
        RK_MPI_AO_EnableReSmp(RK_AO_DEV, RK_AO_CHN, rate);
    RK_MPI_AO_SetVolume(RK_AO_DEV, priv->volume);
    priv->out_running = 1;
    return 0;
}

static int rk_audio_start(audio_device_t *dev, audio_direction_t dir) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;
    int rc;

    if (dir == AUDIO_DIRECTION_INPUT && priv->in_running)
        return -EBUSY;
    if (dir == AUDIO_DIRECTION_OUTPUT && priv->out_running)
        return -EBUSY;

    if (!priv->sys_acquired) {
        if (rk_mpi_sys_acquire() != 0)
            return -EIO;
        priv->sys_acquired = 1;
    }

    rc = (dir == AUDIO_DIRECTION_INPUT) ? rk_audio_ai_start(priv) : rk_audio_ao_start(priv);
    if (rc != 0 && !priv->in_running && !priv->out_running) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
    }
    return rc;
}

static int rk_audio_stop(audio_device_t *dev, audio_direction_t dir) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;

    if (dir == AUDIO_DIRECTION_INPUT && priv->in_running) {
        priv->in_running = 0;
        RK_MPI_AI_DisableChn(RK_AI_DEV, RK_AI_CHN);
        RK_MPI_AI_Disable(RK_AI_DEV);
    }
    if (dir == AUDIO_DIRECTION_OUTPUT && priv->out_running) {
        priv->out_running = 0;
        RK_MPI_AO_DisableReSmp(RK_AO_DEV, RK_AO_CHN);
        RK_MPI_AO_DisableChn(RK_AO_DEV, RK_AO_CHN);
        RK_MPI_AO_Disable(RK_AO_DEV);
    }
    if (!priv->in_running && !priv->out_running && priv->sys_acquired) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
    }
    return 0;
}

/* 阻塞读一帧 PCM（数据经 MB 虚拟地址拷出） */
static int rk_audio_read(audio_device_t *dev, audio_buffer_t *buf, int timeout_ms) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;
    AUDIO_FRAME_S frame;
    const void *data;

    if (buf == NULL || buf->data == NULL)
        return -EINVAL;
    if (!priv->in_running)
        return -EINVAL;

    memset(&frame, 0, sizeof(frame));
    if (RK_MPI_AI_GetFrame(RK_AI_DEV, RK_AI_CHN, &frame, NULL, timeout_ms) != RK_SUCCESS)
        return -ETIMEDOUT;

    data = RK_MPI_MB_Handle2VirAddr(frame.pMbBlk);
    if (data == NULL) {
        RK_MPI_AI_ReleaseFrame(RK_AI_DEV, RK_AI_CHN, &frame, NULL);
        return -EIO;
    }
    if (frame.u32Len > buf->size) { /* buf->size 入参为容量 */
        RK_MPI_AI_ReleaseFrame(RK_AI_DEV, RK_AI_CHN, &frame, NULL);
        return -ENOSPC;
    }
    memcpy(buf->data, data, frame.u32Len);
    buf->size = frame.u32Len;
    buf->timestamp_ns = frame.u64TimeStamp * 1000ull; /* MPI 时间戳单位 us */
    RK_MPI_AI_ReleaseFrame(RK_AI_DEV, RK_AI_CHN, &frame, NULL);
    return 0;
}

/* 阻塞写一帧 PCM（用户缓冲包成 MB 送 AO，发送返回即归还——rkipc 同款时序） */
static int rk_audio_write(audio_device_t *dev, const audio_buffer_t *buf, int timeout_ms) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;
    AUDIO_FRAME_S frame;
    MB_EXT_CONFIG_S ext;
    int rc;

    if (buf == NULL || buf->data == NULL || buf->size == 0)
        return -EINVAL;
    if (!priv->out_running)
        return -EINVAL;

    memset(&frame, 0, sizeof(frame));
    frame.u32Len = buf->size;
    frame.u64TimeStamp = buf->timestamp_ns / 1000ull;
    frame.enBitWidth = AUDIO_BIT_WIDTH_16;
    frame.enSoundMode = rk_audio_sound_mode(priv->fmt_out.channel_count);
    frame.bBypassMbBlk = RK_FALSE;

    memset(&ext, 0, sizeof(ext));
    ext.pOpaque = buf->data;
    ext.pu8VirAddr = (RK_U8 *)buf->data;
    ext.u64Size = buf->size;
    if (RK_MPI_SYS_CreateMB(&frame.pMbBlk, &ext) != RK_SUCCESS)
        return -ENOMEM;

    rc = RK_MPI_AO_SendFrame(RK_AO_DEV, RK_AO_CHN, &frame, timeout_ms);
    RK_MPI_MB_ReleaseMB(frame.pMbBlk);
    return rc == RK_SUCCESS ? 0 : -EIO;
}

static int rk_audio_set_control(audio_device_t *dev, uint32_t id, int32_t value) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;

    if (value < 0)
        value = 0;
    if (value > 100 && id != AUDIO_CTRL_MUTE)
        value = 100;

    switch (id) {
    case AUDIO_CTRL_VOLUME:
        priv->volume = value;
        if (priv->out_running && RK_MPI_AO_SetVolume(RK_AO_DEV, value) != RK_SUCCESS)
            return -EIO;
        return 0;
    case AUDIO_CTRL_MUTE:
        priv->mute = value ? 1 : 0;
        if (priv->out_running &&
            RK_MPI_AO_SetMute(RK_AO_DEV, priv->mute ? RK_TRUE : RK_FALSE, NULL) != RK_SUCCESS)
            return -EIO;
        return 0;
    case AUDIO_CTRL_MIC_GAIN:
        priv->mic_gain = value;
        if (priv->in_running && RK_MPI_AI_SetVolume(RK_AI_DEV, value) != RK_SUCCESS)
            return -EIO;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int rk_audio_get_control(audio_device_t *dev, uint32_t id, int32_t *value) {
    rk_audio_priv_t *priv = (rk_audio_priv_t *)dev->priv;

    if (value == NULL)
        return -EINVAL;
    switch (id) {
    case AUDIO_CTRL_VOLUME:
        *value = priv->volume;
        return 0;
    case AUDIO_CTRL_MUTE:
        *value = priv->mute;
        return 0;
    case AUDIO_CTRL_MIC_GAIN:
        *value = priv->mic_gain;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const audio_device_ops_t rk_audio_ops = {
    .get_capabilities = rk_audio_get_capabilities,
    .set_format = rk_audio_set_format,
    .get_format = rk_audio_get_format,
    .start = rk_audio_start,
    .stop = rk_audio_stop,
    .read = rk_audio_read,
    .write = rk_audio_write,
    .set_control = rk_audio_set_control,
    .get_control = rk_audio_get_control,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int rk_audio_close(hw_device_t *device) {
    audio_device_t *dev = (audio_device_t *)device;
    rk_audio_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (rk_audio_priv_t *)dev->priv;
    if (priv != NULL) {
        rk_audio_stop(dev, AUDIO_DIRECTION_INPUT);
        rk_audio_stop(dev, AUDIO_DIRECTION_OUTPUT);
        free(priv);
    }
    free(dev);
    return 0;
}

static int rk_audio_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    audio_device_t *dev;
    rk_audio_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (audio_device_t *)calloc(1, sizeof(*dev));
    priv = (rk_audio_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    /* 默认 8k 单声道（对讲场景主流配置） */
    priv->fmt_in = (audio_format_t){
        .sample_rate = 8000, .channel_count = 1, .format = AUDIO_FORMAT_PCM_S16LE};
    priv->fmt_out = priv->fmt_in;
    priv->volume = 50;
    priv->mic_gain = 50;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = rk_audio_close;
    dev->ops = &rk_audio_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出（合并库形态：hal.rockchip.so，dlsym("HMI_audio")）
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t rk_audio_methods = {
    .open = rk_audio_open,
};

struct hw_module_t HMI_audio = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = AUDIO_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = AUDIO_HARDWARE_MODULE_ID,
    .name = "Rockchip RV1126B Audio HAL (MPI AI/AO)",
    .author = "DarkOS",
    .methods = &rk_audio_methods,
};
