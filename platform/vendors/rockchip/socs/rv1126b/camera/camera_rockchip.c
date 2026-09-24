/*
 * Rockchip RV1126B Camera HAL：基于 rockit MPI VI 的采集实现。
 *
 * 把 hardware/interfaces/camera 的抽象接口映射到 rockit MPI VI 通道：
 *   start → RK_MPI_SYS_Init + VI dev/pipe/chn 建立
 *   采集线程 → RK_MPI_VI_GetChnFrame → 拷贝出 NV12 → 帧回调 → ReleaseChnFrame
 *
 * 多实例：open id "camera"/"camera0" → 实例 0，"cameraN" → 实例 N。
 *   实例 N 默认映射 VI dev=pipe=N、chn=0，rkaiq 物理相机序号 = N；
 *   可用环境变量 DARKOS_CAMERA{N}_VI_DEV / _VI_PIPE / _VI_CHN 覆盖
 *   （如 DARKOS_CAMERA1_VI_DEV=2）。
 *
 * 说明：
 *   - 对上始终呈现 NV12（RK_FMT_YUV420SP），与编码器输入对齐；
 *   - 3A（rkaiq）已接入：start 时按 rkipc 流程初始化（对齐 SDK rkipc 的
 *     common/isp/rv1126b/isp.c：enumStaticMetas → preInit_scene →
 *     sysctl_init/prepare/start）。IQ 文件目录取环境变量
 *     DARKOS_IQ_FILE_DIR，未设置则依次探测发布包的 /oem/usr2/etc/iqfiles、
 *     /oem/usr/share/iqfiles、/etc/iqfiles、/usr/share/iqfiles；找不到或传感器未上线时降级为
 *     驱动默认调参（仅告警，不阻断采集）；
 *   - 采集回调路径为零拷贝：GetChnFrame 的 dma-buf fd 与虚拟地址在
 *     回调期间透出（frame->fd / frame->data），回调返回后
 *     ReleaseChnFrame——消费方须在回调内同步用完（MediaService
 *     即在回调内同步编码）。VI 行对齐与请求尺寸不一致的帧回退到
 *     内存拷贝路径；capture()（调试阻塞接口）始终走拷贝；
 *   - set_control/get_control（亮度/对比度/曝光/增益/白平衡）映射到
 *     rkaiq uAPI2，3A 未启动时返回 -ENOTSUP。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* pthread_setname_np（glibc 严格模式下需要） */
#endif

#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rk_comm_vi.h>
#include <rk_comm_venc.h>
#include <rk_comm_video.h>
#include <rk_comm_vo.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_vo.h>
#include <rkaiq/uAPI2/rk_aiq_user_api2_imgproc.h>
#include <rkaiq/uAPI2/rk_aiq_user_api2_sysctl.h>

#include "common/mpi_sys_guard.h"

#define RK_VI_BUF_COUNT 5 /* ISP 出图缓冲数（预览换手会瞬占 1~2 块，3 块会
                           * 缺血——板上实测 GetChnFrame 被拖到 60~120ms；
                           * rkipc 默认 3 是无预览场景的配法） */
#define RK_VI_GET_TIMEOUT_MS 1000
#define RK_CAM_MAX_INSTANCES 8
#define RK_PREVIEW_SEND_TIMEOUT_MS 100 /* 预览送帧超时（VO 层缓冲满则丢帧） */
#define RK_PREVIEW_VI_CHN_DEFAULT 5 /* 对齐 SDK：VI chn 5 专供本地显示 */
#define RK_PREVIEW_VI_WIDTH 1920
#define RK_PREVIEW_VI_HEIGHT 1080
#define RK_ENCODED_VENC_MAX_WIDTH 3840
#define RK_ENCODED_VENC_MAX_HEIGHT 2160

typedef struct rk_camera_priv {
    int idx;     /* 实例号（open id "cameraN" 的 N） */
    int vi_dev;  /* VI dev/pipe/chn：默认 dev=pipe=idx、chn=0，env 可覆盖 */
    int vi_pipe;
    int vi_chn;  /* raw/capture 通道，默认 0 */
    int encoded_vi_chn; /* 编码通道，RV1126B SDK 拓扑默认 3 */
    int type; /* camera_type_t：env DARKOS_CAMERA{idx}_TYPE=ir|white，默认 white */

    camera_format_t fmt; /* 请求并生效的格式（NV12） */

    int dev_enabled;
    int chn_enabled;
    int streaming;

    /* 编码视频输出通路（与普通回调采集互斥） */
    int encoded_dev_enabled;
    int encoded_chn_enabled;
    int encoded_venc_created;
    int encoded_bound;
    int encoded_streaming;
    int encoded_sys_acquired;
    int encoded_venc_chn;
    codec_format_t encoded_cfg;

    /* 本地预览：独立线程 cam{N}.prv 送帧（与采集线程换手解耦，见
     * preview_start 头注）。prv_mtx/prv_cond 保护 prv_frame 换手。 */
    volatile int preview_on;
    int preview_vo_layer;      /* VO 视频层层号（DARKOS_VO_LAYER，默认 0） */
    unsigned preview_err;      /* 预览送帧失败计数（日志节流） */
    pthread_t preview_thread;
    int preview_thread_on;
    pthread_mutex_t prv_mtx;
    pthread_cond_t prv_cond;
    MB_BLK prv_blk;          /* 换手信箱（filled=1 时所有权在预览线程） */
    volatile int prv_filled;
    /* 预览统计（每 10s 打点，评估 VO 通路性能用） */
    unsigned prv_sent;      /* 预览线程已送帧数 */
    unsigned prv_dropped;   /* 丢帧数（采集线程：信箱被占/池空） */
    uint64_t prv_send_ns;   /* SendFrame 耗时合计 */
    uint64_t prv_send_max;  /* SendFrame 耗时峰值 */
    uint64_t prv_stat_ns;   /* 上次统计时刻 */
    unsigned prv_pool_empty; /* 其中池空丢帧数 */
    int preview_rotation;   /* VO chn enRotation（DARKOS_PREVIEW_ROT 覆盖） */
    int preview_vo_send;    /* 0 = 预览线程不送 VO 只还帧（瓶颈隔离测量用，
                             * DARKOS_PREVIEW_VO_SEND=0） */
    MB_POOL prv_pool;         /* 预览拷贝缓冲池（6 块，preview_start 建） */
    int preview_fps;          /* 预览送帧节奏（DARKOS_PREVIEW_FPS，默认 15） */
    /* 编码路径的硬件直通预览：VI chn 5 -> VO layer/chn 0。 */
    int preview_vi_chn;
    int preview_vi_chn_enabled;
    int preview_bound;
    int preview_direct;

    uint8_t *nv12; /* 复用缓冲（capture 与行对齐回退路径用，回调期内有效） */
    size_t nv12_size;
    uint32_t seq;

    rk_aiq_sys_ctx_t *aiq_ctx; /* 3A 上下文，NULL 表示降级运行 */
    int sys_acquired;          /* 已持有进程级 SYS_Init 引用（见 common/mpi_sys_guard） */
    int32_t ctl_brightness;    /* 控制项缓存（rkaiq 无档位 getter，get 返回最近设置值） */
    int32_t ctl_contrast;
    int32_t ctl_exposure; /* <0 自动；否则手动曝光时间，单位 100µs */
    int32_t ctl_gain;     /* <=0 自动；否则手动增益倍数 */
    int32_t ctl_wb;       /* 0 手动 / 1 自动 */

    camera_frame_cb cb;
    void *cb_ctx;
    pthread_t thread;
    volatile int running;
} rk_camera_priv_t;

/* 编码路径在 CameraEncodedVideo 停止时也必须摘掉本地显示绑定。 */
static int rk_camera_preview_stop(camera_device_t *dev);

/* ---------------------------------------------------------------------------
 * 3A（rkaiq）：流程对齐 SDK rkipc common/isp/rv1126b/isp.c
 * ------------------------------------------------------------------------- */

/* IQ 文件目录：环境变量优先，其次按发布包、系统镜像的惯例探测。 */
static const char *rk_aiq_find_iq_dir(void) {
    static const char *candidates[] = {"/oem/usr2/etc/iqfiles", "/oem/usr/share/iqfiles",
                                       "/etc/iqfiles", "/usr/share/iqfiles"};
    const char *env = getenv("DARKOS_IQ_FILE_DIR");
    size_t i;

    if (env != NULL) {
        if (access(env, R_OK) == 0)
            return env;
        fprintf(stderr, "rk_camera: DARKOS_IQ_FILE_DIR=%s 不可读，按默认路径探测\n", env);
    }
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0)
            return candidates[i];
    }
    return NULL;
}

/* 启动 3A；任何一步失败都降级为无 3A 运行（仅告警），返回时 aiq_ctx 可能为 NULL */
static void rk_aiq_3a_start(rk_camera_priv_t *priv) {
    rk_aiq_static_info_t info;
    const char *iq_dir;

    memset(&info, 0, sizeof(info));
    rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(priv->idx, &info);
    if (info.sensor_info.phyId == -1) {
        fprintf(stderr, "rk_camera%d: 未探测到传感器，3A 不启动（驱动默认调参）\n",
                priv->idx);
        return;
    }
    iq_dir = rk_aiq_find_iq_dir();
    if (iq_dir == NULL) {
        fprintf(stderr,
                "rk_camera%d: 找不到 IQ 文件目录（可设 DARKOS_IQ_FILE_DIR），3A 不启动\n",
                priv->idx);
        return;
    }

    if (rk_aiq_uapi2_sysctl_preInit_scene(info.sensor_info.sensor_name, "normal", "day") < 0)
        fprintf(stderr, "rk_camera%d: preInit_scene failed (%s)\n", priv->idx,
                info.sensor_info.sensor_name);
    priv->aiq_ctx =
        rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iq_dir, NULL, NULL);
    if (priv->aiq_ctx == NULL) {
        fprintf(stderr, "rk_camera%d: aiq sysctl_init failed (%s, iq=%s)\n", priv->idx,
                info.sensor_info.sensor_name, iq_dir);
        return;
    }
    if (rk_aiq_uapi2_sysctl_prepare(priv->aiq_ctx, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) != 0) {
        fprintf(stderr, "rk_camera%d: aiq prepare failed\n", priv->idx);
        goto out_deinit;
    }
    /* raw 流归 rkaiq 还是 rockit 控制，由算法需求决定（对齐 rkipc） */
    RK_MPI_VI_CtrlRawStream(rk_aiq_uapi2_judgeNeedRawPipeCtrl(priv->aiq_ctx) ? 0 : 1);
    if (rk_aiq_uapi2_sysctl_start(priv->aiq_ctx) != 0) {
        fprintf(stderr, "rk_camera%d: aiq start failed\n", priv->idx);
        goto out_deinit;
    }
    fprintf(stderr, "rk_camera%d: 3A started, sensor=%s iq=%s\n", priv->idx,
            info.sensor_info.sensor_name, iq_dir);
    return;

out_deinit:
    rk_aiq_uapi2_sysctl_deinit(priv->aiq_ctx);
    priv->aiq_ctx = NULL;
}

static void rk_aiq_3a_stop(rk_camera_priv_t *priv) {
    if (priv->aiq_ctx == NULL)
        return;
    rk_aiq_uapi2_sysctl_stop(priv->aiq_ctx, false);
    rk_aiq_uapi2_sysctl_deinit(priv->aiq_ctx);
    priv->aiq_ctx = NULL;
}

/* ---------------------------------------------------------------------------
 * 采集线程
 * ------------------------------------------------------------------------- */

static int ensure_nv12_buf(rk_camera_priv_t *priv) {
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

/* 用 VI 帧填 camera_frame。
 * force_copy=0（采集回调路径）：优先零拷贝——VI 出帧无压缩且行对齐与请求一致时，
 *   直接透出 dma-buf fd 与虚拟地址，仅在持帧期间（ReleaseChnFrame 前）有效；
 *   条件不满足时回退拷入复用缓冲。
 * force_copy=1（capture 路径）：始终拷入复用缓冲，返回后帧仍有效（至下次调用）。 */
static int fill_frame_from_vi(rk_camera_priv_t *priv, VIDEO_FRAME_INFO_S *vi_frame,
                              camera_frame_t *frame, int force_copy) {
    const VIDEO_FRAME_S *vf = &vi_frame->stVFrame;
    size_t expect = (size_t)priv->fmt.width * priv->fmt.height * 3 / 2;
    const void *data;
    int fd;

    if ((size_t)vf->u32Width * vf->u32Height * 3 / 2 > expect)
        return -ENOMEM;

    frame->timestamp_ns = vf->u64PTS * 1000ull; /* MPI PTS 单位 us */
    frame->width = priv->fmt.width;
    frame->height = priv->fmt.height;
    frame->pixel_format = CAMERA_PIX_FMT_NV12;
    frame->stride = priv->fmt.width;
    frame->size = (uint32_t)expect;
    frame->priv = NULL;

    if (!force_copy && vf->u32Width == priv->fmt.width && vf->u32Height == priv->fmt.height &&
        vf->u32VirWidth == priv->fmt.width && vf->u32VirHeight == priv->fmt.height &&
        vf->enCompressMode == COMPRESS_MODE_NONE) {
        fd = RK_MPI_MB_Handle2Fd(vf->pMbBlk);
        data = RK_MPI_MB_Handle2VirAddr(vf->pMbBlk);
        if (fd >= 0 && data != NULL) {
            frame->index = priv->seq++;
            frame->fd = fd;
            frame->data = (void *)data;
            frame->priv = (void *)vf->pMbBlk; /* MB_BLK 透出：同厂商 codec 可直送 VENC */
            return 0;
        }
    }

    /* 拷贝路径：行对齐不一致时按 stride 逐行打包 */
    if (ensure_nv12_buf(priv) != 0)
        return -ENOMEM;
    data = RK_MPI_MB_Handle2VirAddr(vf->pMbBlk);
    if (data == NULL)
        return -EIO;
    if (vf->u32VirWidth == vf->u32Width) {
        memcpy(priv->nv12, data, (size_t)vf->u32Width * vf->u32Height * 3 / 2);
    } else {
        const uint8_t *src = (const uint8_t *)data;
        uint8_t *dst = priv->nv12;
        uint32_t row;
        for (row = 0; row < vf->u32Height * 3 / 2; row++) {
            memcpy(dst, src, vf->u32Width);
            src += vf->u32VirWidth;
            dst += vf->u32Width;
        }
    }
    frame->index = priv->seq++;
    frame->fd = -1;
    frame->data = priv->nv12;
    return 0;
}

/* 采集线程：持帧回调（零拷贝帧须在回调内同步用完），回调返回即还帧 */
static void *rk_capture_thread(void *arg) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)arg;
    unsigned err_count = 0;

    char tname[16];
    snprintf(tname, sizeof(tname), "cam%d.cap", priv->idx);
    tname[sizeof(tname) - 1] = '\0';
    pthread_setname_np(pthread_self(), tname);



    while (priv->running) {
        VIDEO_FRAME_INFO_S vi_frame;
        camera_frame_t frame;
        int rc;
        struct timespec t0, t1, t2;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        memset(&vi_frame, 0, sizeof(vi_frame));
        if (RK_MPI_VI_GetChnFrame(priv->vi_pipe, priv->vi_chn, &vi_frame,
                                  RK_VI_GET_TIMEOUT_MS) != 0)
            continue; /* 超时，继续等 */
        clock_gettime(CLOCK_MONOTONIC, &t1);

        /* 本地预览：采集线程就地拷贝进预览池块（~4ms，CACHED），VI 帧随即
         * 归还——VI 句柄绝不出采集线程（跨线程 Release/信箱压帧都会拖垮
         * GetChnFrame，板上实测）。池块换手给预览线程：信箱被占就丢旧块。 */
        if (priv->preview_on) {
            const VIDEO_FRAME_S *svf = &vi_frame.stVFrame;
            MB_BLK blk =
                RK_MPI_MB_GetMB(priv->prv_pool,
                                (RK_U32)priv->fmt.width * priv->fmt.height * 3 / 2, RK_FALSE);
            if (blk != NULL) {
                void *src_vir = RK_MPI_MB_Handle2VirAddr(svf->pMbBlk);
                void *dst_vir = RK_MPI_MB_Handle2VirAddr(blk);
                if (src_vir != NULL && dst_vir != NULL) {
                    /* NV12：Y + UV 两段；行对齐不一致时逐行打包 */
                    const uint8_t *sp = (const uint8_t *)src_vir;
                    uint8_t *dp = (uint8_t *)dst_vir;
                    uint32_t row, rows = svf->u32Height * 3 / 2;
                    if (svf->u32VirWidth == svf->u32Width &&
                        svf->u32VirHeight == svf->u32Height) {
                        memcpy(dst_vir, src_vir,
                               (size_t)svf->u32Width * svf->u32Height * 3 / 2);
                    } else {
                        for (row = 0; row < rows; row++) {
                            memcpy(dp, sp, svf->u32Width);
                            sp += svf->u32VirWidth;
                            dp += svf->u32Width;
                        }
                    }
                    RK_MPI_SYS_MmzFlushCache(blk, RK_FALSE); /* CACHED 池块 */
                }
                pthread_mutex_lock(&priv->prv_mtx);
                if (!priv->prv_filled) {
                    priv->prv_blk = blk;
                    priv->prv_filled = 1;
                    pthread_cond_signal(&priv->prv_cond);
                } else {
                    priv->prv_dropped++; /* 预览没消费完，丢最新帧 */
                    RK_MPI_MB_ReleaseMB(blk);
                }
                pthread_mutex_unlock(&priv->prv_mtx);
            } else {
                priv->prv_dropped++;
                priv->prv_pool_empty++;
            }
        }

        rc = fill_frame_from_vi(priv, &vi_frame, &frame, 0);
        if (rc == 0 && priv->cb != NULL)
            priv->cb(priv->cb_ctx, &frame);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        RK_MPI_VI_ReleaseChnFrame(priv->vi_pipe, priv->vi_chn, &vi_frame);

#if 0
        /* 采集性能统计（10s 打点，定位预览对主链路的拖累用） */
        unsigned cap_n = 0;
        uint64_t cap_get_ns = 0, cap_cb_ns = 0, cap_stat_ns = 0, cap_max_cb = 0;

        /* 采集统计：10s 打点（capture fps / GetChnFrame 与回调耗时均值） */
        cap_n++;
        cap_get_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                      (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        uint64_t cb_ns = (uint64_t)(t2.tv_sec - t1.tv_sec) * 1000000000ull +
                         (uint64_t)(t2.tv_nsec - t1.tv_nsec);
        cap_cb_ns += cb_ns;
        if (cb_ns > cap_max_cb)
            cap_max_cb = cb_ns;
        uint64_t now = (uint64_t)t2.tv_sec * 1000000000ull + t2.tv_nsec;
        if (cap_stat_ns == 0)
            cap_stat_ns = now;
        if (now - cap_stat_ns >= 10ull * 1000 * 1000 * 1000) {
            double span = (double)(now - cap_stat_ns) / 1e9;
            fprintf(stderr,
                    "rk_camera%d: capture fps=%.1f getframe=%.1fms cb=%.1fms(峰值%llums)\n",
                    priv->idx, cap_n / span, (double)cap_get_ns / cap_n / 1e6,
                    (double)cap_cb_ns / cap_n / 1e6,
                    (unsigned long long)(cap_max_cb / 1000000));
            cap_n = 0;
            cap_get_ns = 0;
            cap_cb_ns = 0;
            cap_max_cb = 0;
            cap_stat_ns = now;
        }
#endif

        if (rc != 0 && ++err_count % 100 == 1)
            fprintf(stderr, "rk_camera%d: dropped %u bad frames (last rc=%d)\n", priv->idx,
                    err_count, rc);
    }
    return NULL;
}

/* capture（调试阻塞接口）用：拷入复用缓冲后还帧，帧在下次调用前有效 */
static int grab_one_frame(rk_camera_priv_t *priv, camera_frame_t *frame) {
    VIDEO_FRAME_INFO_S vi_frame;
    int rc;

    memset(&vi_frame, 0, sizeof(vi_frame));
    if (RK_MPI_VI_GetChnFrame(priv->vi_pipe, priv->vi_chn, &vi_frame, RK_VI_GET_TIMEOUT_MS) !=
        0)
        return -ETIMEDOUT;
    rc = fill_frame_from_vi(priv, &vi_frame, frame, 1);
    RK_MPI_VI_ReleaseChnFrame(priv->vi_pipe, priv->vi_chn, &vi_frame);
    return rc;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int rk_camera_get_capabilities(camera_device_t *dev, camera_caps_t *caps) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    rk_aiq_static_info_t info;

    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->min_width = 128;
    caps->min_height = 128;
    caps->max_width = 4096;
    caps->max_height = 4096;
    caps->supported_formats = CAMERA_CAPS_FMT_NV12;
    caps->camera_type = priv->type;

    /* sensor 名经 rkaiq 静态信息探测（不依赖 VI/SYS 启动）；
     * sensor 支持的分辨率档位由 dtb/IQ 决定、MPI 无通用枚举，
     * res_count 置 0 按连续范围表达（max=4096 为 VI 能力上限） */
    memset(&info, 0, sizeof(info));
    rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(priv->idx, &info);
    if (info.sensor_info.phyId != -1) {
        snprintf(caps->sensor_name, sizeof(caps->sensor_name), "%s",
                 info.sensor_info.sensor_name);
    }
    return 0;
}

static int rk_camera_set_format(camera_device_t *dev, const camera_format_t *fmt) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->pixel_format != CAMERA_PIX_FMT_NV12)
        return -EINVAL; /* MPI VI 对上只呈现 NV12 */
    if (priv->streaming || priv->encoded_streaming)
        return -EBUSY;

    priv->fmt = *fmt;
    return 0;
}

static int rk_camera_get_format(camera_device_t *dev, camera_format_t *fmt) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = priv->fmt;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 编码输出：后端使用 VI -> VENC 直连实现
 * ------------------------------------------------------------------------- */

static void rk_camera_encoded_cleanup(rk_camera_priv_t *priv) {
    if (priv->encoded_bound) {
        MPP_CHN_S src = {RK_ID_VI, priv->vi_pipe, priv->encoded_vi_chn};
        MPP_CHN_S dst = {RK_ID_VENC, priv->encoded_venc_chn, 0};
        RK_MPI_SYS_UnBind(&src, &dst);
        priv->encoded_bound = 0;
    }
    if (priv->encoded_venc_created) {
        RK_MPI_VENC_StopRecvFrame(priv->encoded_venc_chn);
        RK_MPI_VENC_DestroyChn(priv->encoded_venc_chn);
        priv->encoded_venc_created = 0;
    }
    if (priv->encoded_chn_enabled) {
        RK_MPI_VI_DisableChn(priv->vi_pipe, priv->encoded_vi_chn);
        priv->encoded_chn_enabled = 0;
    }
    if (priv->encoded_dev_enabled) {
        RK_MPI_VI_DisableDev(priv->vi_dev);
        priv->encoded_dev_enabled = 0;
    }
    rk_aiq_3a_stop(priv);
    if (priv->encoded_sys_acquired) {
        rk_mpi_sys_release();
        priv->encoded_sys_acquired = 0;
    }
    priv->encoded_streaming = 0;
}

static int rk_camera_encoded_start(camera_device_t *dev,
                                   const codec_format_t *config) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    VI_CHN_ATTR_S chn_attr;
    VENC_CHN_ATTR_S venc_attr;
    VENC_RECV_PIC_PARAM_S recv;
    MPP_CHN_S src;
    MPP_CHN_S dst;
    uint32_t w;
    uint32_t h;
    uint32_t fps;
    uint32_t bitrate;
    uint32_t gop;

    if (config == NULL)
        return -EINVAL;
    if (priv->streaming || priv->encoded_streaming)
        return -EBUSY;
    if (config->codec != CODEC_ID_H264 ||
        config->pixel_format != CAMERA_PIX_FMT_NV12)
        return -EINVAL;

    w = config->width;
    h = config->height;
    fps = config->fps != 0 ? config->fps : 30;
    bitrate = config->bitrate_bps != 0 ? config->bitrate_bps : 8000000;
    gop = config->gop != 0 ? config->gop : fps;
    if (w < 128 || h < 128 || w > RK_ENCODED_VENC_MAX_WIDTH ||
        h > RK_ENCODED_VENC_MAX_HEIGHT)
        return -EINVAL;

    priv->encoded_cfg = *config;
    priv->encoded_cfg.fps = fps;
    priv->encoded_cfg.bitrate_bps = bitrate;
    priv->encoded_cfg.gop = gop;
    priv->fmt = (camera_format_t){w, h, config->pixel_format, fps};

    /* 与普通 Camera 路径相同：3A 先启动，随后取得进程级 SYS 引用。 */
    rk_aiq_3a_start(priv);
    if (rk_mpi_sys_acquire() != 0)
        goto fail;
    priv->encoded_sys_acquired = 1;

    memset(&dev_attr, 0, sizeof(dev_attr));
    if (RK_MPI_VI_GetDevAttr(priv->vi_dev, &dev_attr) != 0) {
        if (RK_MPI_VI_SetDevAttr(priv->vi_dev, &dev_attr) != 0) {
            fprintf(stderr, "rk_camera%d: encoded VI SetDevAttr failed\n", priv->idx);
            goto fail;
        }
    }
    if (RK_MPI_VI_GetDevIsEnable(priv->vi_dev) != 0) {
        if (RK_MPI_VI_EnableDev(priv->vi_dev) != 0) {
            fprintf(stderr, "rk_camera%d: encoded VI EnableDev failed\n", priv->idx);
            goto fail;
        }
        memset(&bind_pipe, 0, sizeof(bind_pipe));
        bind_pipe.u32Num = 1;
        bind_pipe.PipeId[0] = priv->vi_pipe;
        if (RK_MPI_VI_SetDevBindPipe(priv->vi_dev, &bind_pipe) != 0) {
            fprintf(stderr, "rk_camera%d: encoded VI SetDevBindPipe failed\n", priv->idx);
            RK_MPI_VI_DisableDev(priv->vi_dev);
            goto fail;
        }
    }
    priv->encoded_dev_enabled = 1;

    /* 对齐 SDK rkipc 的 rkipc_vi_dev_init：扩展通道 4/5 必须先切到 VI
     * 扩展通道模式，之后才能在编码通道运行时启用显示通道 5。 */
    VI_PARAM_MOD_S mod_param;
    memset(&mod_param, 0, sizeof(mod_param));
    mod_param.enViModType = VI_EXT_CHN_MODE;
    mod_param.stExtChnParam.extChn[4] = 1;
    mod_param.stExtChnParam.extChn[5] = 1;
    if (RK_MPI_VI_SetModParam(&mod_param) != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: encoded VI SetModParam failed\n", priv->idx);
        goto fail;
    }

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.stIspOpt.u32BufCount = RK_VI_BUF_COUNT;
    chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn_attr.stIspOpt.stMaxSize.u32Width = w;
    chn_attr.stIspOpt.stMaxSize.u32Height = h;
    chn_attr.stSize.u32Width = w;
    chn_attr.stSize.u32Height = h;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.u32Depth = 0; /* Bind 路径不保留用户帧队列 */
    chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    chn_attr.stFrameRate.s32SrcFrameRate = fps;
    chn_attr.stFrameRate.s32DstFrameRate = fps;
    if (RK_MPI_VI_SetChnAttr(priv->vi_pipe, priv->encoded_vi_chn, &chn_attr) != 0) {
        fprintf(stderr, "rk_camera%d: encoded VI SetChnAttr failed (%ux%u)\n",
                priv->idx, w, h);
        goto fail;
    }
    if (RK_MPI_VI_EnableChn(priv->vi_pipe, priv->encoded_vi_chn) != 0) {
        fprintf(stderr, "rk_camera%d: encoded VI EnableChn failed\n", priv->idx);
        goto fail;
    }
    priv->encoded_chn_enabled = 1;

    memset(&venc_attr, 0, sizeof(venc_attr));
    venc_attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    venc_attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    venc_attr.stVencAttr.u32Profile = H264E_PROFILE_MAIN;
    venc_attr.stVencAttr.u32MaxPicWidth = w;
    venc_attr.stVencAttr.u32MaxPicHeight = h;
    venc_attr.stVencAttr.u32PicWidth = w;
    venc_attr.stVencAttr.u32PicHeight = h;
    venc_attr.stVencAttr.u32VirWidth = w;
    venc_attr.stVencAttr.u32VirHeight = h;
    venc_attr.stVencAttr.u32StreamBufCnt = 4;
    venc_attr.stVencAttr.u32BufSize = (RK_U32)((uint64_t)w * h * 3 / 2);
    venc_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    venc_attr.stRcAttr.stH264Cbr.u32Gop = gop;
    venc_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = fps;
    venc_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    venc_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = fps;
    venc_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    venc_attr.stRcAttr.stH264Cbr.u32BitRate = bitrate / 1000;
    venc_attr.stRcAttr.stH264Cbr.u32StatTime = 1;
    venc_attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    if (RK_MPI_VENC_CreateChn(priv->encoded_venc_chn, &venc_attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: encoded VENC CreateChn failed (%ux%u)\n",
                priv->idx, w, h);
        goto fail;
    }
    priv->encoded_venc_created = 1;
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    if (RK_MPI_VENC_StartRecvFrame(priv->encoded_venc_chn, &recv) != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: encoded VENC StartRecvFrame failed\n", priv->idx);
        goto fail;
    }

    src = (MPP_CHN_S){RK_ID_VI, priv->vi_pipe, priv->encoded_vi_chn};
    dst = (MPP_CHN_S){RK_ID_VENC, priv->encoded_venc_chn, 0};
    if (RK_MPI_SYS_Bind(&src, &dst) != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: encoded source bind failed\n", priv->idx);
        goto fail;
    }
    priv->encoded_bound = 1;
    priv->encoded_streaming = 1;
    fprintf(stderr, "rk_camera%d: encoded camera path VI(%d,%d,%d)->VENC(%d) bound, %ux%u@%u bitrate=%u\n",
            priv->idx, priv->vi_dev, priv->vi_pipe, priv->encoded_vi_chn,
            priv->encoded_venc_chn, w, h, fps, bitrate);
    return 0;

fail:
    rk_camera_encoded_cleanup(priv);
    return -EIO;
}

static int rk_camera_encoded_get_packet(camera_device_t *dev,
                                        codec_buffer_t *packet,
                                        int timeout_ms) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    VENC_STREAM_S stream;
    VENC_PACK_S packs[8];
    uint32_t i;
    uint32_t copied = 0;
    int rc;

    if (packet == NULL || packet->data == NULL || packet->size == 0)
        return -EINVAL;
    if (!priv->encoded_streaming)
        return -EPIPE;

    memset(&stream, 0, sizeof(stream));
    memset(packs, 0, sizeof(packs));
    packet->offset = 0;
    packet->flags = 0;
    stream.pstPack = packs;
    rc = RK_MPI_VENC_GetStream(priv->encoded_venc_chn, &stream, timeout_ms);
    if (rc != RK_SUCCESS)
        return -ETIMEDOUT;
    if (stream.u32PackCount == 0 || stream.u32PackCount > 8) {
        RK_MPI_VENC_ReleaseStream(priv->encoded_venc_chn, &stream);
        return -EIO;
    }

    for (i = 0; i < stream.u32PackCount; ++i) {
        VENC_PACK_S *pack = &packs[i];
        const uint8_t *data;
        if (copied > packet->size || pack->u32Len > packet->size - copied) {
            RK_MPI_VENC_ReleaseStream(priv->encoded_venc_chn, &stream);
            return -ENOSPC;
        }
        data = (const uint8_t *)RK_MPI_MB_Handle2VirAddr(pack->pMbBlk);
        if (data == NULL) {
            RK_MPI_VENC_ReleaseStream(priv->encoded_venc_chn, &stream);
            return -EIO;
        }
        memcpy((uint8_t *)packet->data + copied, data + pack->u32Offset,
               pack->u32Len);
        copied += pack->u32Len;
        if (pack->DataType.enH264EType == H264E_NALU_IDRSLICE ||
            pack->DataType.enH264EType == H264E_NALU_ISLICE)
            packet->flags |= CODEC_BUFFER_FLAG_KEYFRAME;
        if (i == 0)
            packet->timestamp_ns = pack->u64PTS * 1000ull;
    }
    RK_MPI_VENC_ReleaseStream(priv->encoded_venc_chn, &stream);
    packet->size = copied;
    return 0;
}

static int rk_camera_encoded_stop(camera_device_t *dev) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    if (!priv->encoded_streaming && !priv->encoded_sys_acquired &&
        !priv->encoded_venc_created)
        return 0;
    rk_camera_preview_stop(dev);
    rk_camera_encoded_cleanup(priv);
    return 0;
}

static int rk_camera_start(camera_device_t *dev) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    VI_DEV_ATTR_S dev_attr;
    VI_DEV_BIND_PIPE_S bind_pipe;
    VI_CHN_ATTR_S chn_attr;
    int rc;
    int ret = -EIO;

    if (priv->streaming || priv->encoded_streaming)
        return -EBUSY;

    /* 3A 先行（对齐 rkipc：rk_isp_init 在 RK_MPI_SYS_Init 之前）；
     * 失败仅降级，不阻断采集 */
    rk_aiq_3a_start(priv);

    if (rk_mpi_sys_acquire() != 0)
        goto out_aiq;
    priv->sys_acquired = 1;

    /* dev：未配置则按默认配置（对齐 SDK rkipc 流程） */
    memset(&dev_attr, 0, sizeof(dev_attr));
    if (RK_MPI_VI_GetDevAttr(priv->vi_dev, &dev_attr) != 0) {
        if (RK_MPI_VI_SetDevAttr(priv->vi_dev, &dev_attr) != 0) {
            fprintf(stderr, "rk_camera%d: RK_MPI_VI_SetDevAttr failed\n", priv->idx);
            goto out_aiq;
        }
    }
    if (RK_MPI_VI_GetDevIsEnable(priv->vi_dev) != 0) {
        if (RK_MPI_VI_EnableDev(priv->vi_dev) != 0) {
            fprintf(stderr, "rk_camera%d: RK_MPI_VI_EnableDev failed\n", priv->idx);
            goto out_aiq;
        }
        memset(&bind_pipe, 0, sizeof(bind_pipe));
        bind_pipe.u32Num = 1;
        bind_pipe.PipeId[0] = priv->vi_pipe;
        if (RK_MPI_VI_SetDevBindPipe(priv->vi_dev, &bind_pipe) != 0) {
            fprintf(stderr, "rk_camera%d: RK_MPI_VI_SetDevBindPipe failed\n", priv->idx);
            RK_MPI_VI_DisableDev(priv->vi_dev);
            goto out_aiq;
        }
    }
    priv->dev_enabled = 1;

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.stIspOpt.u32BufCount = RK_VI_BUF_COUNT;
    chn_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn_attr.stIspOpt.stMaxSize.u32Width = priv->fmt.width;
    chn_attr.stIspOpt.stMaxSize.u32Height = priv->fmt.height;
    chn_attr.stSize.u32Width = priv->fmt.width;
    chn_attr.stSize.u32Height = priv->fmt.height;
    chn_attr.enPixelFormat = RK_FMT_YUV420SP;
    chn_attr.u32Depth = 1;
    chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    chn_attr.enCompressMode = COMPRESS_MODE_NONE;
    if (RK_MPI_VI_SetChnAttr(priv->vi_pipe, priv->vi_chn, &chn_attr) != 0) {
        fprintf(stderr, "rk_camera%d: RK_MPI_VI_SetChnAttr failed (%ux%u)\n", priv->idx,
                priv->fmt.width, priv->fmt.height);
        goto out_aiq;
    }
    if (RK_MPI_VI_EnableChn(priv->vi_pipe, priv->vi_chn) != 0) {
        fprintf(stderr, "rk_camera%d: RK_MPI_VI_EnableChn failed\n", priv->idx);
        goto out_aiq;
    }
    priv->chn_enabled = 1;

    priv->streaming = 1;
    if (priv->cb != NULL) {
        priv->running = 1;
        rc = pthread_create(&priv->thread, NULL, rk_capture_thread, priv);
        if (rc != 0) {
            priv->running = 0;
            priv->streaming = 0;
            ret = -rc;
            goto out_aiq;
        }
    }
    return 0;

out_aiq:
    /* 按使能逆序回收：chn → dev → SYS 引用 → 3A（与 stop 的回收顺序一致），
     * 避免 EnableChn/pthread_create 失败后 VI dev 残留使能态 */
    if (priv->chn_enabled) {
        RK_MPI_VI_DisableChn(priv->vi_pipe, priv->vi_chn);
        priv->chn_enabled = 0;
    }
    if (priv->dev_enabled) {
        RK_MPI_VI_DisableDev(priv->vi_dev);
        priv->dev_enabled = 0;
    }
    if (priv->sys_acquired) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
    }
    rk_aiq_3a_stop(priv);
    return ret;
}

/* ---------------------------------------------------------------------------
 * 本地预览：采集线程就地拷贝进预览 MB 池，独立线程 cam{N}.prv 定速送 VO
 *
 * 演化史（板上实测依据）：
 * - VI 显示通道 + SYS_Bind 方案：采集中途新开 VI 通道 EnableChn 报
 *   RK_ERR_BUSY（chn2/chn5 同，rkipc 全是启动期一次建齐），弃用；
 * - VI 帧句柄换手给预览线程直送：VO 持帧期间 VI 缓冲被长占，
 *   GetChnFrame 被拖到 60~120ms（capture 30→13fps，RTSP 同步降帧），弃用；
 * - 现方案：采集线程把帧拷进预览自有 MB 池（CACHED，~4ms/帧，拷完即还
 *   VI 帧——VI 句柄绝不出采集线程），池块换手给 cam{N}.prv（信箱被占就丢
 *   旧块，绝不阻塞采集）；预览线程按 preview_fps（默认 15，
 *   DARKOS_PREVIEW_FPS 覆盖）恒定节奏 SendFrame——VO+RGA 回收周期实测
 *   40~90ms 抖动，30fps 直送在 VO 侧积压，稳的 15fps 观感更好。
 *
 * 旋转：横屏 sensor → 竖屏 panel 由 VO 通道旋转承担（enRotation +
 * 层 splice RGA，对齐 rkipc rv1126b_dv media_ctrl/photo_mode 序列）。
 * 方向语义：RK 惯例 ROTATION_90 为顺时针，逆时针 90° 用 ROTATION_270
 * （方向以屏幕实测为准，反了改 ROTATION_90 一行即可；
 * DARKOS_PREVIEW_ROT=0/90/270 可覆盖，性能 A/B 用）。
 * 约束：预览依赖回调式采集线程（MediaService 路径恒满足）；display HAL
 * 须已由上层 start（VO 层已建）。
 * ------------------------------------------------------------------------- */

/* 全局 env 整型读取（DARKOS_VO_LAYER / DARKOS_PREVIEW_ROT 等），非法值回落默认 */
static int rk_env_int(const char *name, int dflt) {
    const char *s = getenv(name);
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0')
        return dflt;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 1024)
        return dflt;
    return (int)v;
}

/* VO 视频层层号：DARKOS_VO_LAYER 覆盖，默认 0（与 display HAL 同 env 约定） */
static int rk_vo_layer(void) {
    return rk_env_int("DARKOS_VO_LAYER", 0);
}

/* 编码输出同时打开本地硬件预览。显示层已由 Display HAL 建好，这里只
 * 配置 VI 的显示通道并做 SYS_Bind，不把 VO 设备生命周期重复到 Camera HAL。 */
static int rk_camera_direct_preview_start(camera_device_t *dev, uint32_t panel_width,
                                          uint32_t panel_height) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    VI_CHN_ATTR_S vi_attr;
    VO_CHN_ATTR_S vo_attr;
    MPP_CHN_S src;
    MPP_CHN_S dst;
    uint32_t source_width = panel_width;
    uint32_t source_height = panel_height;
    int rc;

    priv->preview_vo_layer = rk_vo_layer();
    priv->preview_vi_chn = rk_env_int("DARKOS_PREVIEW_VI_CHN", RK_PREVIEW_VI_CHN_DEFAULT);
    if (priv->preview_vi_chn == priv->encoded_vi_chn) {
        fprintf(stderr, "rk_camera%d: preview VI chn %d conflicts with encoded chn %d\n",
                priv->idx, priv->preview_vi_chn, priv->encoded_vi_chn);
        return -EINVAL;
    }

    /* 竖屏 MIPI panel 的旋转和缩放交给 VO/RGA，源帧保持 NV12。 */
    priv->preview_rotation = rk_env_int("DARKOS_PREVIEW_ROT", ROTATION_270);
    if (priv->preview_rotation != ROTATION_0 && priv->preview_rotation != ROTATION_90 &&
        priv->preview_rotation != ROTATION_180 && priv->preview_rotation != ROTATION_270)
        priv->preview_rotation = ROTATION_270;
    /* 竖屏 panel 采用 270° 旋转时，VI 先输出旋转前的 800x480 横帧。
     * 这样 RGA 只做旋转，不再把 1920x1080 缩放到 480x800，减少显示延迟。 */
    if (priv->preview_rotation == ROTATION_90 || priv->preview_rotation == ROTATION_270) {
        source_width = panel_height;
        source_height = panel_width;
    }
    if (source_width < 128 || source_height < 128) {
        source_width = RK_PREVIEW_VI_WIDTH;
        source_height = RK_PREVIEW_VI_HEIGHT;
    }
    if (RK_MPI_VO_SetLayerSpliceMode(priv->preview_vo_layer, VO_SPLICE_MODE_RGA) != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: direct preview SetLayerSpliceMode(RGA) failed\n",
                priv->idx);
        return -EIO;
    }
    memset(&vo_attr, 0, sizeof(vo_attr));
    vo_attr.stRect = (RECT_S){0, 0, panel_width, panel_height};
    vo_attr.bDeflicker = RK_FALSE;
    vo_attr.u32Priority = 1;
    vo_attr.enRotation = (ROTATION_E)priv->preview_rotation;
    if (RK_MPI_VO_SetChnAttr(priv->preview_vo_layer, 0, &vo_attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: direct preview VO chn setup failed\n", priv->idx);
        return -EIO;
    }

    memset(&vi_attr, 0, sizeof(vi_attr));
    vi_attr.stIspOpt.u32BufCount = 3;
    vi_attr.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    vi_attr.stIspOpt.stMaxSize.u32Width = source_width;
    vi_attr.stIspOpt.stMaxSize.u32Height = source_height;
    vi_attr.stSize.u32Width = source_width;
    vi_attr.stSize.u32Height = source_height;
    vi_attr.enPixelFormat = RK_FMT_YUV420SP;
    vi_attr.u32Depth = 0;
    vi_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    vi_attr.enCompressMode = COMPRESS_MODE_NONE;
    vi_attr.stFrameRate.s32SrcFrameRate = priv->encoded_cfg.fps;
    vi_attr.stFrameRate.s32DstFrameRate = priv->encoded_cfg.fps;
    rc = RK_MPI_VI_SetChnAttr(priv->vi_pipe, priv->preview_vi_chn, &vi_attr);
    if (rc != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: direct preview VI SetChnAttr failed (%d)\n",
                priv->idx, rc);
        return -EIO;
    }
    /* 当前固件的 VPSS 扩展通道默认 scl mode 为 0，必须显式选择有效算法，
     * 否则 chn 5 的 STREAM_ON 会以“scale down algo invalid”失败。固件的
     * 实际参数映射中 AVS 枚举值 2 对应可用的扩展通道缩放模式。 */
    rc = RK_MPI_VI_SetChnSclMode(priv->vi_pipe, priv->preview_vi_chn,
                                 VI_CHN_SCL_AVS_ALGO);
    if (rc != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: direct preview VI SetChnSclMode failed (%d)\n",
                priv->idx, rc);
        return -EIO;
    }
    rc = RK_MPI_VI_EnableChn(priv->vi_pipe, priv->preview_vi_chn);
    if (rc != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: direct preview VI EnableChn failed (%d)\n",
                priv->idx, rc);
        return -EIO;
    }
    priv->preview_vi_chn_enabled = 1;

    src = (MPP_CHN_S){RK_ID_VI, priv->vi_pipe, priv->preview_vi_chn};
    dst = (MPP_CHN_S){RK_ID_VO, priv->preview_vo_layer, 0};
    rc = RK_MPI_SYS_Bind(&src, &dst);
    if (rc != RK_SUCCESS) {
        fprintf(stderr, "rk_camera%d: direct preview VI->VO bind failed (%d)\n",
                priv->idx, rc);
        RK_MPI_VI_DisableChn(priv->vi_pipe, priv->preview_vi_chn);
        priv->preview_vi_chn_enabled = 0;
        return -EIO;
    }
    priv->preview_bound = 1;
    priv->preview_direct = 1;
    priv->preview_on = 1;
    fprintf(stderr,
            "rk_camera%d: direct preview VI(%d,%d,%d)->VO(layer=%d,chn=0) bound, "
            "source=%ux%u panel=%ux%u rotation=%d\n",
            priv->idx, priv->vi_dev, priv->vi_pipe, priv->preview_vi_chn,
            priv->preview_vo_layer, source_width, source_height,
            panel_width, panel_height, priv->preview_rotation);
    return 0;
}

/* 预览送帧线程：取信箱池块 → 节奏睡眠 → SendFrame → 还池块。
 * 按 preview_fps 恒定节奏送帧（VO+RGA 回收周期实测 40~90ms 抖动，30fps
 * 直送会在 VO 侧积压；稳的 15fps 比抖动的高帧率观感好——板上实测）。
 * 每 10s 打点性能统计 */
static void *rk_preview_thread(void *arg) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)arg;
    char tname[16];
    int64_t period_ns = 1000000000ll / (priv->preview_fps > 0 ? priv->preview_fps : 15);
    int64_t next_send = 0;

    snprintf(tname, sizeof(tname), "cam%d.prv", priv->idx);
    tname[sizeof(tname) - 1] = '\0';
    pthread_setname_np(pthread_self(), tname);

    for (;;) {
        MB_BLK blk;
        struct timespec t0, t1;

        pthread_mutex_lock(&priv->prv_mtx);
        while (!priv->prv_filled && priv->preview_on)
            pthread_cond_wait(&priv->prv_cond, &priv->prv_mtx);
        if (!priv->preview_on) {
            pthread_mutex_unlock(&priv->prv_mtx);
            break;
        }
        blk = priv->prv_blk;
        priv->prv_filled = 0;
        pthread_mutex_unlock(&priv->prv_mtx);

        /* 节奏控制：距上次送帧不足一个周期就先睡（信箱里的旧块会被更新的
         * 覆盖，睡醒取到的是最新帧；VI 句柄已不在此处，睡眠无侧害） */
        if (next_send > 0) {
            struct timespec nw;
            clock_gettime(CLOCK_MONOTONIC, &nw);
            int64_t wait_ns =
                next_send - ((int64_t)nw.tv_sec * 1000000000ll + nw.tv_nsec);
            if (wait_ns > 0) {
                struct timespec ts = {wait_ns / 1000000000ll, wait_ns % 1000000000ll};
                nanosleep(&ts, NULL);
            }
        }

        /* 旋转由 VO 通道 splice（RGA）承担：NV12 直送，VO 侧转 90° 并缩放
         * 到 panel。SendFrame 阻塞最多 100ms，超时就丢 */
        VIDEO_FRAME_INFO_S out;
        memset(&out, 0, sizeof(out));
        out.stVFrame.pMbBlk = blk;
        out.stVFrame.u32Width = priv->fmt.width;
        out.stVFrame.u32Height = priv->fmt.height;
        out.stVFrame.u32VirWidth = priv->fmt.width; /* 池块已按行打包 */
        out.stVFrame.u32VirHeight = priv->fmt.height;
        out.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
        out.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (priv->preview_vo_send &&
            RK_MPI_VO_SendFrame(priv->preview_vo_layer, 0, &out,
                                RK_PREVIEW_SEND_TIMEOUT_MS) != 0 &&
            ++priv->preview_err % 100 == 1)
            fprintf(stderr, "rk_camera%d: preview SendFrame failed %u times\n", priv->idx,
                    priv->preview_err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        RK_MPI_MB_ReleaseMB(blk); /* VO 自持引用（同 display show 语义），池块即还 */
        next_send = (int64_t)t1.tv_sec * 1000000000ll + t1.tv_nsec + period_ns;

#if 0
        /* 统计：10s 打点（fps/耗时均值与峰值/丢帧），评估 VO 通路瓶颈用 */
        uint64_t cost = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                        (uint64_t)(t1.tv_nsec - t0.tv_nsec);
        priv->prv_sent++;
        priv->prv_send_ns += cost;
        if (cost > priv->prv_send_max)
            priv->prv_send_max = cost;
        if (priv->prv_stat_ns == 0)
            priv->prv_stat_ns = (uint64_t)t1.tv_sec * 1000000000ull + t1.tv_nsec;
        uint64_t now = (uint64_t)t1.tv_sec * 1000000000ull + t1.tv_nsec;
        if (now - priv->prv_stat_ns >= 10ull * 1000 * 1000 * 1000) {
            double span = (double)(now - priv->prv_stat_ns) / 1e9;
            fprintf(stderr,
                    "rk_camera%d: preview fps=%.1f sendframe=%.1f/%llums 丢帧=%u(池空%u)\n",
                    priv->idx, priv->prv_sent / span,
                    (double)priv->prv_send_ns / (priv->prv_sent ? priv->prv_sent : 1) / 1e6,
                    (unsigned long long)(priv->prv_send_max / 1000000), priv->prv_dropped,
                    priv->prv_pool_empty);
            priv->prv_sent = 0;
            priv->prv_dropped = 0;
            priv->prv_send_ns = 0;
            priv->prv_send_max = 0;
            priv->prv_pool_empty = 0;
            priv->prv_stat_ns = now;
        }
#endif

    }
    return NULL;
}

static int rk_camera_preview_start(camera_device_t *dev, uint32_t width, uint32_t height) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    VO_CHN_ATTR_S chn_attr;

    if (!priv->streaming && !priv->encoded_streaming)
        return -EINVAL; /* 须在 Camera start 之后 */
    if (priv->preview_on)
        return 0; /* 幂等 */

    if (priv->encoded_streaming)
        return rk_camera_direct_preview_start(dev, width, height);

    /* VO 视频层旋转配置（camera HAL 内聚，层归 display HAL 建）：splice RGA
     * + chn enRotation 逆时针 90°。旋转后 1080x1920 由 VOP 缩放进 panel
     * 全幅 chn 矩形（display HAL 建层时已按 panel 定稿）。
     * DARKOS_PREVIEW_ROT=0/90/270 可覆盖（性能 A/B 测量用） */
    priv->preview_vo_layer = rk_vo_layer();
    priv->preview_rotation = rk_env_int("DARKOS_PREVIEW_ROT", ROTATION_270);
    priv->preview_vo_send = rk_env_int("DARKOS_PREVIEW_VO_SEND", 1);
    priv->preview_fps = rk_env_int("DARKOS_PREVIEW_FPS", 15); /* 实测依据见 preview 头注 */
    if (RK_MPI_VO_SetLayerSpliceMode(priv->preview_vo_layer, VO_SPLICE_MODE_RGA) != 0)
        fprintf(stderr, "rk_camera%d: VO SetLayerSpliceMode(RGA) failed\n", priv->idx);
    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.stRect = (RECT_S){0, 0, width, height};
    chn_attr.bDeflicker = RK_FALSE;
    chn_attr.u32Priority = 1; /* 与 display HAL 建层时的优先级一致 */
    chn_attr.enRotation = (ROTATION_E)priv->preview_rotation;
    if (RK_MPI_VO_SetChnAttr(priv->preview_vo_layer, 0, &chn_attr) != 0) {
        fprintf(stderr, "rk_camera%d: VO chn rotation setup failed\n", priv->idx);
        return -EIO;
    }

    priv->preview_err = 0;
    priv->prv_filled = 0;

    /* 预览拷贝缓冲池：6 块 CACHED。块数须盖住 VO 持帧时长——VO 送显期间
     * 内部持有引用，池块要等 VO 播完才回收（板上实测 VO 持帧 ~50ms+，
     * 2 块时 30fps 送帧大量"池空"丢帧，预览掉到 ~10fps）。
     * CACHED 而非 NOCACHE：NOCACHE 上 3MB memcpy 实测极慢（无缓存写），
     * CACHED 拷贝 + 送帧前 MmzFlushCache 是 rk_ui.c 同款做法。 */
    MB_POOL_CONFIG_S pool_cfg;
    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.u64MBSize = (uint64_t)priv->fmt.width * priv->fmt.height * 3 / 2;
    pool_cfg.u32MBCnt = 6;
    pool_cfg.enRemapMode = MB_REMAP_MODE_CACHED;
    pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    pool_cfg.bPreAlloc = RK_TRUE;
    priv->prv_pool = RK_MPI_MB_CreatePool(&pool_cfg);
    if (priv->prv_pool == MB_INVALID_POOLID) {
        fprintf(stderr, "rk_camera%d: preview MB pool create failed\n", priv->idx);
        return -ENOMEM;
    }

    priv->preview_on = 1;
    if (pthread_create(&priv->preview_thread, NULL, rk_preview_thread, priv) != 0) {
        priv->preview_on = 0;
        RK_MPI_MB_DestroyPool(priv->prv_pool);
        priv->prv_pool = MB_INVALID_POOLID;
        return -EIO;
    }
    priv->preview_thread_on = 1;
    fprintf(stderr, "rk_camera%d: preview on（cam%d.prv 直送 VO layer %d，逆时针 90°，"
                    "panel %ux%u）\n",
            priv->idx, priv->idx, priv->preview_vo_layer, width, height);
    return 0;
}

static int rk_camera_preview_stop(camera_device_t *dev) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;

    if (priv->preview_direct) {
        MPP_CHN_S src = {RK_ID_VI, priv->vi_pipe, priv->preview_vi_chn};
        MPP_CHN_S dst = {RK_ID_VO, priv->preview_vo_layer, 0};
        if (priv->preview_bound) {
            RK_MPI_SYS_UnBind(&src, &dst);
            priv->preview_bound = 0;
        }
        if (priv->preview_vi_chn_enabled) {
            RK_MPI_VI_DisableChn(priv->vi_pipe, priv->preview_vi_chn);
            priv->preview_vi_chn_enabled = 0;
        }
        priv->preview_direct = 0;
        priv->preview_on = 0;
        return 0;
    }
    if (!priv->preview_on)
        return 0;
    pthread_mutex_lock(&priv->prv_mtx);
    priv->preview_on = 0;
    pthread_cond_broadcast(&priv->prv_cond);
    pthread_mutex_unlock(&priv->prv_mtx);
    if (priv->preview_thread_on) {
        pthread_join(priv->preview_thread, NULL);
        priv->preview_thread_on = 0;
    }
    /* 信箱里可能还压着一个池块：还池 */
    pthread_mutex_lock(&priv->prv_mtx);
    if (priv->prv_filled) {
        priv->prv_filled = 0;
        RK_MPI_MB_ReleaseMB(priv->prv_blk);
    }
    pthread_mutex_unlock(&priv->prv_mtx);
    if (priv->prv_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(priv->prv_pool);
        priv->prv_pool = MB_INVALID_POOLID;
    }
    return 0;
}

static int rk_camera_stop(camera_device_t *dev) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;

    if (!priv->streaming)
        return 0;

    rk_camera_preview_stop(dev); /* 先摘预览通道（VI 显示通道 + VO 绑定） */
    priv->streaming = 0;
    if (priv->running) {
        priv->running = 0;
        pthread_join(priv->thread, NULL); /* GetChnFrame 最长 1s 超时，join 有界 */
    }
    if (priv->chn_enabled) {
        RK_MPI_VI_DisableChn(priv->vi_pipe, priv->vi_chn);
        priv->chn_enabled = 0;
    }
    if (priv->dev_enabled) {
        RK_MPI_VI_DisableDev(priv->vi_dev);
        priv->dev_enabled = 0;
    }
    rk_aiq_3a_stop(priv); /* 先停 VI 再停 3A（对齐 rkipc 逆序回收） */
    if (priv->sys_acquired) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
    }
    return 0;
}

static int rk_camera_set_frame_callback(camera_device_t *dev, camera_frame_cb cb, void *ctx) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    priv->cb = cb;
    priv->cb_ctx = ctx;
    return 0;
}

static int rk_camera_capture(camera_device_t *dev, camera_frame_t *frame, int timeout_ms) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;

    (void)timeout_ms; /* GetChnFrame 自带超时语义 */
    if (frame == NULL)
        return -EINVAL;
    if (!priv->streaming)
        return -EINVAL;
    return grab_one_frame(priv, frame);
}

/* 控制项映射 rkaiq uAPI2（亮度/对比度档位 0-255；曝光值单位 100µs，<0 恢复自动；
 * 增益为倍数，<=0 恢复自动；白平衡 0=手动 1=自动）。3A 未启动时返回 -ENOTSUP */
static int rk_camera_set_control(camera_device_t *dev, uint32_t id, int32_t value) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;
    XCamReturn rc;

    if (priv->aiq_ctx == NULL)
        return -ENOTSUP;

    switch (id) {
    case CAMERA_CTRL_BRIGHTNESS:
        if (value < 0)
            value = 0;
        if (value > 255)
            value = 255;
        rc = rk_aiq_uapi2_setBrightness(priv->aiq_ctx, (unsigned)value);
        priv->ctl_brightness = value;
        break;
    case CAMERA_CTRL_CONTRAST:
        if (value < 0)
            value = 0;
        if (value > 255)
            value = 255;
        rc = rk_aiq_uapi2_setContrast(priv->aiq_ctx, (unsigned)value);
        priv->ctl_contrast = value;
        break;
    case CAMERA_CTRL_EXPOSURE:
        if (value < 0) {
            rc = rk_aiq_uapi2_setExpTimeMode(priv->aiq_ctx, OP_AUTO);
        } else {
            rk_aiq_uapi2_setExpTimeMode(priv->aiq_ctx, OP_MANUAL);
            rc = rk_aiq_uapi2_setExpManualTime(priv->aiq_ctx, value * 1e-4f);
        }
        priv->ctl_exposure = value;
        break;
    case CAMERA_CTRL_GAIN:
        if (value <= 0) {
            rc = rk_aiq_uapi2_setExpGainMode(priv->aiq_ctx, OP_AUTO);
        } else {
            rk_aiq_uapi2_setExpGainMode(priv->aiq_ctx, OP_MANUAL);
            rc = rk_aiq_uapi2_setExpManualGain(priv->aiq_ctx, (float)value);
        }
        priv->ctl_gain = value;
        break;
    case CAMERA_CTRL_WHITE_BALANCE:
        rc = rk_aiq_uapi2_setWBMode(priv->aiq_ctx, value ? OP_AUTO : OP_MANUAL);
        priv->ctl_wb = value ? 1 : 0;
        break;
    default:
        return -ENOTSUP;
    }
    return rc == XCAM_RETURN_NO_ERROR ? 0 : -EIO;
}

static int rk_camera_get_control(camera_device_t *dev, uint32_t id, int32_t *value) {
    rk_camera_priv_t *priv = (rk_camera_priv_t *)dev->priv;

    if (priv->aiq_ctx == NULL || value == NULL)
        return -ENOTSUP;

    switch (id) {
    case CAMERA_CTRL_BRIGHTNESS:
        *value = priv->ctl_brightness;
        break;
    case CAMERA_CTRL_CONTRAST:
        *value = priv->ctl_contrast;
        break;
    case CAMERA_CTRL_EXPOSURE:
        *value = priv->ctl_exposure;
        break;
    case CAMERA_CTRL_GAIN:
        *value = priv->ctl_gain;
        break;
    case CAMERA_CTRL_WHITE_BALANCE:
        *value = priv->ctl_wb;
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static const camera_device_ops_t rk_camera_ops = {
    .get_capabilities = rk_camera_get_capabilities,
    .set_format = rk_camera_set_format,
    .get_format = rk_camera_get_format,
    .start = rk_camera_start,
    .stop = rk_camera_stop,
    .set_frame_callback = rk_camera_set_frame_callback,
    .capture = rk_camera_capture,
    .set_control = rk_camera_set_control,
    .get_control = rk_camera_get_control,
    .preview_start = rk_camera_preview_start,
    .preview_stop = rk_camera_preview_stop,
    .encoded_start = rk_camera_encoded_start,
    .encoded_get_packet = rk_camera_encoded_get_packet,
    .encoded_stop = rk_camera_encoded_stop,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int rk_camera_close(hw_device_t *device) {
    camera_device_t *dev = (camera_device_t *)device;
    rk_camera_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (rk_camera_priv_t *)dev->priv;
    if (priv != NULL) {
        if (priv->encoded_streaming || priv->encoded_venc_created)
            rk_camera_encoded_stop((camera_device_t *)dev);
        if (priv->streaming)
            rk_camera_stop(dev); /* 内含 SYS 引用释放 */
        free(priv->nv12);
        free(priv);
    }
    free(dev);
    return 0;
}

/* 解析 open id："camera"/"camera0" → 0，"cameraN" → N；非法返回 -1 */
static int rk_camera_parse_idx(const char *id) {
    const char *prefix = CAMERA_HARDWARE_MODULE_ID; /* "camera" */
    size_t plen = strlen(prefix);
    long idx;
    char *end;

    if (id == NULL || strncmp(id, prefix, plen) != 0)
        return -1;
    if (id[plen] == '\0')
        return 0; /* "camera" 等价 camera0 */
    idx = strtol(id + plen, &end, 10);
    if (*end != '\0' || idx < 0 || idx >= RK_CAM_MAX_INSTANCES)
        return -1;
    return (int)idx;
}

/* env 读取实例级整型配置（DARKOS_CAMERA{idx}_VI_DEV 等），非法值回落 def */
static int rk_camera_env_int(int idx, const char *key, int def) {
    char name[64];
    const char *val;
    char *end;
    long v;

    snprintf(name, sizeof(name), "DARKOS_CAMERA%d_%s", idx, key);
    val = getenv(name);
    if (val == NULL || val[0] == '\0')
        return def;
    v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < 0 || v > 4096)
        return def;
    return (int)v;
}

static int rk_camera_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    camera_device_t *dev;
    rk_camera_priv_t *priv;
    char env_name[64];
    const char *env_val;
    int idx;

    if (device == NULL)
        return -EINVAL;
    idx = rk_camera_parse_idx(id);
    if (idx < 0) {
        fprintf(stderr, "rk_camera: 非法设备 id \"%s\"（期望 camera / cameraN）\n",
                id != NULL ? id : "(null)");
        return -EINVAL;
    }

    dev = (camera_device_t *)calloc(1, sizeof(*dev));
    priv = (rk_camera_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->idx = idx;
    priv->vi_dev = rk_camera_env_int(idx, "VI_DEV", idx);
    priv->vi_pipe = rk_camera_env_int(idx, "VI_PIPE", idx);
    priv->vi_chn = rk_camera_env_int(idx, "VI_CHN", 0);
    priv->encoded_vi_chn = rk_camera_env_int(idx, "ENCODED_VI_CHN", 3);
    priv->encoded_venc_chn = rk_camera_env_int(idx, "VENC_CHN", idx);

    /* 摄像头类型：DARKOS_CAMERA{idx}_TYPE=ir|white，默认白光（全彩） */
    snprintf(env_name, sizeof(env_name), "DARKOS_CAMERA%d_TYPE", idx);
    env_val = getenv(env_name);
    priv->type = (env_val != NULL && strcmp(env_val, "ir") == 0) ? CAMERA_TYPE_IR
                                                                 : CAMERA_TYPE_VISUAL;

    priv->fmt = (camera_format_t){
        .width = 1920, .height = 1080, .pixel_format = CAMERA_PIX_FMT_NV12, .fps = 30};
    /* 控制项缓存初值：中档亮度/对比度，曝光/增益自动，白平衡自动 */
    priv->ctl_brightness = 128;
    priv->ctl_contrast = 128;
    priv->ctl_exposure = -1;
    priv->ctl_gain = -1;
    priv->ctl_wb = 1;

    /* 预览换手信箱（preview_start/stop 使用） */
    pthread_mutex_init(&priv->prv_mtx, NULL);
    pthread_cond_init(&priv->prv_cond, NULL);
    priv->prv_pool = MB_INVALID_POOLID;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CAMERA_DEVICE_API_VERSION_1_1;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = rk_camera_close;
    dev->ops = &rk_camera_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出（hal.rockchip.rv1126b.so，dlsym("HMI_camera")）
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t rk_camera_methods = {
    .open = rk_camera_open,
};

struct hw_module_t HMI_camera = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = CAMERA_HARDWARE_MODULE_ID,
    .name = "Rockchip RV1126B Camera HAL (MPI VI)",
    .author = "DarkOS",
    .methods = &rk_camera_methods,
};
