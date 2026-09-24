#include <codec/ICodec.h>
#include <hardware/hardware.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vs_buffer.h>
#include <vs_mal_mmz.h>
#include <vs_mal_vbm.h>
#include <vs_mal_vdec.h>
#include <vs_mal_venc.h>

#include "common/vs_sys_guard.h"
#include "common/vs_venc_stream.h"

#define VS_CODEC_MAX_INSTANCES 8

typedef struct vs_codec_priv {
    int index;
    int venc_chn;
    int vdec_chn;
    codec_format_t format;
    int sys_acquired;
    int encoding;
    int decoding;
} vs_codec_priv_t;

static int parse_index(const char *id) {
    char *end;
    long index;
    if (id == NULL || strcmp(id, "codec") == 0 || strcmp(id, "codec0") == 0)
        return 0;
    if (strncmp(id, "codec", 5) != 0 || id[5] == '\0')
        return -1;
    index = strtol(id + 5, &end, 10);
    return (*end == '\0' && index >= 0 && index < VS_CODEC_MAX_INSTANCES) ? (int)index : -1;
}

static void fill_venc_attr(const codec_format_t *format, vs_venc_chn_attr_s *attr) {
    uint32_t fps = format->fps != 0 ? format->fps : 30;
    uint32_t gop = format->gop != 0 ? format->gop : fps;
    uint32_t bitrate = format->bitrate_bps != 0 ? format->bitrate_bps : 4000000;
    memset(attr, 0, sizeof(*attr));
    attr->enc_attr.type = E_PT_TYPE_H264;
    attr->enc_attr.max_frame_width = format->width;
    attr->enc_attr.max_frame_height = format->height;
    attr->enc_attr.frame_width = format->width;
    attr->enc_attr.frame_height = format->height;
    attr->enc_attr.stream_buf_size = format->width * format->height;
    attr->enc_attr.stream_mode = E_VENC_STREAM_MODE_FRAME;
    attr->enc_attr.profile = E_VENC_PROFILE_H264_MAIN;
    attr->brc_attr.brc_mode = E_VENC_BRC_MODE_H264_CBR;
    attr->brc_attr.h264_cbr.gop = gop;
    attr->brc_attr.h264_cbr.bitrate_window = 1;
    attr->brc_attr.h264_cbr.src_framerate = fps;
    attr->brc_attr.h264_cbr.dst_framerate = fps;
    attr->brc_attr.h264_cbr.bitrate = bitrate / 1000u;
    attr->gop_attr.mode = E_VENC_GOP_MODE_NORMP;
    attr->gop_attr.normp.qpdelta_i_p = 2;
}

static int venc_start(vs_codec_priv_t *priv) {
    vs_venc_chn_attr_s attr;
    vs_venc_start_param_s start_param;
    fill_venc_attr(&priv->format, &attr);
    if (vs_mal_venc_chn_create(priv->venc_chn, &attr) != VS_SUCCESS)
        return -EIO;
    memset(&start_param, 0, sizeof(start_param));
    start_param.rcvframe_num = (vs_uint32_t)-1;
    if (vs_mal_venc_chn_start(priv->venc_chn, &start_param) != VS_SUCCESS) {
        vs_mal_venc_chn_destroy(priv->venc_chn);
        return -EIO;
    }
    priv->encoding = 1;
    return 0;
}

static void venc_stop(vs_codec_priv_t *priv) {
    if (!priv->encoding)
        return;
    vs_mal_venc_chn_stop(priv->venc_chn);
    vs_mal_venc_chn_destroy(priv->venc_chn);
    priv->encoding = 0;
}

static int vdec_start(vs_codec_priv_t *priv) {
    vs_vdec_mod_param_s mod;
    vs_vdec_chn_attr_s attr;
    vs_vdec_chn_param_s param;
    memset(&mod, 0, sizeof(mod));
    if (vs_mal_vdec_mod_param_get(&mod) != VS_SUCCESS)
        return -EIO;
    mod.vb_source = VB_SOURCE_PRIVATE;
    if (vs_mal_vdec_mod_param_set(&mod) != VS_SUCCESS)
        return -EIO;
    memset(&attr, 0, sizeof(attr));
    attr.type = E_PT_TYPE_H264;
    attr.input_mode = E_VDEC_INPUT_MODE_FRAME;
    attr.width = priv->format.width;
    attr.height = priv->format.height;
    attr.stream_buf_size = priv->format.width * priv->format.height * 2u;
    attr.frame_buf_cnt = 6;
    attr.frame_buf_size = vdec_frame_buffer_size_get(
        E_PT_TYPE_H264, priv->format.width, priv->format.height,
        E_VIDEO_FORMAT_LINEAR, E_PIXEL_FORMAT_YUV_420SP, E_COMPRESS_MODE_NONE);
    attr.video_attr.ref_frame_num = 4;
    if (vs_mal_vdec_chn_create(priv->vdec_chn, &attr) != VS_SUCCESS)
        return -EIO;
    if (vs_mal_vdec_chn_param_get(priv->vdec_chn, &param) != VS_SUCCESS)
        goto fail;
    param.type = E_PT_TYPE_H264;
    param.output_frame_num = 2;
    param.compress_mode = E_COMPRESS_MODE_NONE;
    param.video_format = E_VIDEO_FORMAT_LINEAR;
    param.pixel_format = E_PIXEL_FORMAT_YUV_420SP;
    if (vs_mal_vdec_chn_param_set(priv->vdec_chn, &param) != VS_SUCCESS ||
        vs_mal_vdec_chn_start(priv->vdec_chn) != VS_SUCCESS)
        goto fail;
    priv->decoding = 1;
    return 0;
fail:
    vs_mal_vdec_chn_destroy(priv->vdec_chn);
    return -EIO;
}

static void vdec_stop(vs_codec_priv_t *priv) {
    if (!priv->decoding)
        return;
    vs_mal_vdec_chn_stop(priv->vdec_chn);
    vs_mal_vdec_chn_destroy(priv->vdec_chn);
    priv->decoding = 0;
}

static int get_caps(codec_device_t *dev, codec_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_codecs = CODEC_CAPS_H264;
    caps->min_width = 128; caps->min_height = 128;
    caps->max_width = 3840; caps->max_height = 2160;
    return 0;
}

static int set_format(codec_device_t *dev, const codec_format_t *format) {
    vs_codec_priv_t *priv = dev->priv;
    if (format == NULL || format->codec != CODEC_ID_H264 ||
        format->pixel_format != 0x3231564e || format->width < 128 ||
        format->height < 128 || format->width > 3840 || format->height > 2160)
        return -EINVAL;
    if (priv->encoding || priv->decoding)
        return -EBUSY;
    priv->format = *format;
    if (priv->format.fps == 0) priv->format.fps = 30;
    if (priv->format.gop == 0) priv->format.gop = priv->format.fps;
    if (priv->format.bitrate_bps == 0) priv->format.bitrate_bps = 4000000;
    return 0;
}

static int get_format(codec_device_t *dev, codec_format_t *format) {
    if (format == NULL)
        return -EINVAL;
    *format = ((vs_codec_priv_t *)dev->priv)->format;
    return 0;
}

static int start(codec_device_t *dev) {
    vs_codec_priv_t *priv = dev->priv;
    int rc;
    if (priv->encoding)
        return -EBUSY;
    if (!priv->sys_acquired) {
        if (vs_sys_acquire(priv->format.width, priv->format.height) != 0)
            return -EIO;
        priv->sys_acquired = 1;
    }
    rc = venc_start(priv);
    if (rc != 0) {
        vs_sys_release();
        priv->sys_acquired = 0;
    }
    return rc;
}

static int stop(codec_device_t *dev) {
    vs_codec_priv_t *priv = dev->priv;
    vdec_stop(priv);
    venc_stop(priv);
    if (priv->sys_acquired) {
        vs_sys_release();
        priv->sys_acquired = 0;
    }
    return 0;
}

static int encode(codec_device_t *dev, const codec_buffer_t *in, codec_buffer_t *out,
                  int timeout_ms) {
    vs_codec_priv_t *priv = dev->priv;
    vs_video_frame_info_s frame;
    vs_video_frame_info_s *source;
    VB_BLK block = VS_INVALID_VB_HANDLE;
    uint64_t phys = 0;
    void *virt = NULL;
    uint32_t bytes;
    int rc;
    if (!priv->encoding || in == NULL || out == NULL || out->data == NULL)
        return -EINVAL;
    source = (vs_video_frame_info_s *)in->priv;
    if (source == NULL) {
        bytes = priv->format.width * priv->format.height * 3u / 2u;
        if (in->data == NULL || in->size < bytes)
            return -EINVAL;
        block = vs_mal_vb_block_get(0, bytes, NULL);
        if (block == VS_INVALID_VB_HANDLE)
            return -ENOMEM;
        phys = vs_mal_vb_handle2physaddr(block);
        virt = vs_mal_sys_mmap_cached(phys, bytes);
        if (virt == NULL) { rc = -ENOMEM; goto cleanup; }
        memcpy(virt, in->data, bytes);
        vs_mal_sys_cache_flush(phys, virt, bytes);
        memset(&frame, 0, sizeof(frame));
        frame.poolid = vs_mal_vb_handle2poolid(block);
        frame.modid = E_MOD_ID_USER;
        frame.frame.width = priv->format.width;
        frame.frame.height = priv->format.height;
        frame.frame.pixel_format = E_PIXEL_FORMAT_YUV_420SP;
        frame.frame.video_format = E_VIDEO_FORMAT_LINEAR;
        frame.frame.compress_mode = E_COMPRESS_MODE_NONE;
        frame.frame.dynamic_range = E_DYNAMIC_RANGE_SDR8;
        frame.frame.stride[0] = priv->format.width;
        frame.frame.stride[1] = priv->format.width;
        frame.frame.phys_addr[0] = phys;
        frame.frame.phys_addr[1] = phys + priv->format.width * priv->format.height;
        frame.frame.virt_addr[0] = (uint64_t)(uintptr_t)virt;
        frame.frame.virt_addr[1] = (uint64_t)(uintptr_t)virt + priv->format.width * priv->format.height;
        frame.frame.pts = in->timestamp_ns / 1000ull;
        source = &frame;
    }
    if (vs_mal_venc_frame_send(priv->venc_chn, source, timeout_ms) != VS_SUCCESS) {
        rc = -ETIMEDOUT;
        goto cleanup;
    }
    rc = vs_venc_packet_acquire(priv->venc_chn, out, timeout_ms);
cleanup:
    if (virt != NULL) vs_mal_sys_unmap(virt, bytes);
    if (block != VS_INVALID_VB_HANDLE) vs_mal_vb_block_release(block);
    return rc;
}

static int decode(codec_device_t *dev, const codec_buffer_t *in, codec_buffer_t *out,
                  int timeout_ms) {
    vs_codec_priv_t *priv = dev->priv;
    vs_vdec_stream_s stream;
    vs_video_frame_info_s frame;
    uint32_t stride, bytes, capacity;
    if (in == NULL || in->data == NULL || in->size == 0 ||
        out == NULL || out->data == NULL || out->size == 0)
        return -EINVAL;
    if (!priv->sys_acquired) {
        if (vs_sys_acquire(priv->format.width, priv->format.height) != 0)
            return -EIO;
        priv->sys_acquired = 1;
    }
    if (!priv->decoding && vdec_start(priv) != 0)
        return -EIO;
    memset(&stream, 0, sizeof(stream));
    stream.len = in->size;
    stream.pts = in->timestamp_ns / 1000ull;
    stream.is_frame_end = VS_TRUE;
    stream.is_display = VS_TRUE;
    stream.p_virt_addr = in->data;
    if (vs_mal_vdec_stream_send(priv->vdec_chn, &stream, timeout_ms) != VS_SUCCESS)
        return -ETIMEDOUT;
    memset(&frame, 0, sizeof(frame));
    if (vs_mal_vdec_frame_acquire(priv->vdec_chn, &frame, timeout_ms) != VS_SUCCESS)
        return -EAGAIN;
    capacity = out->size;
    stride = frame.frame.stride[0] != 0 ? frame.frame.stride[0] : frame.frame.width;
    bytes = stride * frame.frame.height * 3u / 2u;
    if (bytes > capacity) {
        out->size = bytes;
        vs_mal_vdec_frame_release(priv->vdec_chn, &frame);
        return -ENOSPC;
    }
    memcpy(out->data, (void *)(uintptr_t)frame.frame.virt_addr[0], bytes);
    out->size = bytes;
    out->offset = 0;
    out->timestamp_ns = frame.frame.pts * 1000ull;
    vs_mal_vdec_frame_release(priv->vdec_chn, &frame);
    return 0;
}

static int flush(codec_device_t *dev) {
    vs_codec_priv_t *priv = dev->priv;
    if (priv->encoding) {
        vs_mal_venc_chn_stop(priv->venc_chn);
        vs_mal_venc_chn_reset(priv->venc_chn);
        vs_venc_start_param_s param = {(vs_uint32_t)-1};
        vs_mal_venc_chn_start(priv->venc_chn, &param);
    }
    if (priv->decoding) {
        vs_mal_vdec_chn_stop(priv->vdec_chn);
        vs_mal_vdec_chn_reset(priv->vdec_chn);
        vs_mal_vdec_chn_start(priv->vdec_chn);
    }
    return 0;
}

static int set_bitrate(codec_device_t *dev, uint32_t bitrate_bps) {
    vs_codec_priv_t *priv = dev->priv;
    vs_venc_chn_attr_s attr;
    if (!priv->encoding || bitrate_bps == 0)
        return -EINVAL;
    if (vs_mal_venc_chn_attr_get(priv->venc_chn, &attr) != VS_SUCCESS)
        return -EIO;
    attr.brc_attr.h264_cbr.bitrate = bitrate_bps / 1000u;
    if (vs_mal_venc_chn_attr_set(priv->venc_chn, &attr) != VS_SUCCESS)
        return -EIO;
    priv->format.bitrate_bps = bitrate_bps;
    return 0;
}

static const codec_device_ops_t ops = {
    .get_capabilities = get_caps, .set_format = set_format, .get_format = get_format,
    .start = start, .stop = stop, .encode = encode, .decode = decode,
    .flush = flush, .set_rc_param = set_bitrate,
};

static int close_device(hw_device_t *hw) {
    codec_device_t *dev = (codec_device_t *)hw;
    if (dev != NULL) { stop(dev); free(dev->priv); free(dev); }
    return 0;
}

static int open_device(const hw_module_t *module, const char *id, hw_device_t **out) {
    codec_device_t *dev;
    vs_codec_priv_t *priv;
    int index = parse_index(id);
    if (out == NULL || index < 0)
        return -EINVAL;
    dev = calloc(1, sizeof(*dev));
    priv = calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) { free(dev); free(priv); return -ENOMEM; }
    priv->index = index; priv->venc_chn = index; priv->vdec_chn = index;
    priv->format = (codec_format_t){CODEC_ID_H264, 1920, 1080, 0x3231564e, 4000000, 30, 30};
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CODEC_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = close_device;
    dev->ops = &ops; dev->priv = priv;
    *out = &dev->common;
    return 0;
}

static hw_module_methods_t methods = {.open = open_device};
hw_module_t HMI_codec = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = CODEC_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = CODEC_HARDWARE_MODULE_ID,
    .name = "Visinextek VS816 Codec HAL (MAL VENC/VDEC H.264)",
    .author = "DarkOS", .methods = &methods,
};
