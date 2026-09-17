/*
 * 主机参考实现（host_x86 变体）：双编码器。
 *
 * - CODEC_ID_RAW ：透传伪编码（输入原样拷贝），验证管道语义；
 * - CODEC_ID_H264：经 x264（third_party）软编，供 RTSP 出流联调。
 *   参数预设 ultrafast + zerolatency（无 B 帧、逐帧出包，贴合直播联调），
 *   输出 Annex-B 裸流且 SPS/PPS 内嵌（RTP 打包可直接解析）。
 *
 * 用途：在宿主机上联调 frameworks/packages，不依赖 MPP。
 * 真实 H.264 硬编由 rockchip 实现（MPP）提供。
 * 用环境变量切换：DARKOS_HAL_VARIANT=host_x86。
 */

#include <hardware/hardware.h>
#include <codec/ICodec.h>

#include <x264.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct host_codec_priv {
    codec_format_t fmt;
    int encoding;
    uint32_t seq; /* 已编码帧序号，驱动 keyframe 标记 */

    /* H.264（x264） */
    x264_t *enc;
    int64_t pts;
} host_codec_priv_t;

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int host_codec_get_capabilities(codec_device_t *dev, codec_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->supported_codecs = CODEC_CAPS_RAW | CODEC_CAPS_H264;
    caps->min_width = 160;
    caps->min_height = 120;
    caps->max_width = 3840;
    caps->max_height = 2160;
    return 0;
}

static int host_codec_set_format(codec_device_t *dev, const codec_format_t *fmt) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->codec != CODEC_ID_RAW && fmt->codec != CODEC_ID_H264)
        return -EINVAL; /* 主机实现仅支持 RAW 透传与 H.264 */
    if (fmt->width < 160 || fmt->height < 120 || fmt->width > 3840 || fmt->height > 2160)
        return -EINVAL;
    if (fmt->codec == CODEC_ID_H264 && fmt->pixel_format != 0x3231564e)
        return -EINVAL; /* x264 输入当前仅接 NV12（相机默认输出） */

    priv->fmt = *fmt;
    return 0;
}

static int host_codec_get_format(codec_device_t *dev, codec_format_t *fmt) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = priv->fmt;
    return 0;
}

static int host_codec_start(codec_device_t *dev) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;

    if (priv->encoding)
        return -EBUSY;

    if (priv->fmt.codec == CODEC_ID_H264) {
        x264_param_t param;
        x264_param_default_preset(&param, "ultrafast", "zerolatency");
        param.i_width = (int)priv->fmt.width;
        param.i_height = (int)priv->fmt.height;
        param.i_csp = X264_CSP_NV12;
        param.i_fps_num = priv->fmt.fps ? priv->fmt.fps : 30;
        param.i_fps_den = 1;
        param.i_keyint_max = (int)(priv->fmt.gop ? priv->fmt.gop : 30);
        param.i_keyint_min = param.i_keyint_max;
        param.i_scenecut_threshold = 0;
        param.rc.i_bitrate =
            (int)(priv->fmt.bitrate_bps ? priv->fmt.bitrate_bps / 1000 : 2000);
        param.b_annexb = 1;        /* Annex-B 裸流 */
        param.b_repeat_headers = 1; /* SPS/PPS 随关键帧内嵌 */
        x264_param_apply_profile(&param, "baseline"); /* IPC 兼容性最好 */

        priv->enc = x264_encoder_open(&param);
        if (priv->enc == NULL)
            return -ENOMEM;
        priv->pts = 0;
    }

    priv->encoding = 1;
    priv->seq = 0;
    return 0;
}

static int host_codec_stop(codec_device_t *dev) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;

    if (priv->enc != NULL) {
        x264_encoder_close(priv->enc);
        priv->enc = NULL;
    }
    priv->encoding = 0;
    return 0;
}

static int encode_raw(host_codec_priv_t *priv, const codec_buffer_t *in, codec_buffer_t *out) {
    uint32_t gop = priv->fmt.gop ? priv->fmt.gop : 30;

    if (out->size < in->size) /* out->size 入参为缓冲容量 */
        return -ENOSPC;

    memcpy(out->data, in->data, in->size);
    out->size = in->size;
    out->flags = (priv->seq % gop == 0) ? CODEC_BUFFER_FLAG_KEYFRAME : 0;
    return 0;
}

static int encode_h264(host_codec_priv_t *priv, const codec_buffer_t *in, codec_buffer_t *out) {
    x264_picture_t pic_in, pic_out;
    x264_nal_t *nals = NULL;
    int nal_count = 0;
    size_t ysize = (size_t)priv->fmt.width * priv->fmt.height;
    int i;

    x264_picture_init(&pic_in);
    pic_in.img.i_csp = X264_CSP_NV12;
    pic_in.img.i_plane = 2;
    pic_in.img.i_stride[0] = (int)priv->fmt.width;
    pic_in.img.i_stride[1] = (int)priv->fmt.width;
    pic_in.img.plane[0] = (uint8_t *)in->data;
    pic_in.img.plane[1] = (uint8_t *)in->data + ysize;
    pic_in.i_pts = priv->pts++;

    int bytes = x264_encoder_encode(priv->enc, &nals, &nal_count, &pic_in, &pic_out);
    if (bytes < 0)
        return -EIO;
    if (bytes == 0) {
        out->size = 0; /* zerolatency 下不应发生，防御性处理 */
        return 0;
    }
    if ((uint32_t)bytes > out->size)
        return -ENOSPC;

    /* 多个 NAL 顺序拼到输出缓冲 */
    uint8_t *p = (uint8_t *)out->data;
    for (i = 0; i < nal_count; i++) {
        memcpy(p, nals[i].p_payload, nals[i].i_payload);
        p += nals[i].i_payload;
    }
    out->size = (uint32_t)bytes;
    out->flags = pic_out.b_keyframe ? CODEC_BUFFER_FLAG_KEYFRAME : 0;
    return 0;
}

static int host_codec_encode(codec_device_t *dev, const codec_buffer_t *in, codec_buffer_t *out,
                             int timeout_ms) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;
    int rc;

    (void)timeout_ms; /* 主机实现即时出包，无阻塞等待 */

    if (in == NULL || out == NULL || in->data == NULL || out->data == NULL)
        return -EINVAL;
    if (!priv->encoding)
        return -EINVAL;

    if (priv->fmt.codec == CODEC_ID_H264)
        rc = encode_h264(priv, in, out);
    else
        rc = encode_raw(priv, in, out);
    if (rc != 0)
        return rc;

    out->offset = 0;
    out->timestamp_ns = in->timestamp_ns;
    if (in->flags & CODEC_BUFFER_FLAG_EOS)
        out->flags |= CODEC_BUFFER_FLAG_EOS;

    priv->seq++;
    return 0;
}

static int host_codec_flush(codec_device_t *dev) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;
    priv->seq = 0; /* RAW 无内部缓存；x264 在 zerolatency 下同样无缓存 */
    return 0;
}

/* 运行态改码率：host 侧 RAW 透传无码率概念，x264 软编的运行态码率调整
 * 对联调无意义——记账存值（get_format 可读出）返回 0，供控制面链路联调 */
static int host_codec_set_rc_param(codec_device_t *dev, uint32_t bitrate_bps) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;
    priv->fmt.bitrate_bps = bitrate_bps;
    return 0;
}

/* 解码：RAW 透传伪解码（与 encode_raw 对称，拷回原始帧）；H.264 软解未接，
 * 主机侧无此联调需求，返回 -ENOTSUP */
static int host_codec_decode(codec_device_t *dev, const codec_buffer_t *in, codec_buffer_t *out,
                             int timeout_ms) {
    host_codec_priv_t *priv = (host_codec_priv_t *)dev->priv;

    (void)timeout_ms;
    if (in == NULL || out == NULL || in->data == NULL || out->data == NULL)
        return -EINVAL;
    if (!priv->encoding)
        return -EINVAL;
    if (priv->fmt.codec != CODEC_ID_RAW)
        return -ENOTSUP;
    if (out->size < in->size)
        return -ENOSPC;

    memcpy(out->data, in->data, in->size);
    out->size = in->size;
    out->offset = 0;
    out->timestamp_ns = in->timestamp_ns;
    return 0;
}

static const codec_device_ops_t host_codec_ops = {
    .get_capabilities = host_codec_get_capabilities,
    .set_format = host_codec_set_format,
    .get_format = host_codec_get_format,
    .start = host_codec_start,
    .stop = host_codec_stop,
    .encode = host_codec_encode,
    .decode = host_codec_decode,
    .flush = host_codec_flush,
    .set_rc_param = host_codec_set_rc_param,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int host_codec_close(hw_device_t *device) {
    codec_device_t *dev = (codec_device_t *)device;

    if (dev == NULL)
        return 0;
    free(dev->priv);
    free(dev);
    return 0;
}

static int host_codec_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    codec_device_t *dev;
    host_codec_priv_t *priv;
    const char *prefix = CODEC_HARDWARE_MODULE_ID; /* "codec" */
    size_t plen = strlen(prefix);

    /* 软编无硬件通道概念：id 仅做合法性校验（"codec" / "codecN"），
     * 实例间天然独立（每个 open 一个设备对象） */
    if (device == NULL)
        return -EINVAL;
    if (id == NULL || strncmp(id, prefix, plen) != 0)
        return -EINVAL;
    if (id[plen] != '\0') {
        char *end = NULL;
        long idx = strtol(id + plen, &end, 10);
        if (*end != '\0' || idx < 0 || idx > 15)
            return -EINVAL;
    }

    dev = (codec_device_t *)calloc(1, sizeof(*dev));
    priv = (host_codec_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->fmt = (codec_format_t){
        .codec = CODEC_ID_RAW,
        .width = 1920,
        .height = 1080,
        .pixel_format = 0x3231564e, /* 'NV12' */
        .bitrate_bps = 4 * 1000 * 1000,
        .fps = 30,
        .gop = 30,
    };

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CODEC_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = host_codec_close;
    dev->ops = &host_codec_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t host_codec_methods = {
    .open = host_codec_open,
};

struct hw_module_t HMI_codec = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = CODEC_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = CODEC_HARDWARE_MODULE_ID,
    .name = "DarkOS Host Codec HAL (raw/h264)",
    .author = "DarkOS",
    .methods = &host_codec_methods,
};
