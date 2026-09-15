/*
 * Rockchip RV1126B Graphics HAL：OSD 叠加。
 *
 * v1 只实现 OSD 三件套（blit/fill 返回 -ENOTSUP，待 RGA 需求落地），
 * 按叠加目标分两条路径：
 *
 * VENC（叠加进编码码流，远端可见）——RGN canvas：
 *   osd_create  → RGN Create(OVERLAY_RGN, ARGB8888, 尺寸 align16, canvasNum=1)
 *                 → AttachToChn（MPP_CHN {RK_ID_VENC, 0, chn}，位置 align16，
 *                 u32Layer=0，u32FgAlpha=255/u32BgAlpha=0——该两字段注释标注
 *                 只对 BGRA5551 生效，ARGB8888 走逐像素 alpha，沿用 rkipc 惯例）
 *                 → GetCanvasInfo 透出 canvas virAddr/stride
 *   osd_flush   → GetCanvasInfo → UpdateCanvas（必须先 GetCanvasInfo：
 *                 rockit 内部的当前 canvas 状态由 GetCanvasInfo 建立，跳过它
 *                 直接 UpdateCanvas 会在 RKRGNImpl_runtimeUpdateCanvas 空指针
 *                 SIGSEGV——板上实测；SDK sample 与 rkipc 同款时序）
 *   osd_destroy → DetachFromChn → Destroy
 *
 * VO（叠加到本地显示，panel 预览）——VO 专用 UI 层（CURSOR 模式）+ SendFrame：
 *   RV1126B 上 RGN 不支持 attach VO（板上实测 AttachToChn(mod=VO) 失败）；
 *   机制对照 rkipc rv1126b_dv 的 ui/rk_ui.c：
 *   osd_create  → BindLayer(layer=1, dev, VO_LAYER_MODE_CURSOR)
 *                 → SetLayerAttr(DispRect/ImageSize=rect，bBypassFrame，
 *                 RK_FMT_RGB888) → SetLayerSpliceMode(RGA) → SetLayerPriority
 *                 （UI 层 1 > 视频层 0，层号约定同 media_ctrl.h）
 *                 → EnableLayer → SetLayerCSC(identity 50/50/50/50)
 *                 → SetChnAttr + EnableChn（chn 0）
 *                 → MMZ_Alloc(CACHEABLE, w*h*4) 作 canvas 透出（CPU 直写）
 *   osd_flush   → MmzFlushCache（CPU 写完，VO 硬件读）→ 包装
 *                 VIDEO_FRAME_INFO_S（RK_FMT_BGRA8888——LVGL ARGB8888 内存序
 *                 即 B,G,R,A）→ RK_MPI_VO_SendFrame(layer, 0, 1000ms)
 *   osd_destroy → DisableChn → DisableLayer → UnBindLayer → MMZ_Free
 *                 （不 VO_Disable：VO dev 归 display HAL 管，见 UiService）
 *
 * 两条路径的 canvas 都是 CPU 可直写的连续内存，LVGL DIRECT 渲染画进去，
 * flush 生效——全链路零拷贝。
 *
 * SYS_Init/Exit 走 common/mpi_sys_guard 引用计数（与其他模块共享）。
 */

#include <hardware/hardware.h>
#include <graphics/IGraphics.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rk_comm_rgn.h>
#include <rk_comm_video.h>
#include <rk_comm_vo.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_mmz.h>
#include <rk_mpi_rgn.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_vo.h>

#include "common/mpi_sys_guard.h"

/* 尺寸/位置对齐：RGN OVERLAY 要求宽高与坐标 16 对齐（rk_comm_rgn.h 注释） */
#define RK_OSD_ALIGN 16
/* RGN handle：v1 每设备单 OSD 实例，固定 0（与 u32Layer 同号惯例） */
#define RK_OSD_HANDLE 0
/* VO 层分配（rkipc rv1126b_dv media_ctrl.h 约定）：视频层 0（display HAL）、
 * UI 层 1（本模块），UI 层 chn 固定 0 */
#define RK_VO_UI_LAYER 1
#define RK_VO_UI_CHN 0
#define RK_VO_UI_PRIORITY 1

typedef struct rk_graphics_priv {
    osd_target_t target;
    int sys_acquired;
    uint32_t canvas_w, canvas_h; /* align16 后的实际画布尺寸 */
    uint64_t flush_count;

    /* VENC 路径（RGN canvas） */
    RGN_HANDLE handle;
    MPP_CHN_S chn; /* attach 目标（destroy 时 Detach 要用） */
    int attached;

    /* VO 路径（UI 层 + SendFrame） */
    int vo_dev;     /* 默认 0，DARKOS_VO_DEV 覆盖（与 display HAL 对齐） */
    int vo_ready;   /* UI 层已 Bind+Enable */
    MB_BLK vo_blk;  /* canvas MMZ 块 */
    void *vo_vir;   /* canvas virAddr */
} rk_graphics_priv_t;

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) / a * a;
}

/* env 整型读取（与 display_rockchip.c 的 DARKOS_VO_DEV 惯例对齐） */
static int rk_env_int(const char *name, int dft) {
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0')
        return dft;
    return atoi(v);
}

/* ---------------------------------------------------------------------------
 * blit/fill：v1 未实现
 * ------------------------------------------------------------------------- */

static int rk_gfx_blit(graphics_device_t *dev, const graphics_buffer_t *src,
                       const graphics_rect_t *src_rect, const graphics_buffer_t *dst,
                       const graphics_rect_t *dst_rect, uint32_t transform) {
    (void)dev;
    (void)src;
    (void)src_rect;
    (void)dst;
    (void)dst_rect;
    (void)transform;
    return -ENOTSUP; /* v1 未实现 */
}

static int rk_gfx_fill(graphics_device_t *dev, graphics_buffer_t *dst,
                       const graphics_rect_t *rect, uint32_t color) {
    (void)dev;
    (void)dst;
    (void)rect;
    (void)color;
    return -ENOTSUP;
}

/* ---------------------------------------------------------------------------
 * VENC 路径：RGN canvas
 * ------------------------------------------------------------------------- */

static int rk_osd_create_venc(rk_graphics_priv_t *priv, const osd_config_t *cfg,
                              graphics_buffer_t *canvas_out) {
    RGN_ATTR_S attr;
    RGN_CHN_ATTR_S chnAttr;
    RGN_CANVAS_INFO_S canvas;

    /* Create：OVERLAY_RGN，ARGB8888 单 canvas（rkipc rv1126b_ipc 与 SDK
     * sample 均用 canvasNum=1），尺寸向上 align16 */
    memset(&attr, 0, sizeof(attr));
    attr.enType = OVERLAY_RGN;
    attr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
    attr.unAttr.stOverlay.stSize.u32Width = align_up(cfg->rect.w, RK_OSD_ALIGN);
    attr.unAttr.stOverlay.stSize.u32Height = align_up(cfg->rect.h, RK_OSD_ALIGN);
    attr.unAttr.stOverlay.u32CanvasNum = 1;
    attr.unAttr.stOverlay.u32ClutNum = 0;
    if (RK_MPI_RGN_Create(RK_OSD_HANDLE, &attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: RGN Create failed (%ux%u)\n",
                attr.unAttr.stOverlay.stSize.u32Width,
                attr.unAttr.stOverlay.stSize.u32Height);
        return -EIO;
    }

    /* Attach：VENC 通道 = 编码通道（= 相机实例号） */
    priv->chn.enModId = RK_ID_VENC;
    priv->chn.s32DevId = 0;
    priv->chn.s32ChnId = (RK_S32)cfg->chn;

    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.bShow = RK_TRUE;
    chnAttr.enType = OVERLAY_RGN;
    chnAttr.unChnAttr.stOverlayChn.stPoint.s32X =
        (RK_S32)(cfg->rect.x < 0 ? 0 : cfg->rect.x) & ~(RK_OSD_ALIGN - 1); /* 位置 align16 */
    chnAttr.unChnAttr.stOverlayChn.stPoint.s32Y =
        (RK_S32)(cfg->rect.y < 0 ? 0 : cfg->rect.y) & ~(RK_OSD_ALIGN - 1);
    /* u32FgAlpha/u32BgAlpha 注释标注仅 BGRA5551 生效；ARGB8888 逐像素 alpha，
     * 沿用 rkipc 惯例设 255/0 */
    chnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 255;
    chnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;
    chnAttr.unChnAttr.stOverlayChn.u32Layer = RK_OSD_HANDLE;
    if (RK_MPI_RGN_AttachToChn(RK_OSD_HANDLE, &priv->chn, &chnAttr) != RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: RGN AttachToChn failed (mod=%d chn=%d)\n",
                priv->chn.enModId, priv->chn.s32ChnId);
        RK_MPI_RGN_Destroy(RK_OSD_HANDLE);
        return -EIO;
    }
    priv->attached = 1;

    memset(&canvas, 0, sizeof(canvas));
    if (RK_MPI_RGN_GetCanvasInfo(RK_OSD_HANDLE, &canvas) != RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: RGN GetCanvasInfo failed\n");
        RK_MPI_RGN_DetachFromChn(RK_OSD_HANDLE, &priv->chn);
        RK_MPI_RGN_Destroy(RK_OSD_HANDLE);
        priv->attached = 0;
        return -EIO;
    }

    priv->canvas_w = canvas.stSize.u32Width;
    priv->canvas_h = canvas.stSize.u32Height;

    memset(canvas_out, 0, sizeof(*canvas_out));
    canvas_out->fd = -1;
    canvas_out->data = (void *)(uintptr_t)canvas.u64VirAddr;
    canvas_out->size = canvas.u32VirWidth * canvas.u32VirHeight * 4;
    canvas_out->width = canvas.stSize.u32Width;
    canvas_out->height = canvas.stSize.u32Height;
    canvas_out->stride = canvas.u32VirWidth * 4; /* 字节 */
    canvas_out->pixel_format = GRAPHICS_OSD_PIXEL_FORMAT;
    return 0;
}

/* ---------------------------------------------------------------------------
 * VO 路径：UI 层（CURSOR）+ SendFrame（对照 rkipc rv1126b_dv ui/rk_ui.c）
 * ------------------------------------------------------------------------- */

static int rk_osd_create_vo(rk_graphics_priv_t *priv, const osd_config_t *cfg,
                            graphics_buffer_t *canvas_out) {
    VO_VIDEO_LAYER_ATTR_S layerAttr;
    VO_CSC_S csc;
    VO_CHN_ATTR_S chnAttr;
    uint32_t w = align_up(cfg->rect.w, RK_OSD_ALIGN);
    uint32_t h = align_up(cfg->rect.h, RK_OSD_ALIGN);
    int32_t x = cfg->rect.x < 0 ? 0 : cfg->rect.x;
    int32_t y = cfg->rect.y < 0 ? 0 : cfg->rect.y;

    priv->vo_dev = rk_env_int("DARKOS_VO_DEV", 0);

    /* 层显示缓冲深度（rk_ui.c：BindLayer 之前设为 2） */
    RK_U32 bufLen = 0;
    RK_MPI_VO_GetLayerDispBufLen(RK_VO_UI_LAYER, &bufLen);
    if (RK_MPI_VO_SetLayerDispBufLen(RK_VO_UI_LAYER, 2) != RK_SUCCESS)
        fprintf(stderr, "rk_graphics: VO SetLayerDispBufLen failed（原值 %u）\n", bufLen);

    if (RK_MPI_VO_BindLayer(RK_VO_UI_LAYER, priv->vo_dev, VO_LAYER_MODE_CURSOR) !=
        RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: VO BindLayer(dev=%d layer=%d) failed\n", priv->vo_dev,
                RK_VO_UI_LAYER);
        return -EIO;
    }
    priv->vo_ready = 1;

    memset(&layerAttr, 0, sizeof(layerAttr));
    layerAttr.stDispRect = (RECT_S){x, y, w, h};
    layerAttr.stImageSize = (SIZE_S){w, h};
    layerAttr.bBypassFrame = RK_TRUE; /* 不过 RGA/GPU 合成，帧直送 VOP */
    layerAttr.u32DispFrmRt = 30;
    layerAttr.enPixFormat = RK_FMT_RGB888;
    if (RK_MPI_VO_SetLayerAttr(RK_VO_UI_LAYER, &layerAttr) != RK_SUCCESS ||
        RK_MPI_VO_SetLayerSpliceMode(RK_VO_UI_LAYER, VO_SPLICE_MODE_RGA) != RK_SUCCESS ||
        RK_MPI_VO_SetLayerPriority(RK_VO_UI_LAYER, RK_VO_UI_PRIORITY) != RK_SUCCESS ||
        RK_MPI_VO_EnableLayer(RK_VO_UI_LAYER) != RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: VO layer setup failed\n");
        goto out_unbind;
    }

    memset(&csc, 0, sizeof(csc));
    csc.enCscMatrix = VO_CSC_MATRIX_IDENTITY;
    csc.u32Contrast = 50;
    csc.u32Hue = 50;
    csc.u32Luma = 50;
    csc.u32Satuature = 50;
    RK_MPI_VO_SetLayerCSC(RK_VO_UI_LAYER, &csc); /* 非关键路径，失败不致命 */

    memset(&chnAttr, 0, sizeof(chnAttr));
    chnAttr.bDeflicker = RK_FALSE;
    chnAttr.u32Priority = 0;
    chnAttr.stRect = (RECT_S){x, y, w, h};
    if (RK_MPI_VO_SetChnAttr(RK_VO_UI_LAYER, RK_VO_UI_CHN, &chnAttr) != RK_SUCCESS ||
        RK_MPI_VO_EnableChn(RK_VO_UI_LAYER, RK_VO_UI_CHN) != RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: VO chn setup failed\n");
        goto out_layer;
    }

    /* canvas：MMZ 物理连续内存（CACHEABLE：CPU 直写，flush 时 MmzFlushCache） */
    if (RK_MPI_MMZ_Alloc(&priv->vo_blk, w * h * 4, RK_MMZ_ALLOC_CACHEABLE) != RK_SUCCESS) {
        fprintf(stderr, "rk_graphics: MMZ alloc %u failed\n", w * h * 4);
        goto out_chn;
    }
    priv->vo_vir = RK_MPI_MB_Handle2VirAddr(priv->vo_blk);
    if (priv->vo_vir == NULL) {
        fprintf(stderr, "rk_graphics: MMZ Handle2VirAddr failed\n");
        goto out_mmz;
    }
    memset(priv->vo_vir, 0, (size_t)w * h * 4);

    priv->canvas_w = w;
    priv->canvas_h = h;

    memset(canvas_out, 0, sizeof(*canvas_out));
    canvas_out->fd = -1;
    canvas_out->data = priv->vo_vir;
    canvas_out->size = w * h * 4;
    canvas_out->width = w;
    canvas_out->height = h;
    canvas_out->stride = w * 4; /* 字节 */
    canvas_out->pixel_format = GRAPHICS_OSD_PIXEL_FORMAT;
    return 0;

out_mmz:
    RK_MPI_MMZ_Free(priv->vo_blk);
    priv->vo_blk = NULL;
out_chn:
    RK_MPI_VO_DisableChn(RK_VO_UI_LAYER, RK_VO_UI_CHN);
out_layer:
    RK_MPI_VO_DisableLayer(RK_VO_UI_LAYER);
out_unbind:
    RK_MPI_VO_UnBindLayer(RK_VO_UI_LAYER, priv->vo_dev);
    priv->vo_ready = 0;
    return -EIO;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int rk_gfx_osd_create(graphics_device_t *dev, const osd_config_t *cfg,
                             graphics_buffer_t *canvas_out) {
    rk_graphics_priv_t *priv = (rk_graphics_priv_t *)dev->priv;
    int rc;

    if (cfg == NULL || canvas_out == NULL)
        return -EINVAL;
    if (cfg->rect.w < RK_OSD_ALIGN || cfg->rect.h < RK_OSD_ALIGN)
        return -EINVAL;
    if (priv->attached || priv->vo_ready)
        return -EBUSY; /* v1 同一设备只支持一个 OSD 实例 */

    if (rk_mpi_sys_acquire() != 0)
        return -EIO;
    priv->sys_acquired = 1;

    priv->target = cfg->target;
    if (cfg->target == OSD_TARGET_VO)
        rc = rk_osd_create_vo(priv, cfg, canvas_out);
    else
        rc = rk_osd_create_venc(priv, cfg, canvas_out);
    if (rc != 0) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
        return rc;
    }

    priv->flush_count = 0;
    return 0;
}

static int rk_gfx_osd_flush(graphics_device_t *dev, const graphics_rect_t *dirty) {
    rk_graphics_priv_t *priv = (rk_graphics_priv_t *)dev->priv;
    (void)dirty; /* 两路径都无局部更新粒度，整幅生效 */

    if (priv->target == OSD_TARGET_VO) {
        VIDEO_FRAME_INFO_S vf;
        RK_S32 ret;

        if (!priv->vo_ready || priv->vo_blk == NULL)
            return -EINVAL;
        /* CPU 写完 canvas，VO 硬件读前先刷 cache */
        RK_MPI_SYS_MmzFlushCache(priv->vo_blk, RK_FALSE);

        memset(&vf, 0, sizeof(vf));
        vf.stVFrame.pMbBlk = priv->vo_blk;
        vf.stVFrame.u32Width = priv->canvas_w;
        vf.stVFrame.u32Height = priv->canvas_h;
        vf.stVFrame.u32VirWidth = priv->canvas_w;
        vf.stVFrame.u32VirHeight = priv->canvas_h;
        /* LVGL ARGB8888 内存序（LE）即 B,G,R,A → RK_FMT_BGRA8888；
         * alpha 语义：LVGL 侧渲染为预乘（LvPort ARGB8888_PREMULTIPLIED） */
        vf.stVFrame.enPixelFormat = RK_FMT_BGRA8888;
        vf.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
        ret = RK_MPI_VO_SendFrame(RK_VO_UI_LAYER, RK_VO_UI_CHN, &vf, 1000);
        /* 前 3 次 + 每 30 次 + 出错打点（stderr 无缓冲，板上可观测） */
        if (ret != RK_SUCCESS || priv->flush_count < 3 || priv->flush_count % 30 == 0)
            fprintf(stderr, "rk_graphics: VO SendFrame #%llu ret=%#x\n",
                    (unsigned long long)priv->flush_count, ret);
        if (ret != RK_SUCCESS)
            return -EIO;
    } else {
        if (!priv->attached)
            return -EINVAL;
        RGN_CANVAS_INFO_S canvas;
        memset(&canvas, 0, sizeof(canvas));
        if (RK_MPI_RGN_GetCanvasInfo(priv->handle, &canvas) != RK_SUCCESS)
            return -EIO;
        if (RK_MPI_RGN_UpdateCanvas(priv->handle) != RK_SUCCESS)
            return -EIO;
    }
    priv->flush_count++;
    return 0;
}

static int rk_gfx_osd_destroy(graphics_device_t *dev) {
    rk_graphics_priv_t *priv = (rk_graphics_priv_t *)dev->priv;

    if (priv->attached) {
        priv->attached = 0;
        RK_MPI_RGN_DetachFromChn(priv->handle, &priv->chn);
        RK_MPI_RGN_Destroy(priv->handle);
    }
    if (priv->vo_ready) {
        priv->vo_ready = 0;
        RK_MPI_VO_DisableChn(RK_VO_UI_LAYER, RK_VO_UI_CHN);
        RK_MPI_VO_DisableLayer(RK_VO_UI_LAYER);
        RK_MPI_VO_UnBindLayer(RK_VO_UI_LAYER, priv->vo_dev);
        /* 不 RK_MPI_VO_Disable(dev)：VO dev 归 display HAL 管 */
    }
    if (priv->vo_blk != NULL) {
        RK_MPI_MMZ_Free(priv->vo_blk);
        priv->vo_blk = NULL;
        priv->vo_vir = NULL;
    }
    if (priv->sys_acquired) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
    }
    return 0;
}

static const graphics_device_ops_t rk_gfx_ops = {
    .blit = rk_gfx_blit,
    .fill = rk_gfx_fill,
    .osd_create = rk_gfx_osd_create,
    .osd_flush = rk_gfx_osd_flush,
    .osd_destroy = rk_gfx_osd_destroy,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int rk_gfx_close(hw_device_t *device) {
    graphics_device_t *dev = (graphics_device_t *)device;

    if (dev == NULL)
        return 0;
    if (dev->priv != NULL) {
        rk_gfx_osd_destroy(dev); /* 确保 RGN/VO 资源与 SYS 引用收回 */
        free(dev->priv);
    }
    free(dev);
    return 0;
}

static int rk_gfx_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    graphics_device_t *dev;
    rk_graphics_priv_t *priv;

    (void)id; /* v1 单实例（osd_create 限定同一设备一个 OSD） */
    if (device == NULL)
        return -EINVAL;

    dev = (graphics_device_t *)calloc(1, sizeof(*dev));
    priv = (rk_graphics_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }
    priv->handle = RK_OSD_HANDLE;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = GRAPHICS_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = rk_gfx_close;
    dev->ops = &rk_gfx_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出（hal.rockchip.rv1126b.so，dlsym("HMI_graphics")）
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t rk_gfx_methods = {
    .open = rk_gfx_open,
};

struct hw_module_t HMI_graphics = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = GRAPHICS_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = GRAPHICS_HARDWARE_MODULE_ID,
    .name = "Rockchip RV1126B Graphics HAL (RGN overlay for VENC / VO UI layer)",
    .author = "DarkOS",
    .methods = &rk_gfx_methods,
};
