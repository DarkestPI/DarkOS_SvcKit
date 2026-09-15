/*
 * Rockchip RV1126B Display HAL：基于 rockit MPI VO 的显示输出实现。
 *
 * 把 hardware/interfaces/display 的抽象接口映射到 rockit MPI VO：
 *   start → VO SetPubAttr（接口类型 + 默认时序）→ Enable → 回读 panel
 *           时序 → video layer（NV12）→ 全屏 chn
 *   show  → SendFrame 送一帧（priv 携 MB_BLK 时零拷贝，否则池内拷贝）
 *   stop  → 逆序回收
 *
 * 说明：
 *   - 共享式：VO dev/layer 是单板单资源，display_open/start 可被多个服务
 *     各调一次（MediaService 预览 + UIService 的 VO 目标 OSD）。模块级
 *     引用计数：首个 start 做真实建立，重复 start 幂等成功并共享 panel
 *     回读值；stop 减计数，归零才真正拆除（格式以首个 start 者为准，
 *     运行中 set_format 幂等忽略）；
 *   - panel 时序用 VO_OUTPUT_DEFAULT 交驱动，Enable 后 GetPubAttr 回读
 *     stSyncInfo.u16Hact/u16Vact；回读为 0（未接屏等）用请求宽高兜底；
 *     VO Enable 失败（未接屏/无显示硬件）直接报错，由上层降级；
 *   - 默认 dev=0 / layer=0 / MIPI，可用环境变量 DARKOS_VO_DEV /
 *     DARKOS_VO_LAYER 覆盖（板型相关，参考 rkipc rv1126b_ipc 的
 *     vo_dev_id/vo_layer_id 配置项）；
 *   - 未做 CSC/旋转/RGA splice（rkipc 竖屏 panel 的 ROTATION_270 等），
 *     有具体屏时按需补 set_control。
 */

#include <display/IDisplay.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rk_comm_mb.h>
#include <rk_comm_video.h>
#include <rk_comm_vo.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_vo.h>

#include "common/mpi_sys_guard.h"

#define RK_VO_POOL_CNT 2 /* show 拷贝路径缓冲池块数 */

/* ---------------------------------------------------------------------------
 * 共享式 VO 状态：VO dev/layer 是单板单资源，而 display_open/start 会被
 * 多个服务各调一次（MediaService 预览 + UIService 的 VO 目标 OSD）。
 * 模块级引用计数：首个 start 做真实 VO 建立，重复 start 只加计数并回读
 * 已生效的 panel 参数；stop 减计数，归零才真正拆除 VO。
 * ------------------------------------------------------------------------- */
static pthread_mutex_t g_vo_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_vo_refcount = 0; /* >0 时 VO 已建立 */
static uint32_t g_panel_w = 0;
static uint32_t g_panel_h = 0;
static MB_POOL g_show_pool = MB_INVALID_POOLID;
static int g_sys_acquired = 0;

typedef struct rk_display_priv {
    display_format_t fmt; /* 请求格式（宽高可为 0 = 跟随 panel） */
    int vo_dev;
    int vo_layer;
    int started; /* 本设备已计入共享引用 */
} rk_display_priv_t;

/* 环境变量覆盖（板型相关参数）；非法值回落默认 */
static int rk_vo_env_int(const char *name, int dflt) {
    const char *s = getenv(name);
    char *end = NULL;
    long v;

    if (s == NULL || s[0] == '\0')
        return dflt;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 16)
        return dflt;
    return (int)v;
}

static VO_INTF_TYPE_E rk_vo_intf_map(uint32_t intf) {
    switch (intf) {
    case DISPLAY_INTF_LCD:
        return VO_INTF_LCD;
    case DISPLAY_INTF_BT1120:
        return VO_INTF_BT1120;
    case DISPLAY_INTF_CVBS:
        return VO_INTF_CVBS;
    case DISPLAY_INTF_HDMI:
        return VO_INTF_HDMI;
    case DISPLAY_INTF_MIPI:
    default:
        return VO_INTF_MIPI;
    }
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int rk_display_get_capabilities(display_device_t *dev, display_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_intfs = DISPLAY_CAPS_INTF_MIPI | DISPLAY_CAPS_INTF_LCD |
                            DISPLAY_CAPS_INTF_BT1120 | DISPLAY_CAPS_INTF_CVBS |
                            DISPLAY_CAPS_INTF_HDMI;
    caps->supported_formats = DISPLAY_FMT_NV12;
    caps->max_width = 1920;
    caps->max_height = 1080;
    return 0;
}

static int rk_display_set_format(display_device_t *dev, const display_format_t *fmt) {
    rk_display_priv_t *priv = (rk_display_priv_t *)dev->priv;
    int rc = 0;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->pixel_format != DISPLAY_FMT_NV12)
        return -EINVAL; /* video layer 直送仅接 NV12 */

    pthread_mutex_lock(&g_vo_mtx);
    if (g_vo_refcount > 0) {
        /* 共享式：VO 已建立时格式以首个 start 者为准，后来的 set_format
         * 幂等成功（各服务请求的都是同一 NV12/跟随 panel 格式） */
    } else {
        priv->fmt = *fmt;
    }
    pthread_mutex_unlock(&g_vo_mtx);
    return rc;
}

static int rk_display_get_format(display_device_t *dev, display_format_t *fmt) {
    rk_display_priv_t *priv = (rk_display_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = priv->fmt;
    pthread_mutex_lock(&g_vo_mtx);
    if (g_vo_refcount > 0) { /* 已回读 panel 时给出实际生效值 */
        fmt->width = g_panel_w;
        fmt->height = g_panel_h;
    }
    pthread_mutex_unlock(&g_vo_mtx);
    return 0;
}

/* 真实 VO 建立（g_vo_mtx 已持）。返回 0 成功。 */
static int rk_vo_bringup(rk_display_priv_t *priv) {
    VO_PUB_ATTR_S pub;
    VO_VIDEO_LAYER_ATTR_S layer_attr;
    VO_CHN_ATTR_S chn_attr;
    MB_POOL_CONFIG_S pool_cfg;
    uint32_t w, h;

    if (rk_mpi_sys_acquire() != 0)
        return -EIO;
    g_sys_acquired = 1;

    memset(&pub, 0, sizeof(pub));
    pub.enIntfType = rk_vo_intf_map(priv->fmt.intf);
    pub.enIntfSync = VO_OUTPUT_DEFAULT; /* panel 时序交驱动 */
    if (RK_MPI_VO_SetPubAttr(priv->vo_dev, &pub) != RK_SUCCESS) {
        fprintf(stderr, "rk_display: VO SetPubAttr failed\n");
        goto out_sys;
    }
    if (RK_MPI_VO_Enable(priv->vo_dev) != RK_SUCCESS) {
        fprintf(stderr, "rk_display: VO Enable failed（未接屏？）\n");
        goto out_sys;
    }

    /* 回读 panel 实际时序；为 0 时用请求宽高兜底（对齐 rkipc 范式） */
    if (RK_MPI_VO_GetPubAttr(priv->vo_dev, &pub) == RK_SUCCESS &&
        pub.stSyncInfo.u16Hact > 0 && pub.stSyncInfo.u16Vact > 0) {
        w = pub.stSyncInfo.u16Hact;
        h = pub.stSyncInfo.u16Vact;
    } else {
        w = priv->fmt.width ? priv->fmt.width : 1920;
        h = priv->fmt.height ? priv->fmt.height : 1080;
        fprintf(stderr, "rk_display: panel 时序回读无效，兜底 %ux%u\n", w, h);
    }
    g_panel_w = w;
    g_panel_h = h;

    /* 视频层显示缓冲深度 3（对齐 rkipc dv：VO 内部队列平滑帧回收，
     * 默认深度下 VO 持帧偏久，预览源端池块周转不开） */
    RK_MPI_VO_SetLayerDispBufLen(priv->vo_layer, 3);

    memset(&layer_attr, 0, sizeof(layer_attr));
    layer_attr.stDispRect = (RECT_S){0, 0, w, h};
    layer_attr.stImageSize = (SIZE_S){w, h};
    layer_attr.u32DispFrmRt = priv->fmt.fps ? priv->fmt.fps : 30;
    layer_attr.enPixFormat = RK_FMT_YUV420SP;
    layer_attr.enCompressMode = COMPRESS_MODE_NONE;
    if (RK_MPI_VO_BindLayer(priv->vo_layer, priv->vo_dev, VO_LAYER_MODE_VIDEO) != RK_SUCCESS) {
        fprintf(stderr, "rk_display: VO BindLayer failed\n");
        goto out_dev;
    }
    /* 视频层优先级压到 0：UI 层（graphics HAL，优先级 1）必须叠在视频之上，
     * 不设置时默认值会导致 OSD 被预览画面盖住（对齐 rkipc dv：
     * VO_PREV_LAYER_PRIORITY=0 / VO_UI_LAYER_PRIORITY=1） */
    RK_MPI_VO_SetLayerPriority(priv->vo_layer, 0);
    if (RK_MPI_VO_SetLayerAttr(priv->vo_layer, &layer_attr) != RK_SUCCESS ||
        RK_MPI_VO_EnableLayer(priv->vo_layer) != RK_SUCCESS) {
        fprintf(stderr, "rk_display: VO layer setup failed\n");
        goto out_layer;
    }

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.u32Priority = 1;
    chn_attr.stRect = (RECT_S){0, 0, w, h};
    chn_attr.bDeflicker = RK_FALSE;
    if (RK_MPI_VO_SetChnAttr(priv->vo_layer, 0, &chn_attr) != RK_SUCCESS ||
        RK_MPI_VO_EnableChn(priv->vo_layer, 0) != RK_SUCCESS) {
        fprintf(stderr, "rk_display: VO chn setup failed\n");
        goto out_layer;
    }

    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.u64MBSize = (uint64_t)w * h * 3 / 2;
    pool_cfg.u32MBCnt = RK_VO_POOL_CNT;
    pool_cfg.enRemapMode = MB_REMAP_MODE_NOCACHE;
    pool_cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    pool_cfg.bPreAlloc = RK_TRUE;
    g_show_pool = RK_MPI_MB_CreatePool(&pool_cfg);
    if (g_show_pool == MB_INVALID_POOLID) {
        fprintf(stderr, "rk_display: show MB pool create failed\n");
        goto out_chn;
    }

    fprintf(stderr, "rk_display: VO started, dev=%d layer=%d %ux%u\n", priv->vo_dev,
            priv->vo_layer, w, h);
    return 0;

out_chn:
    RK_MPI_VO_DisableChn(priv->vo_layer, 0);
out_layer:
    RK_MPI_VO_DisableLayer(priv->vo_layer);
    RK_MPI_VO_UnBindLayer(priv->vo_layer, priv->vo_dev);
out_dev:
    RK_MPI_VO_Disable(priv->vo_dev);
out_sys:
    rk_mpi_sys_release();
    g_sys_acquired = 0;
    return -EIO;
}

/* 真实 VO 拆除（g_vo_mtx 已持，引用计数已归零） */
static void rk_vo_teardown(rk_display_priv_t *priv) {
    RK_MPI_VO_DisableChn(priv->vo_layer, 0);
    RK_MPI_VO_DisableLayer(priv->vo_layer);
    RK_MPI_VO_Disable(priv->vo_dev);
    RK_MPI_VO_UnBindLayer(priv->vo_layer, priv->vo_dev);
    RK_MPI_VO_CloseFd(); /* 回收 VO 的 fd 资源（对齐 rkipc 去初始化序列） */
    if (g_show_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(g_show_pool);
        g_show_pool = MB_INVALID_POOLID;
    }
    if (g_sys_acquired) {
        rk_mpi_sys_release();
        g_sys_acquired = 0;
    }
    g_panel_w = 0;
    g_panel_h = 0;
}

static int rk_display_start(display_device_t *dev) {
    rk_display_priv_t *priv = (rk_display_priv_t *)dev->priv;
    int rc = 0;

    pthread_mutex_lock(&g_vo_mtx);
    if (priv->started)
        goto out; /* 本设备已 start，幂等 */
    if (g_vo_refcount > 0) {
        g_vo_refcount++; /* 共享式：重复 start 只加计数 */
        priv->started = 1;
        goto out;
    }
    rc = rk_vo_bringup(priv);
    if (rc == 0) {
        g_vo_refcount = 1;
        priv->started = 1;
    }
out:
    pthread_mutex_unlock(&g_vo_mtx);
    return rc;
}

static int rk_display_stop(display_device_t *dev) {
    rk_display_priv_t *priv = (rk_display_priv_t *)dev->priv;

    pthread_mutex_lock(&g_vo_mtx);
    if (priv->started) {
        priv->started = 0;
        if (--g_vo_refcount == 0)
            rk_vo_teardown(priv); /* 最后一个引用才真正停 VO */
    }
    pthread_mutex_unlock(&g_vo_mtx);
    return 0;
}

/* 送一帧显示：priv 携 MB_BLK（同厂商帧源，如 camera/VDEC 出帧）零拷贝直送，
 * 否则池内拷贝一帧再送；SendFrame 返回后 VO 自持引用，帧即可释放 */
static int rk_display_show(display_device_t *dev, const display_frame_t *frame,
                           int timeout_ms) {
    rk_display_priv_t *priv = (rk_display_priv_t *)dev->priv;
    VIDEO_FRAME_INFO_S vf;
    MB_BLK blk, owned_blk = NULL;
    int rc;

    if (frame == NULL)
        return -EINVAL;

    blk = (MB_BLK)frame->priv;
    if (blk == NULL) {
        void *vir;

        if (frame->data == NULL)
            return -EINVAL;
        pthread_mutex_lock(&g_vo_mtx);
        if (g_vo_refcount == 0) {
            pthread_mutex_unlock(&g_vo_mtx);
            return -EINVAL;
        }
        owned_blk = RK_MPI_MB_GetMB(g_show_pool, frame->size, RK_TRUE);
        pthread_mutex_unlock(&g_vo_mtx);
        if (owned_blk == NULL)
            return -ENOMEM;
        vir = RK_MPI_MB_Handle2VirAddr(owned_blk);
        if (vir == NULL) {
            RK_MPI_MB_ReleaseMB(owned_blk);
            return -EIO;
        }
        memcpy(vir, frame->data, frame->size);
        blk = owned_blk;
    }

    memset(&vf, 0, sizeof(vf));
    vf.stVFrame.pMbBlk = blk;
    vf.stVFrame.u32Width = frame->width;
    vf.stVFrame.u32Height = frame->height;
    vf.stVFrame.u32VirWidth = frame->stride ? frame->stride : frame->width;
    vf.stVFrame.u32VirHeight = frame->height;
    vf.stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    vf.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    vf.stVFrame.u64PTS = frame->timestamp_ns / 1000ull;

    rc = RK_MPI_VO_SendFrame(priv->vo_layer, 0, &vf, timeout_ms);
    if (owned_blk != NULL)
        RK_MPI_MB_ReleaseMB(owned_blk);
    return rc == RK_SUCCESS ? 0 : -EIO;
}

static const display_device_ops_t rk_display_ops = {
    .get_capabilities = rk_display_get_capabilities,
    .set_format = rk_display_set_format,
    .get_format = rk_display_get_format,
    .start = rk_display_start,
    .stop = rk_display_stop,
    .show = rk_display_show,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int rk_display_close(hw_device_t *device) {
    display_device_t *dev = (display_device_t *)device;
    rk_display_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (rk_display_priv_t *)dev->priv;
    if (priv != NULL) {
        rk_display_stop(dev); /* 未 stop 的引用在 close 时归还 */
        free(priv);
    }
    free(dev);
    return 0;
}

static int rk_display_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    display_device_t *dev;
    rk_display_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (display_device_t *)calloc(1, sizeof(*dev));
    priv = (rk_display_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->fmt = (display_format_t){.width = 0, /* 0 = 跟随 panel */
                                   .height = 0,
                                   .pixel_format = DISPLAY_FMT_NV12,
                                   .intf = DISPLAY_INTF_MIPI,
                                   .fps = 30};
    priv->vo_dev = rk_vo_env_int("DARKOS_VO_DEV", 0);
    priv->vo_layer = rk_vo_env_int("DARKOS_VO_LAYER", 0);

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = rk_display_close;
    dev->ops = &rk_display_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出（hal.rockchip.rv1126b.so，dlsym("HMI_display")）
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t rk_display_methods = {
    .open = rk_display_open,
};

struct hw_module_t HMI_display = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = DISPLAY_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = DISPLAY_HARDWARE_MODULE_ID,
    .name = "Rockchip RV1126B Display HAL (MPI VO)",
    .author = "DarkOS",
    .methods = &rk_display_methods,
};
