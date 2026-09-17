/*
 * 主机参考实现（host_x86 变体）：无硬件依赖，生成合成测试图案帧。
 *
 * 用途：在宿主机上开发/联调 frameworks、IPC 等上层，不依赖真实相机，
 *       也不依赖还在迭代中的 rockchip 实现（后续切 MPI）。
 *
 * 与 Rockchip SoC Camera HAL 实现同一套 camera_device_ops，上层无感知。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 *
 * 环境变量 DARKOS_CAMERA_DEVICE 指定 V4L2 设备节点（如 /dev/video0）时，
 * open 可分流到 shared/linux/camera 的 V4L2 后端，采集真实摄像头画面。
 *
 * 当前仅生成 NV12 测试图案（水平渐变 + 随帧号移动），
 * 后续可按需扩展 YUYV / RGB24 等。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setname_np（glibc 严格模式下需要） */
#endif

#include "camera_v4l2.h"

#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct host_camera_priv {
    int idx;  /* 实例号（open id "cameraN" 的 N） */
    int type; /* camera_type_t：env DARKOS_CAMERA{idx}_TYPE=ir|white，默认 white */

    camera_format_t fmt;
    uint8_t *buf; /* 复用缓冲，存一帧 NV12 */
    size_t buf_size;
    uint32_t seq; /* 帧序号，驱动测试图案移动 */

    camera_frame_cb cb;
    void *cb_ctx;
    pthread_t thread;
    atomic_bool running;
    int streaming;

    int32_t brightness;
    int32_t contrast;
    int32_t exposure;
    int32_t gain;
    int32_t white_balance;
} host_camera_priv_t;

/* ---------------------------------------------------------------------------
 * 测试图案生成
 * ------------------------------------------------------------------------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void add_ns(struct timespec *value, uint64_t nanoseconds) {
    value->tv_sec += (time_t)(nanoseconds / 1000000000ull);
    value->tv_nsec += (long)(nanoseconds % 1000000000ull);
    if (value->tv_nsec >= 1000000000L) {
        value->tv_sec++;
        value->tv_nsec -= 1000000000L;
    }
}

/* 单次绝对时间等待，避免按 1ms 分片造成每秒近千次无效唤醒。 */
static void sleep_until_frame(host_camera_priv_t *priv, const struct timespec *deadline) {
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (rc == EINTR && atomic_load_explicit(&priv->running, memory_order_relaxed));
}

static int ensure_buf(host_camera_priv_t *priv) {
    size_t ysize = (size_t)priv->fmt.width * priv->fmt.height;
    size_t uvsize = ysize / 2;
    size_t total = ysize + uvsize; /* NV12：1.5 字节/像素 */

    if (priv->buf == NULL || priv->buf_size < total) {
        uint8_t *p = (uint8_t *)realloc(priv->buf, total);
        if (p == NULL)
            return -ENOMEM;
        priv->buf = p;
        priv->buf_size = total;
    }
    return 0;
}

static void fill_test_pattern(host_camera_priv_t *priv, camera_frame_t *frame) {
    uint32_t w = priv->fmt.width;
    uint32_t h = priv->fmt.height;
    size_t ysize = (size_t)w * h;
    size_t uvsize = ysize / 2;
    uint8_t *p = priv->buf;
    uint32_t x, y;

    /* Y 平面：水平渐变，随帧号移动（可见的"动起来"效果） */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            p[y * w + x] = (uint8_t)((x + priv->seq * 8) & 0xff);

    /* UV 平面：中性色（灰） */
    memset(p + ysize, 128, uvsize);

    frame->index = priv->seq;
    frame->fd = -1;
    frame->data = priv->buf;
    frame->size = ysize + uvsize;
    frame->width = w;
    frame->height = h;
    frame->pixel_format = priv->fmt.pixel_format;
    frame->stride = w;
    frame->timestamp_ns = now_ns();
    frame->priv = NULL;
}

/* ---------------------------------------------------------------------------
 * 出帧线程（回调模型）
 * ------------------------------------------------------------------------- */

static void *generate_thread(void *arg) {
    host_camera_priv_t *priv = (host_camera_priv_t *)arg;
    unsigned fps = priv->fmt.fps ? priv->fmt.fps : 30;
    uint64_t interval_ns = 1000000000ull / fps;
    struct timespec next_frame;

    char tname[16];
    snprintf(tname, sizeof(tname), "cam%d.gen", priv->idx);
    tname[sizeof(tname) - 1] = '\0';
    pthread_setname_np(pthread_self(), tname);
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (atomic_load_explicit(&priv->running, memory_order_relaxed)) {
        camera_frame_t frame;
        if (ensure_buf(priv) != 0)
            break;
        fill_test_pattern(priv, &frame);
        priv->seq++;
        if (priv->cb != NULL)
            priv->cb(priv->cb_ctx, &frame);
        add_ns(&next_frame, interval_ns);
        sleep_until_frame(priv, &next_frame);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_camera_get_capabilities(camera_device_t *dev, camera_caps_t *caps) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;
    static const camera_res_fps_t k_res[] = {
        {640, 480, 30},
        {1280, 720, 30},
        {1920, 1080, 30},
    };

    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->min_width = 160;
    caps->min_height = 120;
    caps->max_width = 3840;
    caps->max_height = 2160;
    caps->supported_formats = CAMERA_CAPS_FMT_NV12;
    snprintf(caps->sensor_name, sizeof(caps->sensor_name), "synthetic");
    caps->camera_type = priv->type;
    caps->res_count = sizeof(k_res) / sizeof(k_res[0]);
    memcpy(caps->res, k_res, sizeof(k_res));
    return 0;
}

static int host_camera_set_format(camera_device_t *dev, const camera_format_t *fmt) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->pixel_format != CAMERA_PIX_FMT_NV12)
        return -EINVAL; /* 主机实现当前仅生成 NV12 */
    if (fmt->width < 160 || fmt->height < 120 || fmt->width > 3840 || fmt->height > 2160)
        return -EINVAL;
    if (fmt->fps == 0 || fmt->fps > 120)
        return -EINVAL;

    priv->fmt = *fmt;
    return 0;
}

static int host_camera_get_format(camera_device_t *dev, camera_format_t *fmt) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = priv->fmt;
    return 0;
}

static int host_camera_start(camera_device_t *dev) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;
    int rc;

    if (priv->streaming)
        return -EBUSY;

    priv->streaming = 1;
    if (priv->cb != NULL) {
        atomic_store_explicit(&priv->running, 1, memory_order_relaxed);
        rc = pthread_create(&priv->thread, NULL, generate_thread, priv);
        if (rc != 0) {
            atomic_store_explicit(&priv->running, 0, memory_order_relaxed);
            priv->streaming = 0;
            return -rc;
        }
    }
    return 0;
}

static int host_camera_stop(camera_device_t *dev) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;

    if (!priv->streaming)
        return 0;

    priv->streaming = 0;
    if (atomic_load_explicit(&priv->running, memory_order_relaxed)) {
        atomic_store_explicit(&priv->running, 0, memory_order_relaxed);
        pthread_join(priv->thread, NULL);
    }
    return 0;
}

static int host_camera_set_frame_callback(camera_device_t *dev, camera_frame_cb cb, void *ctx) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;
    priv->cb = cb;
    priv->cb_ctx = ctx;
    return 0;
}

static int host_camera_capture(camera_device_t *dev, camera_frame_t *frame, int timeout_ms) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;

    (void)timeout_ms; /* 主机实现即时出帧，无阻塞等待 */

    if (frame == NULL)
        return -EINVAL;
    if (!priv->streaming)
        return -EINVAL;

    if (ensure_buf(priv) != 0)
        return -ENOMEM;
    fill_test_pattern(priv, frame);
    priv->seq++;
    return 0;
}

static int host_camera_set_control(camera_device_t *dev, uint32_t id, int32_t value) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;
    switch (id) {
    case CAMERA_CTRL_BRIGHTNESS:
        priv->brightness = value;
        break;
    case CAMERA_CTRL_CONTRAST:
        priv->contrast = value;
        break;
    case CAMERA_CTRL_EXPOSURE:
        priv->exposure = value;
        break;
    case CAMERA_CTRL_GAIN:
        priv->gain = value;
        break;
    case CAMERA_CTRL_WHITE_BALANCE:
        priv->white_balance = value;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int host_camera_get_control(camera_device_t *dev, uint32_t id, int32_t *value) {
    host_camera_priv_t *priv = (host_camera_priv_t *)dev->priv;
    if (value == NULL)
        return -EINVAL;
    switch (id) {
    case CAMERA_CTRL_BRIGHTNESS:
        *value = priv->brightness;
        break;
    case CAMERA_CTRL_CONTRAST:
        *value = priv->contrast;
        break;
    case CAMERA_CTRL_EXPOSURE:
        *value = priv->exposure;
        break;
    case CAMERA_CTRL_GAIN:
        *value = priv->gain;
        break;
    case CAMERA_CTRL_WHITE_BALANCE:
        *value = priv->white_balance;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

/* 本地预览：host 无显示输出，记账返回 0（联调用） */
static int host_camera_preview_start(camera_device_t *dev, uint32_t width, uint32_t height) {
    (void)dev;
    printf("[camera] preview_start %ux%u（host 记账，无实际显示）\n", width, height);
    return 0;
}

static int host_camera_preview_stop(camera_device_t *dev) {
    (void)dev;
    return 0;
}

static const camera_device_ops_t host_camera_ops = {
    .get_capabilities = host_camera_get_capabilities,
    .set_format = host_camera_set_format,
    .get_format = host_camera_get_format,
    .start = host_camera_start,
    .stop = host_camera_stop,
    .set_frame_callback = host_camera_set_frame_callback,
    .capture = host_camera_capture,
    .set_control = host_camera_set_control,
    .get_control = host_camera_get_control,
    .preview_start = host_camera_preview_start,
    .preview_stop = host_camera_preview_stop,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_camera_close(hw_device_t *device) {
    camera_device_t *dev = (camera_device_t *)device;
    host_camera_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (host_camera_priv_t *)dev->priv;
    if (priv != NULL) {
        if (priv->streaming)
            host_camera_stop(dev);
        free(priv->buf);
        free(priv);
    }
    free(dev);
    return 0;
}

/* 解析 open id："camera"/"camera0" → 0，"cameraN" → N；非法返回 -1 */
static int host_camera_parse_idx(const char *id) {
    const char *prefix = CAMERA_HARDWARE_MODULE_ID; /* "camera" */
    size_t plen = strlen(prefix);
    long idx;
    char *end;

    if (id == NULL || strncmp(id, prefix, plen) != 0)
        return -1;
    if (id[plen] == '\0')
        return 0;
    idx = strtol(id + plen, &end, 10);
    if (*end != '\0' || idx < 0 || idx > 15)
        return -1;
    return (int)idx;
}

static int host_camera_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    camera_device_t *dev;
    host_camera_priv_t *priv;
    const char *uvc_dev;
    char env_name[64];
    const char *env_val;
    int idx;

    if (device == NULL)
        return -EINVAL;
    idx = host_camera_parse_idx(id);
    if (idx < 0) {
        fprintf(stderr, "host_camera: 非法设备 id \"%s\"（期望 camera / cameraN）\n",
                id != NULL ? id : "(null)");
        return -EINVAL;
    }

    /* 指定了 V4L2 设备节点则走 Linux 公共 V4L2 后端。
     * 实例 0 兼容旧变量 DARKOS_CAMERA_DEVICE；实例 N 用 DARKOS_CAMERA_DEVICE_N。 */
    snprintf(env_name, sizeof(env_name), "DARKOS_CAMERA_DEVICE_%d", idx);
    uvc_dev = getenv(env_name);
    if (uvc_dev == NULL && idx == 0)
        uvc_dev = getenv("DARKOS_CAMERA_DEVICE");
    if (uvc_dev != NULL && uvc_dev[0] != '\0')
        return linux_v4l2_camera_open(module, uvc_dev, device);

    dev = (camera_device_t *)calloc(1, sizeof(*dev));
    priv = (host_camera_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->idx = idx;
    /* 摄像头类型：DARKOS_CAMERA{idx}_TYPE=ir|white，默认白光（全彩） */
    snprintf(env_name, sizeof(env_name), "DARKOS_CAMERA%d_TYPE", idx);
    env_val = getenv(env_name);
    priv->type = (env_val != NULL && strcmp(env_val, "ir") == 0) ? CAMERA_TYPE_IR
                                                                 : CAMERA_TYPE_VISUAL;

    priv->fmt = (camera_format_t){
        .width = 1920, .height = 1080, .pixel_format = CAMERA_PIX_FMT_NV12, .fps = 30};
    atomic_init(&priv->running, 0);

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CAMERA_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_camera_close;
    dev->ops = &host_camera_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_camera_methods = {
    .open = host_camera_open,
};

struct hw_module_t HMI_camera = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = CAMERA_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Camera HAL (synthetic)",
    .author = "DarkOS",
    .methods = &host_camera_methods,
};
