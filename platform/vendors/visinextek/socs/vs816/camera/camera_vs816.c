#ifndef _GNU_SOURCE
#define _GNU_SOURCE
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

#include <sample_common.h>
#include <vs_mal_mipi_rx.h>
#include <vs_mal_sys.h>
#include <vs_mal_venc.h>
#include <vs_mal_vii.h>
#include <vs_mal_vpp.h>

#include "common/vs_sys_guard.h"
#include "common/vs_venc_stream.h"

#define VS_CAM_MAX_INSTANCES 4

extern sample_sensor_type_e g_sensor_type[VII_MAX_ROUTE_NUM];

typedef struct vs_camera_priv {
    int index;
    int pipe;
    int chn;
    int vpp_grp;
    int vpp_chn;
    int venc_chn;
    camera_format_t format;
    sample_vii_cfg_s vii_cfg;
    int sys_acquired;
    int route_started;
    int vpp_created;
    int vpp_chn_enabled;
    int vpp_started;
    int vii_vpp_bound;
    int streaming;
    int encoded;
    int venc_created;
    int venc_bound;
    volatile int thread_run;
    pthread_t thread;
    camera_frame_cb callback;
    void *callback_ctx;
    pthread_mutex_t lock;
    uint32_t sequence;
} vs_camera_priv_t;

static void route_stop(vs_camera_priv_t *priv);

static int parse_index(const char *id) {
    char *end;
    long index;
    if (id == NULL || strcmp(id, "camera") == 0 || strcmp(id, "camera0") == 0)
        return 0;
    if (strncmp(id, "camera", 6) != 0 || id[6] == '\0')
        return -1;
    index = strtol(id + 6, &end, 10);
    return (*end == '\0' && index >= 0 && index < VS_CAM_MAX_INSTANCES) ? (int)index : -1;
}

static int env_channel(int index, const char *suffix, int fallback) {
    char key[64];
    char *end;
    const char *value;
    long result;
    snprintf(key, sizeof(key), "DARKOS_CAMERA%d_%s", index, suffix);
    value = getenv(key);
    if (value == NULL || *value == '\0')
        return fallback;
    result = strtol(value, &end, 10);
    return (*end == '\0' && result >= 0) ? (int)result : fallback;
}

static uint32_t frame_size(const vs_video_frame_s *frame) {
    uint32_t stride = frame->stride[0] != 0 ? frame->stride[0] : frame->width;
    return stride * frame->height * 3u / 2u;
}

static void export_frame(vs_camera_priv_t *priv, const vs_video_frame_info_s *source,
                         camera_frame_t *out) {
    memset(out, 0, sizeof(*out));
    out->index = priv->sequence++;
    out->fd = -1; /* MAL exports physical/virtual addresses, not dma-buf fds. */
    out->data = (void *)(uintptr_t)source->frame.virt_addr[0];
    out->size = frame_size(&source->frame);
    out->width = source->frame.width;
    out->height = source->frame.height;
    out->pixel_format = CAMERA_PIX_FMT_NV12;
    out->stride = source->frame.stride[0] != 0 ? source->frame.stride[0] : source->frame.width;
    out->timestamp_ns = source->frame.pts * 1000ull;
    out->priv = (void *)source;
}

static int route_start(vs_camera_priv_t *priv) {
    sample_vii_route_cfg_s *route;
    sample_vii_phys_chn_cfg_s *chn;
    vs_vpp_grp_attr_s vpp_grp_attr;
    vs_vpp_chn_attr_s vpp_chn_attr;
    vs_chn_s vii_source;
    vs_chn_s vpp_sink;
    vs_int32_t mal_ret;

    if (priv->route_started)
        return 0;
    if (vs_sys_acquire(3840, 2160) != 0)
        return -EIO;
    priv->sys_acquired = 1;

    /* The vendor sample resets the sensor after VB setup and before VII. */
    vs_mal_mipi_rx_sensor_reset(priv->index);
    usleep(500);
    vs_mal_mipi_rx_sensor_unreset(priv->index);

    memset(&priv->vii_cfg, 0, sizeof(priv->vii_cfg));
    /* VS816's vendor VENC pipeline is VII(offline) -> VPP(online) -> VENC. */
    priv->vii_cfg.vii_vpp_mode = E_VII_OFFLINE_VPP_ONLINE;
    priv->vii_cfg.route_num = 1;
    route = &priv->vii_cfg.route_cfg[0];
    sample_common_vii_default_cfg_get(priv->index, route);
    route->dev_id = priv->index;
    route->pipe_id[0] = priv->pipe;
    route->pipe_cfg[0].pipe_id = priv->pipe;
    route->pipe_cfg[0].pipe_attr.compress_mode = E_COMPRESS_MODE_NONE;
    if (route->pipe_cfg[0].phys_chn_num == 0) {
        vs_sys_release();
        priv->sys_acquired = 0;
        return -ENODEV;
    }
    chn = &route->pipe_cfg[0].phys_chn_cfg[0];
    chn->chn_id = priv->chn;
    /* Keep the SDK-provided sensor size and VII output format. VPP owns scaling. */
    chn->chn_attr.compress_mode = E_COMPRESS_MODE_NONE;
    chn->chn_attr.depth = 4;
    chn->chn_attr.framerate.src_framerate = priv->format.fps;
    chn->chn_attr.framerate.dst_framerate = priv->format.fps;

    if (sample_common_vii_start(&priv->vii_cfg) != VS_SUCCESS) {
        vs_sys_release();
        priv->sys_acquired = 0;
        return -EIO;
    }
    priv->route_started = 1;

    memset(&vpp_grp_attr, 0, sizeof(vpp_grp_attr));
    vpp_grp_attr.max_width = route->pipe_cfg[0].pipe_attr.image_size.width;
    vpp_grp_attr.max_height = route->pipe_cfg[0].pipe_attr.image_size.height;
    vpp_grp_attr.pixel_format = E_PIXEL_FORMAT_YVU_420SP;
    vpp_grp_attr.dynamic_range = E_DYNAMIC_RANGE_SDR8;
    vpp_grp_attr.framerate.src_framerate = priv->format.fps;
    vpp_grp_attr.framerate.dst_framerate = priv->format.fps;
    mal_ret = vs_mal_vpp_grp_create(priv->vpp_grp, &vpp_grp_attr);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: vpp group create failed: 0x%x\n", mal_ret);
        goto fail;
    }
    priv->vpp_created = 1;

    memset(&vpp_chn_attr, 0, sizeof(vpp_chn_attr));
    vpp_chn_attr.chn_mode = E_VPP_CHN_MODE_USER;
    vpp_chn_attr.width = priv->format.width;
    vpp_chn_attr.height = priv->format.height;
    vpp_chn_attr.video_format = E_VIDEO_FORMAT_LINEAR;
    vpp_chn_attr.pixel_format = E_PIXEL_FORMAT_YVU_420SP;
    vpp_chn_attr.dynamic_range = E_DYNAMIC_RANGE_SDR8;
    vpp_chn_attr.compress_mode = E_COMPRESS_MODE_NONE;
    vpp_chn_attr.framerate.src_framerate = priv->format.fps;
    vpp_chn_attr.framerate.dst_framerate = priv->format.fps;
    vpp_chn_attr.aspect_ratio.mode = E_ASPECT_RATIO_MODE_NONE;
    mal_ret = vs_mal_vpp_chn_attr_set(priv->vpp_grp, priv->vpp_chn, &vpp_chn_attr);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: vpp channel attr failed: 0x%x\n", mal_ret);
        goto fail;
    }
    mal_ret = vs_mal_vpp_chn_enable(priv->vpp_grp, priv->vpp_chn);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: vpp channel enable failed: 0x%x\n", mal_ret);
        goto fail;
    }
    priv->vpp_chn_enabled = 1;
    mal_ret = vs_mal_vpp_grp_start(priv->vpp_grp);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: vpp group start failed: 0x%x\n", mal_ret);
        goto fail;
    }
    priv->vpp_started = 1;

    vii_source = (vs_chn_s){E_MOD_ID_VII, (vs_uint32_t)priv->pipe,
                            (vs_uint32_t)priv->chn};
    vpp_sink = (vs_chn_s){E_MOD_ID_VPP, (vs_uint32_t)priv->vpp_grp, 0};
    mal_ret = vs_mal_sys_bind(&vii_source, &vpp_sink);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: VII-to-VPP bind failed: 0x%x\n", mal_ret);
        goto fail;
    }
    priv->vii_vpp_bound = 1;
    return 0;

fail:
    route_stop(priv);
    return -EIO;
}

static void route_stop(vs_camera_priv_t *priv) {
    vs_chn_s vii_source = {E_MOD_ID_VII, (vs_uint32_t)priv->pipe,
                           (vs_uint32_t)priv->chn};
    vs_chn_s vpp_sink = {E_MOD_ID_VPP, (vs_uint32_t)priv->vpp_grp, 0};
    if (priv->vii_vpp_bound) {
        vs_mal_sys_unbind(&vii_source, &vpp_sink);
        priv->vii_vpp_bound = 0;
    }
    if (priv->vpp_started) {
        vs_mal_vpp_grp_stop(priv->vpp_grp);
        priv->vpp_started = 0;
    }
    if (priv->vpp_chn_enabled) {
        vs_mal_vpp_chn_disable(priv->vpp_grp, priv->vpp_chn);
        priv->vpp_chn_enabled = 0;
    }
    if (priv->vpp_created) {
        vs_mal_vpp_grp_destroy(priv->vpp_grp);
        priv->vpp_created = 0;
    }
    if (priv->route_started) {
        sample_common_vii_stop(&priv->vii_cfg);
        priv->route_started = 0;
    }
    if (priv->sys_acquired) {
        vs_sys_release();
        priv->sys_acquired = 0;
    }
}

static void *capture_thread(void *arg) {
    vs_camera_priv_t *priv = arg;
    while (priv->thread_run) {
        vs_video_frame_info_s source;
        camera_frame_t frame;
        camera_frame_cb callback;
        void *ctx;
        memset(&source, 0, sizeof(source));
        if (vs_mal_vii_chn_frame_acquire(priv->pipe, priv->chn, &source, 500) != VS_SUCCESS)
            continue;
        export_frame(priv, &source, &frame);
        pthread_mutex_lock(&priv->lock);
        callback = priv->callback;
        ctx = priv->callback_ctx;
        pthread_mutex_unlock(&priv->lock);
        if (callback != NULL && callback(ctx, &frame) != 0)
            priv->thread_run = 0;
        vs_mal_vii_chn_frame_release(priv->pipe, priv->chn, &source);
    }
    return NULL;
}

static int get_capabilities(camera_device_t *dev, camera_caps_t *caps) {
    vs_camera_priv_t *priv = dev->priv;
    const char *name;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->min_width = 128; caps->min_height = 128;
    caps->max_width = 3840; caps->max_height = 2160;
    caps->supported_formats = CAMERA_CAPS_FMT_NV12;
    caps->camera_type = CAMERA_TYPE_VISUAL;
    name = sample_common_sensor_type_name_get(g_sensor_type[priv->index]);
    if (name != NULL)
        snprintf(caps->sensor_name, sizeof(caps->sensor_name), "%s", name);
    return 0;
}

static int set_format(camera_device_t *dev, const camera_format_t *format) {
    vs_camera_priv_t *priv = dev->priv;
    if (format == NULL || format->pixel_format != CAMERA_PIX_FMT_NV12 ||
        format->width < 128 || format->height < 128 ||
        format->width > 3840 || format->height > 2160)
        return -EINVAL;
    if (priv->streaming || priv->encoded)
        return -EBUSY;
    priv->format = *format;
    if (priv->format.fps == 0)
        priv->format.fps = 30;
    return 0;
}

static int get_format(camera_device_t *dev, camera_format_t *format) {
    if (format == NULL)
        return -EINVAL;
    *format = ((vs_camera_priv_t *)dev->priv)->format;
    return 0;
}

static int start(camera_device_t *dev) {
    vs_camera_priv_t *priv = dev->priv;
    int rc;
    if (priv->streaming || priv->encoded)
        return -EBUSY;
    rc = route_start(priv);
    if (rc != 0)
        return rc;
    priv->thread_run = 1;
    if (pthread_create(&priv->thread, NULL, capture_thread, priv) != 0) {
        priv->thread_run = 0;
        route_stop(priv);
        return -errno;
    }
    priv->streaming = 1;
    return 0;
}

static int stop(camera_device_t *dev) {
    vs_camera_priv_t *priv = dev->priv;
    if (!priv->streaming)
        return 0;
    priv->thread_run = 0;
    pthread_join(priv->thread, NULL);
    priv->streaming = 0;
    route_stop(priv);
    return 0;
}

static int set_callback(camera_device_t *dev, camera_frame_cb callback, void *ctx) {
    vs_camera_priv_t *priv = dev->priv;
    pthread_mutex_lock(&priv->lock);
    priv->callback = callback;
    priv->callback_ctx = ctx;
    pthread_mutex_unlock(&priv->lock);
    return 0;
}

static int capture(camera_device_t *dev, camera_frame_t *out, int timeout_ms) {
    vs_camera_priv_t *priv = dev->priv;
    vs_video_frame_info_s source;
    camera_frame_t view;
    uint32_t capacity;
    if (out == NULL || out->data == NULL || out->size == 0)
        return -EINVAL;
    if (!priv->streaming)
        return -EPIPE;
    capacity = out->size;
    memset(&source, 0, sizeof(source));
    if (vs_mal_vii_chn_frame_acquire(priv->pipe, priv->chn, &source, timeout_ms) != VS_SUCCESS)
        return -ETIMEDOUT;
    export_frame(priv, &source, &view);
    if (view.size > capacity) {
        out->size = view.size;
        vs_mal_vii_chn_frame_release(priv->pipe, priv->chn, &source);
        return -ENOSPC;
    }
    memcpy(out->data, view.data, view.size);
    view.data = out->data;
    view.priv = NULL;
    *out = view;
    vs_mal_vii_chn_frame_release(priv->pipe, priv->chn, &source);
    return 0;
}

static int control_set(camera_device_t *dev, uint32_t id, int32_t value) {
    (void)dev; (void)id; (void)value;
    return -ENOTSUP;
}
static int control_get(camera_device_t *dev, uint32_t id, int32_t *value) {
    (void)dev; (void)id; (void)value;
    return -ENOTSUP;
}
static int preview_start(camera_device_t *dev, uint32_t width, uint32_t height) {
    (void)dev; (void)width; (void)height;
    return -ENOTSUP;
}
static int preview_stop(camera_device_t *dev) { (void)dev; return 0; }

static int venc_create(vs_camera_priv_t *priv, const codec_format_t *cfg) {
    vs_venc_chn_attr_s attr;
    vs_venc_start_param_s start_param;
    vs_chn_s source = {E_MOD_ID_VPP, (vs_uint32_t)priv->vpp_grp,
                       (vs_uint32_t)priv->vpp_chn};
    vs_chn_s sink = {E_MOD_ID_VENC, 0, (vs_uint32_t)priv->venc_chn};
    uint32_t fps = cfg->fps != 0 ? cfg->fps : 30;
    uint32_t gop = cfg->gop != 0 ? cfg->gop : fps;
    uint32_t bitrate = cfg->bitrate_bps != 0 ? cfg->bitrate_bps : 4000000;
    vs_int32_t mal_ret;
    memset(&attr, 0, sizeof(attr));
    attr.enc_attr.type = E_PT_TYPE_H264;
    attr.enc_attr.max_frame_width = cfg->width;
    attr.enc_attr.max_frame_height = cfg->height;
    attr.enc_attr.frame_width = cfg->width;
    attr.enc_attr.frame_height = cfg->height;
    /* Match the vendor sample: four bytes per pixel, rounded to a 4 KiB page. */
    attr.enc_attr.stream_buf_size =
        (cfg->width * cfg->height * 4u + 4095u) & ~4095u;
    attr.enc_attr.stream_mode = E_VENC_STREAM_MODE_FRAME;
    attr.enc_attr.profile = E_VENC_PROFILE_H264_MAIN;
    attr.brc_attr.brc_mode = E_VENC_BRC_MODE_H264_CBR;
    attr.brc_attr.h264_cbr.gop = gop;
    attr.brc_attr.h264_cbr.bitrate_window = 1;
    attr.brc_attr.h264_cbr.src_framerate = fps;
    attr.brc_attr.h264_cbr.dst_framerate = fps;
    attr.brc_attr.h264_cbr.bitrate = bitrate / 1000u;
    attr.gop_attr.mode = E_VENC_GOP_MODE_NORMP;
    attr.gop_attr.normp.qpdelta_i_p = 2;
    mal_ret = vs_mal_venc_chn_create(priv->venc_chn, &attr);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: VENC channel create failed: 0x%x\n", mal_ret);
        return -EIO;
    }
    priv->venc_created = 1;
    memset(&start_param, 0, sizeof(start_param));
    start_param.rcvframe_num = (vs_uint32_t)-1;
    mal_ret = vs_mal_venc_chn_start(priv->venc_chn, &start_param);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: VENC channel start failed: 0x%x\n", mal_ret);
        return -EIO;
    }
    mal_ret = vs_mal_sys_bind(&source, &sink);
    if (mal_ret != VS_SUCCESS) {
        fprintf(stderr, "vs816-camera: VPP-to-VENC bind failed: 0x%x\n", mal_ret);
        return -EIO;
    }
    priv->venc_bound = 1;
    return 0;
}

static void venc_destroy(vs_camera_priv_t *priv) {
    vs_chn_s source = {E_MOD_ID_VPP, (vs_uint32_t)priv->vpp_grp,
                       (vs_uint32_t)priv->vpp_chn};
    vs_chn_s sink = {E_MOD_ID_VENC, 0, (vs_uint32_t)priv->venc_chn};
    if (priv->venc_bound) {
        vs_mal_sys_unbind(&source, &sink);
        priv->venc_bound = 0;
    }
    if (priv->venc_created) {
        vs_mal_venc_chn_stop(priv->venc_chn);
        vs_mal_venc_chn_destroy(priv->venc_chn);
        priv->venc_created = 0;
    }
}

static int encoded_start(camera_device_t *dev, const codec_format_t *cfg) {
    vs_camera_priv_t *priv = dev->priv;
    int rc;
    if (cfg == NULL || cfg->codec != CODEC_ID_H264 || cfg->pixel_format != CAMERA_PIX_FMT_NV12)
        return -EINVAL;
    if (priv->streaming || priv->encoded)
        return -EBUSY;
    priv->format = (camera_format_t){cfg->width, cfg->height, CAMERA_PIX_FMT_NV12,
                                     cfg->fps != 0 ? cfg->fps : 30};
    rc = route_start(priv);
    if (rc == 0)
        rc = venc_create(priv, cfg);
    if (rc != 0) {
        venc_destroy(priv);
        route_stop(priv);
        return rc;
    }
    priv->encoded = 1;
    return 0;
}

static int encoded_packet(camera_device_t *dev, codec_buffer_t *packet, int timeout_ms) {
    vs_camera_priv_t *priv = dev->priv;
    if (!priv->encoded)
        return -EPIPE;
    return vs_venc_packet_acquire(priv->venc_chn, packet, timeout_ms);
}

static int encoded_stop(camera_device_t *dev) {
    vs_camera_priv_t *priv = dev->priv;
    if (!priv->encoded && !priv->venc_created)
        return 0;
    venc_destroy(priv);
    priv->encoded = 0;
    route_stop(priv);
    return 0;
}

static const camera_device_ops_t ops = {
    .get_capabilities = get_capabilities, .set_format = set_format,
    .get_format = get_format, .start = start, .stop = stop,
    .set_frame_callback = set_callback, .capture = capture,
    .set_control = control_set, .get_control = control_get,
    .preview_start = preview_start, .preview_stop = preview_stop,
    .encoded_start = encoded_start, .encoded_get_packet = encoded_packet,
    .encoded_stop = encoded_stop,
};

static int close_device(hw_device_t *hw) {
    camera_device_t *dev = (camera_device_t *)hw;
    vs_camera_priv_t *priv;
    if (dev == NULL)
        return 0;
    priv = dev->priv;
    stop(dev);
    encoded_stop(dev);
    pthread_mutex_destroy(&priv->lock);
    free(priv); free(dev);
    return 0;
}

static int open_device(const hw_module_t *module, const char *id, hw_device_t **out) {
    camera_device_t *dev;
    vs_camera_priv_t *priv;
    int index = parse_index(id);
    if (out == NULL || index < 0)
        return -EINVAL;
    dev = calloc(1, sizeof(*dev));
    priv = calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) { free(dev); free(priv); return -ENOMEM; }
    priv->index = index;
    priv->pipe = env_channel(index, "VI_PIPE", index);
    priv->chn = env_channel(index, "VI_CHN", 0);
    priv->vpp_grp = env_channel(index, "VPP_GRP", index);
    priv->vpp_chn = env_channel(index, "VPP_CHN", 0);
    priv->venc_chn = env_channel(index, "VENC_CHN", index);
    priv->format = (camera_format_t){1920, 1080, CAMERA_PIX_FMT_NV12, 30};
    pthread_mutex_init(&priv->lock, NULL);
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CAMERA_DEVICE_API_VERSION_1_1;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = close_device;
    dev->ops = &ops;
    dev->priv = priv;
    *out = &dev->common;
    return 0;
}

static hw_module_methods_t methods = {.open = open_device};
hw_module_t HMI_camera = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = CAMERA_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = CAMERA_HARDWARE_MODULE_ID,
    .name = "Visinextek VS816 Camera HAL (MAL VII/ISP/VENC)",
    .author = "DarkOS", .methods = &methods,
};
