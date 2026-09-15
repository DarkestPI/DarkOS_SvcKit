/*
 * 主机参考实现（host_x86 变体）：GRAPHICS 2D 设备的软件模拟。
 *
 * v1 只实现 OSD 三件套（blit/fill 返回 -ENOTSUP）：malloc 一块 ARGB8888
 * canvas 透出给上层直写（当前上层是 LVGL 的 DIRECT 渲染），osd_flush
 * 记账计数；环境变量 DARKOS_OSD_DUMP=1 时每次 flush 把 canvas 转 RGB
 * 写成二进制 PPM 到 /tmp/osd_dump.ppm（覆盖写），供联调目检/像素验证。
 *
 * 与硬件 SoC HAL 的 OSD canvas 实现同一套 graphics_device_ops。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 */

#include <hardware/hardware.h>
#include <graphics/IGraphics.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_OSD_DUMP_PATH "/tmp/osd_dump.ppm"

typedef struct host_graphics_priv {
    uint8_t *canvas;      /* ARGB8888，w*h*4 */
    uint32_t w, h;
    uint64_t flush_count; /* flush 记账 */
    int dump;             /* DARKOS_OSD_DUMP=1 */
} host_graphics_priv_t;

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_gfx_blit(graphics_device_t *dev, const graphics_buffer_t *src,
                         const graphics_rect_t *src_rect, const graphics_buffer_t *dst,
                         const graphics_rect_t *dst_rect, uint32_t transform) {
    (void)dev;
    (void)src;
    (void)src_rect;
    (void)dst;
    (void)dst_rect;
    (void)transform;
    return -ENOTSUP; /* v1 未实现，待 RGA 联调需求落地 */
}

static int host_gfx_fill(graphics_device_t *dev, graphics_buffer_t *dst,
                         const graphics_rect_t *rect, uint32_t color) {
    (void)dev;
    (void)dst;
    (void)rect;
    (void)color;
    return -ENOTSUP;
}

static int host_gfx_osd_create(graphics_device_t *dev, const osd_config_t *cfg,
                               graphics_buffer_t *canvas_out) {
    host_graphics_priv_t *priv = (host_graphics_priv_t *)dev->priv;

    if (cfg == NULL || canvas_out == NULL)
        return -EINVAL;
    if (cfg->rect.w == 0 || cfg->rect.h == 0)
        return -EINVAL;
    if (priv->canvas != NULL)
        return -EBUSY; /* v1 同一设备只支持一个 OSD 实例 */

    priv->w = cfg->rect.w;
    priv->h = cfg->rect.h;
    priv->canvas = (uint8_t *)calloc((size_t)priv->w * priv->h, 4);
    if (priv->canvas == NULL)
        return -ENOMEM;
    priv->flush_count = 0;

    memset(canvas_out, 0, sizeof(*canvas_out));
    canvas_out->fd = -1;
    canvas_out->data = priv->canvas;
    canvas_out->size = priv->w * priv->h * 4;
    canvas_out->width = priv->w;
    canvas_out->height = priv->h;
    canvas_out->stride = priv->w * 4; /* 字节 */
    canvas_out->pixel_format = GRAPHICS_OSD_PIXEL_FORMAT;
    return 0;
}

/* canvas（ARGB8888）转 RGB 写 PPM（P6，覆盖写） */
static void host_gfx_dump_ppm(host_graphics_priv_t *priv) {
    FILE *fp = fopen(HOST_OSD_DUMP_PATH, "wb");
    if (fp == NULL)
        return;
    fprintf(fp, "P6\n%u %u\n255\n", priv->w, priv->h);
    const uint8_t *px = priv->canvas;
    for (uint32_t i = 0; i < priv->w * priv->h; i++, px += 4) {
        /* canvas 为预乘 alpha（premultiplied）ARGB8888，内存序（LE）B,G,R,A：
         * RGB 已被 alpha 调制，直接重排字节即是黑底合成结果 */
        uint8_t rgb[3] = {px[2], px[1], px[0]};
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
}

static int host_gfx_osd_flush(graphics_device_t *dev, const graphics_rect_t *dirty) {
    host_graphics_priv_t *priv = (host_graphics_priv_t *)dev->priv;
    (void)dirty; /* host 侧无翻页概念，整幅即生效 */

    if (priv->canvas == NULL)
        return -EINVAL;
    priv->flush_count++;
    if (priv->dump) {
        host_gfx_dump_ppm(priv);
        /* dump 模式即调试模式：逐次打计数（stderr 无缓冲，重定向不丢） */
        fprintf(stderr, "[graphics] osd flush #%llu (%ux%u)\n",
                (unsigned long long)priv->flush_count, priv->w, priv->h);
    } else if (priv->flush_count % 30 == 1) { /* 约每秒一条（30fps 渲染时），防刷屏 */
        fprintf(stderr, "[graphics] osd flush #%llu (%ux%u)\n",
                (unsigned long long)priv->flush_count, priv->w, priv->h);
    }
    return 0;
}

static int host_gfx_osd_destroy(graphics_device_t *dev) {
    host_graphics_priv_t *priv = (host_graphics_priv_t *)dev->priv;

    free(priv->canvas);
    priv->canvas = NULL;
    priv->w = priv->h = 0;
    return 0;
}

static const graphics_device_ops_t host_gfx_ops = {
    .blit = host_gfx_blit,
    .fill = host_gfx_fill,
    .osd_create = host_gfx_osd_create,
    .osd_flush = host_gfx_osd_flush,
    .osd_destroy = host_gfx_osd_destroy,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_gfx_close(hw_device_t *device) {
    graphics_device_t *dev = (graphics_device_t *)device;

    if (dev == NULL)
        return 0;
    if (dev->priv != NULL) {
        host_gfx_osd_destroy(dev); /* 确保 canvas 收回 */
        free(dev->priv);
    }
    free(dev);
    return 0;
}

static int host_gfx_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    graphics_device_t *dev;
    host_graphics_priv_t *priv;

    (void)id;
    if (device == NULL)
        return -EINVAL;

    dev = (graphics_device_t *)calloc(1, sizeof(*dev));
    priv = (host_graphics_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    const char *dump = getenv("DARKOS_OSD_DUMP");
    priv->dump = (dump != NULL && strcmp(dump, "1") == 0);

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_gfx_close;
    dev->ops = &host_gfx_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_gfx_methods = {
    .open = host_gfx_open,
};

struct hw_module_t HMI_graphics = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = GRAPHICS_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = GRAPHICS_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Graphics HAL (software emulated)",
    .author = "DarkOS",
    .methods = &host_gfx_methods,
};
