/*
 * 主机参考实现（host_x86 变体）：无硬件依赖，模拟双向 PCM 音频设备。
 *
 * 用途：在宿主机上验证 audio 接口语义（能力查询、格式协商、按方向启停、
 *       阻塞读写节拍、控制项存取），供 frameworks 上层联调，
 *       不依赖 ALSA。真实采集/播放由 rockchip 实现（ALSA）提供。
 *
 * 与 audio.rockchip.so（待开发）实现同一套 audio_device_ops，上层无感知。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 模拟语义：输入方向 read 生成 440Hz 正弦波，输出方向 write 收下数据丢弃；
 * 双向均按数据时长 usleep，模拟真实采集/播放节拍。
 */

#define _POSIX_C_SOURCE 200809L    /* clock_gettime 在 -std=c17 严格模式下需要特性宏（gnu17 下冗余，防御保留） */
#define _DEFAULT_SOURCE            /* usleep 同上（BSD 扩展） */

#include <hardware/hardware.h>
#include <audio/IAudio.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOST_AUDIO_SINE_FREQ_HZ 440.0 /* 输入方向正弦波频率 */
#define HOST_AUDIO_SINE_AMP 9830.0    /* 幅度：0.3 * 32767 */
#define HOST_AUDIO_BYTES_PER_SAMPLE 2 /* S16LE */

typedef struct host_audio_stream {
    audio_format_t fmt;
    int running;
    uint64_t sample_index; /* 已产生的采样点序号，驱动正弦波相位连续 */
} host_audio_stream_t;

typedef struct host_audio_priv {
    host_audio_stream_t in;
    host_audio_stream_t out;

    int32_t volume;
    int32_t mute;
    int32_t mic_gain;
} host_audio_priv_t;

/* ---------------------------------------------------------------------------
 * 工具函数
 * ------------------------------------------------------------------------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 不依赖 libm 的正弦近似：归约到 [-PI, PI] 后做泰勒展开，精度足够驱动测试音 */
static double host_sin(double x) {
    const double pi = 3.14159265358979323846;
    const double two_pi = 2.0 * pi;
    double x2, term, sum;
    int n;

    /* 归约到 [-PI, PI] */
    while (x > pi)
        x -= two_pi;
    while (x < -pi)
        x += two_pi;

    x2 = x * x;
    term = x;
    sum = x;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((double)(2 * n) * (double)(2 * n + 1));
        sum += term;
    }
    return sum;
}

static int check_direction(audio_direction_t dir) {
    return (dir == AUDIO_DIRECTION_INPUT || dir == AUDIO_DIRECTION_OUTPUT) ? 0 : -EINVAL;
}

static int check_format(const audio_format_t *fmt) {
    if (fmt == NULL)
        return -EINVAL;
    if (fmt->format != AUDIO_FORMAT_PCM_S16LE)
        return -EINVAL; /* 主机实现仅支持 S16LE */
    if (fmt->sample_rate != 8000 && fmt->sample_rate != 16000 && fmt->sample_rate != 48000)
        return -EINVAL;
    if (fmt->channel_count < 1 || fmt->channel_count > 2)
        return -EINVAL;
    return 0;
}

/* 按数据时长 sleep，模拟真实采集/播放节拍 */
static void sleep_for_frames(uint32_t frames, uint32_t sample_rate) {
    uint64_t us = (uint64_t)frames * 1000000ull / sample_rate;
    usleep((useconds_t)us);
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_audio_get_capabilities(audio_device_t *dev, audio_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_directions = AUDIO_CAPS_DIR_INPUT | AUDIO_CAPS_DIR_OUTPUT;
    caps->supported_formats = AUDIO_FORMAT_PCM_S16LE;
    caps->supported_rates = AUDIO_RATE_8000 | AUDIO_RATE_16000 | AUDIO_RATE_48000;
    caps->max_channels = 2;
    return 0;
}

static int host_audio_set_format(audio_device_t *dev, audio_direction_t dir,
                                 const audio_format_t *fmt) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;
    host_audio_stream_t *stream;
    int rc;

    rc = check_direction(dir);
    if (rc != 0)
        return rc;
    rc = check_format(fmt);
    if (rc != 0)
        return rc;

    stream = (dir == AUDIO_DIRECTION_INPUT) ? &priv->in : &priv->out;
    if (stream->running)
        return -EBUSY; /* 流运行中不允许重新协商格式 */

    stream->fmt = *fmt;
    stream->sample_index = 0;
    return 0;
}

static int host_audio_get_format(audio_device_t *dev, audio_direction_t dir,
                                 audio_format_t *fmt) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;

    if (check_direction(dir) != 0 || fmt == NULL)
        return -EINVAL;

    *fmt = (dir == AUDIO_DIRECTION_INPUT) ? priv->in.fmt : priv->out.fmt;
    return 0;
}

static int host_audio_start(audio_device_t *dev, audio_direction_t dir) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;
    host_audio_stream_t *stream;

    if (check_direction(dir) != 0)
        return -EINVAL;

    stream = (dir == AUDIO_DIRECTION_INPUT) ? &priv->in : &priv->out;
    if (stream->running)
        return -EBUSY;

    stream->running = 1;
    stream->sample_index = 0;
    return 0;
}

static int host_audio_stop(audio_device_t *dev, audio_direction_t dir) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;

    if (check_direction(dir) != 0)
        return -EINVAL;

    if (dir == AUDIO_DIRECTION_INPUT)
        priv->in.running = 0;
    else
        priv->out.running = 0;
    return 0;
}

static int host_audio_read(audio_device_t *dev, audio_buffer_t *buf, int timeout_ms) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;
    uint32_t frame_bytes, frames, i, ch;
    int16_t *pcm;

    (void)timeout_ms; /* 主机实现按数据时长节拍返回，无额外阻塞等待 */

    if (buf == NULL || buf->data == NULL)
        return -EINVAL;
    if (!priv->in.running)
        return -EINVAL; /* 输入方向未 start */

    frame_bytes = priv->in.fmt.channel_count * HOST_AUDIO_BYTES_PER_SAMPLE;
    frames = buf->size / frame_bytes; /* buf->size 入参为缓冲容量 */
    if (frames == 0)
        return -ENOSPC;

    /* 生成 440Hz 正弦波，各声道复制同一样本，相位随流持续递增 */
    pcm = (int16_t *)buf->data;
    for (i = 0; i < frames; i++) {
        double phase = HOST_AUDIO_SINE_FREQ_HZ * 2.0 * 3.14159265358979323846 *
                       (double)(priv->in.sample_index + i) / (double)priv->in.fmt.sample_rate;
        int16_t sample = (int16_t)(HOST_AUDIO_SINE_AMP * host_sin(phase));
        for (ch = 0; ch < priv->in.fmt.channel_count; ch++)
            pcm[i * priv->in.fmt.channel_count + ch] = sample;
    }
    priv->in.sample_index += frames;

    buf->size = frames * frame_bytes;
    buf->timestamp_ns = now_ns();

    sleep_for_frames(frames, priv->in.fmt.sample_rate);
    return 0;
}

static int host_audio_write(audio_device_t *dev, const audio_buffer_t *buf, int timeout_ms) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;
    uint32_t frame_bytes, frames;

    (void)timeout_ms; /* 主机实现无底层缓冲拥塞，收下即返回 */

    if (buf == NULL || buf->data == NULL)
        return -EINVAL;
    if (!priv->out.running)
        return -EINVAL; /* 输出方向未 start */

    frame_bytes = priv->out.fmt.channel_count * HOST_AUDIO_BYTES_PER_SAMPLE;
    frames = buf->size / frame_bytes;

    /* 数据丢弃，仅按播放时长 sleep 模拟真实消耗节拍 */
    sleep_for_frames(frames, priv->out.fmt.sample_rate);
    return 0;
}

static int host_audio_set_control(audio_device_t *dev, uint32_t id, int32_t value) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;

    switch (id) {
    case AUDIO_CTRL_VOLUME:
        priv->volume = value;
        return 0;
    case AUDIO_CTRL_MUTE:
        priv->mute = value;
        return 0;
    case AUDIO_CTRL_MIC_GAIN:
        priv->mic_gain = value;
        return 0;
    default:
        return -EINVAL;
    }
}

static int host_audio_get_control(audio_device_t *dev, uint32_t id, int32_t *value) {
    host_audio_priv_t *priv = (host_audio_priv_t *)dev->priv;

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
        return -EINVAL;
    }
}

static const audio_device_ops_t host_audio_ops = {
    .get_capabilities = host_audio_get_capabilities,
    .set_format = host_audio_set_format,
    .get_format = host_audio_get_format,
    .start = host_audio_start,
    .stop = host_audio_stop,
    .read = host_audio_read,
    .write = host_audio_write,
    .set_control = host_audio_set_control,
    .get_control = host_audio_get_control,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_audio_close(hw_device_t *device) {
    audio_device_t *dev = (audio_device_t *)device;

    if (dev == NULL)
        return 0;
    free(dev->priv);
    free(dev);
    return 0;
}

static int host_audio_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    audio_device_t *dev;
    host_audio_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (audio_device_t *)calloc(1, sizeof(*dev));
    priv = (host_audio_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    /* 默认格式：16kHz 单声道 S16LE，双向一致 */
    priv->in.fmt = (audio_format_t){
        .sample_rate = 16000,
        .channel_count = 1,
        .format = AUDIO_FORMAT_PCM_S16LE,
    };
    priv->out.fmt = priv->in.fmt;
    priv->volume = 50;
    priv->mute = 0;
    priv->mic_gain = 50;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = AUDIO_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_audio_close;
    dev->ops = &host_audio_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_audio_methods = {
    .open = host_audio_open,
};

struct hw_module_t HMI_audio = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = AUDIO_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = AUDIO_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Audio HAL (simulated duplex PCM)",
    .author = "DarkOS",
    .methods = &host_audio_methods,
};
