/*
 * host_x86 UVC 相机实现：DARKOS_CAMERA_DEVICE 指定 V4L2 设备节点时启用。
 *
 * 与 camera_host_x86.c（合成图案）实现同一套 camera_device_ops，由后者在
 * open 时按环境变量分流，上层无感知：
 *   DARKOS_CAMERA_DEVICE=/dev/video0 ./live_probe
 *
 * 采集像素格式优先 MJPEG（libjpeg 解码）：虚拟机 USB 直通等带宽受限环境下
 * 摄像头往往只有 MJPEG 档位能出流；DARKOS_CAMERA_FORMAT=yuyv 可强制 YUYV。
 * 无论采集是 MJPEG 还是 YUYV，采集线程都就地转换为 NV12 对上呈现，
 * 与编码器输入对齐。
 *
 * 设备节点在 open 时打开、set_format 时即完成 V4L2 格式协商（S_FMT/S_PARM），
 * 使上层能在 start 前通过 get_format 拿到实际生效的分辨率/帧率。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setname_np（glibc 严格模式下需要） */
#endif

#include "camera_uvc.h"

#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h> /* 依赖 stdio.h/stdio 的 FILE，须排在其后 */

#define UVC_NUM_BUFS 4 /* mmap 环形缓冲个数 */
#define UVC_POLL_TIMEOUT_MS 1000

typedef struct uvc_mmap_buf {
    void *start;
    size_t length;
} uvc_mmap_buf_t;

typedef struct uvc_camera_priv {
    int fd;
    camera_format_t fmt; /* 对上呈现：NV12，宽高为 S_FMT 实际生效值 */
    uint32_t v4l2_pixfmt; /* 采集格式：V4L2_PIX_FMT_MJPEG / YUYV */
    size_t yuyv_stride;   /* YUYV 时的 V4L2 bytesperline */

    uvc_mmap_buf_t bufs[UVC_NUM_BUFS];
    int n_bufs;
    int streaming;

    uint8_t *nv12; /* 复用缓冲，存一帧转换后的 NV12 */
    size_t nv12_size;
    uint32_t seq;

    camera_frame_cb cb;
    void *cb_ctx;
    pthread_t thread;
    volatile int running;
} uvc_camera_priv_t;

/* ioctl 包裹：被信号打断自动重试 */
static int xioctl(int fd, unsigned long req, void *arg) {
    int rc;
    do {
        rc = ioctl(fd, req, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static uint64_t uvc_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------------------
 * 帧格式转换（采集线程上下文）
 * ------------------------------------------------------------------------- */

/* YUYV（4:2:2 packed）→ NV12（Y 平面 + 交错 UV 平面），垂直方向 UV 取偶数行 */
static void yuyv_to_nv12(const uint8_t *src, size_t src_stride, uint8_t *dst, uint32_t w,
                         uint32_t h) {
    uint8_t *dst_uv = dst + (size_t)w * h;
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)y * src_stride;
        uint8_t *drow = dst + (size_t)y * w;
        for (x = 0; x < w; x++)
            drow[x] = srow[x * 2]; /* YUYV packed：Y 在偶数字节 */
        if ((y & 1u) == 0) {
            uint8_t *duv = dst_uv + (size_t)(y / 2) * w;
            for (x = 0; x + 1 < w; x += 2) {
                duv[x] = srow[x * 2 + 1];     /* U */
                duv[x + 1] = srow[x * 2 + 3]; /* V */
            }
        }
    }
}

/* libjpeg 错误出口：longjmp 回调用点，避免默认的 exit() */
typedef struct jpg_err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    int warned; /* 截断等警告标记：宁可丢帧也不出灰底残帧 */
} jpg_err_mgr_t;

static void jpg_error_exit(j_common_ptr cinfo) {
    jpg_err_mgr_t *err = (jpg_err_mgr_t *)cinfo->err;
    longjmp(err->jb, 1);
}

static void jpg_emit_silent(j_common_ptr cinfo, int msg_level) {
    if (msg_level < 0) /* 警告（如 Premature end of JPEG file）：标记丢弃 */
        ((jpg_err_mgr_t *)cinfo->err)->warned = 1;
}

/*
 * MJPEG → NV12：JCS_YCbCr 输出跳过色彩转换，得 4:4:4 Y/Cb/Cr 三元组，
 * Y 平面直接抽取，UV 取 2x2 块左上角（偶数行偶数列）交错存放。
 */
static int mjpeg_to_nv12(const uint8_t *src, size_t len, uint8_t *dst, uint32_t w, uint32_t h) {
    struct jpeg_decompress_struct cinfo;
    jpg_err_mgr_t jerr;
    uint8_t *row = NULL;
    int rc = -1;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpg_error_exit;
    jerr.pub.emit_message = jpg_emit_silent;
    jerr.warned = 0;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        free(row);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)src, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_YCbCr;
    jpeg_start_decompress(&cinfo);

    if (cinfo.output_width != w || cinfo.output_height != h) {
        fprintf(stderr, "uvc_camera: jpeg size %ux%u != negotiated %ux%u\n", cinfo.output_width,
                cinfo.output_height, w, h);
        longjmp(jerr.jb, 1);
    }

    row = (uint8_t *)malloc((size_t)w * 3);
    if (row == NULL)
        longjmp(jerr.jb, 1);

    uint8_t *dst_uv = dst + (size_t)w * h;
    while (cinfo.output_scanline < cinfo.output_height) {
        uint32_t y = cinfo.output_scanline;
        uint8_t *drow = dst + (size_t)y * w;
        uint32_t x;
        JSAMPROW rows[1] = {row};
        jpeg_read_scanlines(&cinfo, rows, 1);
        for (x = 0; x < w; x++)
            drow[x] = row[x * 3]; /* Y */
        if ((y & 1u) == 0) {
            uint8_t *duv = dst_uv + (size_t)(y / 2) * w;
            for (x = 0; x + 1 < w; x += 2) {
                duv[x] = row[x * 3 + 1];     /* Cb */
                duv[x + 1] = row[x * 3 + 2]; /* Cr */
            }
        }
    }

    jpeg_finish_decompress(&cinfo);
    /* 警告级损坏（截断/熵数据错）仍出帧，与 ffmpeg/VLC 的容错行为一致：
     * 弱带宽环境下全丢会断流；致命错误（非 JPEG 等）才丢帧 */
    rc = 0;
    jpeg_destroy_decompress(&cinfo);
    free(row);
    return rc;
}

static int ensure_nv12_buf(uvc_camera_priv_t *priv) {
    size_t total = (size_t)priv->fmt.width * priv->fmt.height * 3 / 2;
    if (priv->nv12 == NULL || priv->nv12_size < total) {
        uint8_t *p = (uint8_t *)realloc(priv->nv12, total);
        if (p == NULL)
            return -ENOMEM;
        priv->nv12 = p;
        priv->nv12_size = total;
    }
    return 0;
}

/* V4L2 时间戳 → ns。本驱动时间基准为 CLOCK_MONOTONIC（flags 含
 * V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC），与上层 RTP 时间轴一致 */
static uint64_t vbuf_ts_ns(const struct v4l2_buffer *vbuf) {
    return (uint64_t)vbuf->timestamp.tv_sec * 1000000000ull + (uint64_t)vbuf->timestamp.tv_usec * 1000ull;
}

/* 取一帧 → 转换 NV12 → 填 frame（数据在 priv->nv12，回调返回后即失效） */
static int grab_one_frame(uvc_camera_priv_t *priv, camera_frame_t *frame, int timeout_ms) {
    struct pollfd pfd = {.fd = priv->fd, .events = POLLIN};
    struct v4l2_buffer vbuf;
    int rc;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc == 0)
        return -ETIMEDOUT;
    if (rc < 0)
        return -errno;

    memset(&vbuf, 0, sizeof(vbuf));
    vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(priv->fd, VIDIOC_DQBUF, &vbuf) < 0)
        return -errno;

    /* 驱动标记的残帧（USB 丢包等）：不解码直接丢弃 */
    if (vbuf.flags & V4L2_BUF_FLAG_ERROR) {
        xioctl(priv->fd, VIDIOC_QBUF, &vbuf);
        return -EIO;
    }

    rc = ensure_nv12_buf(priv);
    if (rc == 0) {
        if (priv->v4l2_pixfmt == V4L2_PIX_FMT_MJPEG)
            rc = mjpeg_to_nv12((const uint8_t *)priv->bufs[vbuf.index].start, vbuf.bytesused,
                               priv->nv12, priv->fmt.width, priv->fmt.height);
        else
            yuyv_to_nv12((const uint8_t *)priv->bufs[vbuf.index].start, priv->yuyv_stride,
                         priv->nv12, priv->fmt.width, priv->fmt.height);
    }

    if (xioctl(priv->fd, VIDIOC_QBUF, &vbuf) < 0) /* 缓冲必须归还，即便转换失败 */
        return -errno;
    if (rc != 0)
        return rc;

    frame->index = priv->seq++;
    frame->fd = -1;
    frame->data = priv->nv12;
    frame->size = (uint32_t)((size_t)priv->fmt.width * priv->fmt.height * 3 / 2);
    frame->width = priv->fmt.width;
    frame->height = priv->fmt.height;
    frame->pixel_format = CAMERA_PIX_FMT_NV12;
    frame->stride = priv->fmt.width;
    /* 时间戳用驱动给的采集时刻（MONOTONIC）；异常老驱动全零时退回当前时刻。
     * 用出队时刻会在缓冲积压时产生节奏抖动，客户端按 RTP 时间戳渲染会卡 */
    frame->timestamp_ns = (vbuf.timestamp.tv_sec != 0 || vbuf.timestamp.tv_usec != 0)
                              ? vbuf_ts_ns(&vbuf)
                              : uvc_now_ns();
    frame->priv = NULL;
    return 0;
}

static void *uvc_capture_thread(void *arg) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)arg;
    unsigned err_count = 0;

    pthread_setname_np(pthread_self(), "cam.uvc"); /* priv 无实例号，静态名 */

    while (priv->running) {
        camera_frame_t frame;
        int rc = grab_one_frame(priv, &frame, UVC_POLL_TIMEOUT_MS);
        if (rc == -ETIMEDOUT)
            continue;
        if (rc != 0) {
            if (rc == -ENODEV) {
                /* 设备已拔出：退出采集线程，避免空转刷日志 */
                fprintf(stderr, "uvc_camera: device disconnected, capture thread exit\n");
                break;
            }
            /* 残帧/解码失败限频打印：弱带宽环境下可能持续大量发生 */
            if (++err_count % 100 == 1)
                fprintf(stderr, "uvc_camera: dropped %u bad frames (last rc=%d)\n", err_count,
                        rc);
            continue;
        }
        if (priv->cb != NULL)
            priv->cb(priv->cb_ctx, &frame);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int uvc_camera_get_capabilities(camera_device_t *dev, camera_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->min_width = 160;
    caps->min_height = 120;
    caps->max_width = 3840;
    caps->max_height = 2160;
    caps->supported_formats = CAMERA_CAPS_FMT_NV12; /* 采集格式内部转换，对上 NV12 */
    snprintf(caps->sensor_name, sizeof(caps->sensor_name), "uvc");
    /* 档位枚举（VIDIOC_ENUM_FRAMESIZES）待需要时细化，先按连续范围表达 */
    return 0;
}

/* 按优先级尝试一种采集格式，返回实际生效的 fourcc；均失败返回 0 */
static uint32_t try_s_fmt(int fd, const camera_format_t *fmt, struct v4l2_format *out) {
    uint32_t order[2] = {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV};
    const char *pref = getenv("DARKOS_CAMERA_FORMAT");
    size_t i;

    if (pref != NULL && strcmp(pref, "yuyv") == 0) {
        order[0] = V4L2_PIX_FMT_YUYV;
        order[1] = V4L2_PIX_FMT_MJPEG;
    }

    for (i = 0; i < 2; i++) {
        memset(out, 0, sizeof(*out));
        out->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        out->fmt.pix.width = fmt->width;
        out->fmt.pix.height = fmt->height;
        out->fmt.pix.pixelformat = order[i];
        out->fmt.pix.field = V4L2_FIELD_ANY; /* 部分 UVC 驱动对 FIELD_NONE 不出帧 */
        if (xioctl(fd, VIDIOC_S_FMT, out) == 0 && out->fmt.pix.pixelformat == order[i])
            return order[i];
    }
    return 0;
}

static int uvc_camera_set_format(camera_device_t *dev, const camera_format_t *fmt) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    struct v4l2_format vfmt;
    struct v4l2_streamparm parm;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->pixel_format != CAMERA_PIX_FMT_NV12)
        return -EINVAL; /* 对上只呈现 NV12（内部转换） */
    if (priv->streaming)
        return -EBUSY;

    priv->v4l2_pixfmt = try_s_fmt(priv->fd, fmt, &vfmt);
    if (priv->v4l2_pixfmt == 0) {
        fprintf(stderr, "uvc_camera: VIDIOC_S_FMT failed: %s\n", strerror(errno));
        return -ENOTSUP;
    }
    fprintf(stderr, "uvc_camera: capture format %s %ux%u\n",
            priv->v4l2_pixfmt == V4L2_PIX_FMT_MJPEG ? "MJPEG" : "YUYV", vfmt.fmt.pix.width,
            vfmt.fmt.pix.height);

    priv->fmt = *fmt;
    priv->fmt.width = vfmt.fmt.pix.width;
    priv->fmt.height = vfmt.fmt.pix.height;
    priv->yuyv_stride = vfmt.fmt.pix.bytesperline;

    /* 帧率尽力设置，失败不算错误（部分驱动不支持 S_PARM） */
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fmt->fps;
    if (xioctl(priv->fd, VIDIOC_S_PARM, &parm) == 0 &&
        parm.parm.capture.timeperframe.numerator != 0) {
        priv->fmt.fps =
            parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator;
    }

    if (priv->fmt.width != fmt->width || priv->fmt.height != fmt->height)
        fprintf(stderr, "uvc_camera: negotiated %ux%u (requested %ux%u)\n", priv->fmt.width,
                priv->fmt.height, fmt->width, fmt->height);
    return 0;
}

static int uvc_camera_get_format(camera_device_t *dev, camera_format_t *fmt) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = priv->fmt;
    return 0;
}

static int uvc_camera_start(camera_device_t *dev) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    struct v4l2_requestbuffers req;
    enum v4l2_buf_type type;
    int i, rc;

    if (priv->streaming)
        return -EBUSY;

    memset(&req, 0, sizeof(req));
    req.count = UVC_NUM_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(priv->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "uvc_camera: VIDIOC_REQBUFS failed: %s\n", strerror(errno));
        return -errno;
    }
    priv->n_bufs = (int)req.count;

    for (i = 0; i < priv->n_bufs; i++) {
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        vbuf.index = (uint32_t)i;
        if (xioctl(priv->fd, VIDIOC_QUERYBUF, &vbuf) < 0)
            return -errno;
        priv->bufs[i].length = vbuf.length;
        priv->bufs[i].start =
            mmap(NULL, vbuf.length, PROT_READ | PROT_WRITE, MAP_SHARED, priv->fd, vbuf.m.offset);
        if (priv->bufs[i].start == MAP_FAILED) {
            fprintf(stderr, "uvc_camera: mmap failed: %s\n", strerror(errno));
            return -errno;
        }
        if (xioctl(priv->fd, VIDIOC_QBUF, &vbuf) < 0)
            return -errno;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(priv->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "uvc_camera: VIDIOC_STREAMON failed: %s\n", strerror(errno));
        return -errno;
    }

    priv->streaming = 1;
    if (priv->cb != NULL) {
        priv->running = 1;
        rc = pthread_create(&priv->thread, NULL, uvc_capture_thread, priv);
        if (rc != 0) {
            priv->running = 0;
            priv->streaming = 0;
            xioctl(priv->fd, VIDIOC_STREAMOFF, &type);
            return -rc;
        }
    }
    return 0;
}

static int uvc_camera_stop(camera_device_t *dev) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    enum v4l2_buf_type type;
    int i;

    if (!priv->streaming)
        return 0;

    priv->streaming = 0;
    if (priv->running) {
        priv->running = 0;
        pthread_join(priv->thread, NULL);
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(priv->fd, VIDIOC_STREAMOFF, &type);
    for (i = 0; i < priv->n_bufs; i++) {
        if (priv->bufs[i].start != NULL && priv->bufs[i].start != MAP_FAILED)
            munmap(priv->bufs[i].start, priv->bufs[i].length);
        priv->bufs[i].start = NULL;
    }
    priv->n_bufs = 0;
    return 0;
}

static int uvc_camera_set_frame_callback(camera_device_t *dev, camera_frame_cb cb, void *ctx) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    priv->cb = cb;
    priv->cb_ctx = ctx;
    return 0;
}

static int uvc_camera_capture(camera_device_t *dev, camera_frame_t *frame, int timeout_ms) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;

    if (frame == NULL)
        return -EINVAL;
    if (!priv->streaming)
        return -EINVAL;
    return grab_one_frame(priv, frame, timeout_ms);
}

/* 控制项 id 已对齐 V4L2 CID（见 camera/types.h），直接透传 */
static int uvc_camera_set_control(camera_device_t *dev, uint32_t id, int32_t value) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    struct v4l2_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    ctrl.value = value;
    if (xioctl(priv->fd, VIDIOC_S_CTRL, &ctrl) < 0)
        return -errno;
    return 0;
}

static int uvc_camera_get_control(camera_device_t *dev, uint32_t id, int32_t *value) {
    uvc_camera_priv_t *priv = (uvc_camera_priv_t *)dev->priv;
    struct v4l2_control ctrl;

    if (value == NULL)
        return -EINVAL;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = id;
    if (xioctl(priv->fd, VIDIOC_G_CTRL, &ctrl) < 0)
        return -errno;
    *value = ctrl.value;
    return 0;
}

static const camera_device_ops_t uvc_camera_ops = {
    .get_capabilities = uvc_camera_get_capabilities,
    .set_format = uvc_camera_set_format,
    .get_format = uvc_camera_get_format,
    .start = uvc_camera_start,
    .stop = uvc_camera_stop,
    .set_frame_callback = uvc_camera_set_frame_callback,
    .capture = uvc_camera_capture,
    .set_control = uvc_camera_set_control,
    .get_control = uvc_camera_get_control,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int uvc_camera_close(hw_device_t *device) {
    camera_device_t *dev = (camera_device_t *)device;
    uvc_camera_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (uvc_camera_priv_t *)dev->priv;
    if (priv != NULL) {
        if (priv->streaming)
            uvc_camera_stop(dev);
        if (priv->fd >= 0)
            close(priv->fd);
        free(priv->nv12);
        free(priv);
    }
    free(dev);
    return 0;
}

int uvc_camera_open(const hw_module_t *module, const char *device_path, hw_device_t **device) {
    camera_device_t *dev;
    uvc_camera_priv_t *priv;
    int fd;

    if (device == NULL || device_path == NULL)
        return -EINVAL;

    fd = open(device_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "uvc_camera: open %s failed: %s\n", device_path, strerror(errno));
        return -errno;
    }

    dev = (camera_device_t *)calloc(1, sizeof(*dev));
    priv = (uvc_camera_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        close(fd);
        return -ENOMEM;
    }

    priv->fd = fd;
    priv->fmt = (camera_format_t){
        .width = 640, .height = 480, .pixel_format = CAMERA_PIX_FMT_NV12, .fps = 30};

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = uvc_camera_close;
    dev->ops = &uvc_camera_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}
