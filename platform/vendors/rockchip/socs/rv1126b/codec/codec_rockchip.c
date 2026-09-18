/*
 * Rockchip RV1126B Codec HAL：基于 rockit MPI VENC/VDEC 的 H.264 硬编/硬解。
 *
 * 把 hardware/interfaces/media 的 codec 接口映射到 rockit MPI 通道：
 *   start  → SYS 引用 + VENC CreateChn（NV12 输入、H.264 CBR、normalP GOP）
 *   encode → SendFrame 送一帧 + GetStream 同步取回码流包（阻塞模型）
 *   decode → （首次调用时惰性建 VDEC 通道）SendStream 送一包 +
 *             GetFrame 取回一帧 NV12；解码器内部有缓冲，暂无帧返回 -EAGAIN
 *   stop   → 逆序销毁 + 释放 SYS 引用
 *
 * 多实例：open id "codec"/"codec0" → 实例 0，"codecN" → 实例 N，
 *   VENC/VDEC 硬件通道号 = N（多路相机时与 cameraN 一一对应）。
 *
 * 说明：
 *   - 编码零拷贝：in->priv 携带 camera 透出的 MB_BLK（VI 出帧，回调期内有效）
 *     时直接包装 VIDEO_FRAME_INFO_S 送 VENC，全程无整帧拷贝；VENC 内部对 MB
 *     自持引用，SendFrame 返回后 camera 即可还帧（SDK rkipc dv 同款时序）。
 *     无 priv 的输入（host 联调/拷贝路径帧）经内部 MB 池拷贝一次再送；
 *   - 码流为 Annex-B，rockit VENC 默认每 IDR 内嵌 SPS/PPS（rkipc RTSP 直接
 *     转发的同款流），与上层 RtpPacketizerH264 的"AU 自带参数集"路径对齐；
 *   - keyframe 标记用 VENC_PACK_S.DataType.enH264EType（IDR/I 帧判定）；
 *   - 解码输出从 VDEC MB 拷出到 out->data（行对齐不一致时逐行打包）；
 *     解码上屏零拷贝（VDEC bind VO）留待本地回放需求落地时再做。
 */

#include <hardware/hardware.h>
#include <codec/ICodec.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rk_comm_mb.h>
#include <rk_comm_vdec.h>
#include <rk_comm_venc.h>
#include <rk_comm_video.h>
#include <rk_mpi_mb.h>
#include <rk_mpi_vdec.h>
#include <rk_mpi_venc.h>

#include "common/mpi_sys_guard.h"

#define RK_CODEC_MAX_INSTANCES 8
#define RK_RAW_POOL_CNT 2    /* encode data 路径补帧池块数 */
#define RK_STREAM_POOL_CNT 2 /* decode 送流池块数 */
#define RK_VENC_MAX_WIDTH 3840
#define RK_VENC_MAX_HEIGHT 2160

typedef struct rk_codec_priv {
    int idx;      /* 实例号（open id "codecN" 的 N） */
    int venc_chn; /* VENC/VDEC 硬件通道 = 实例号 */
    int vdec_chn;

    codec_format_t fmt;

    int sys_acquired;
    int encoding; /* VENC 通道已建 */
    int decoding; /* VDEC 通道已建（惰性，首次 decode 时） */

    MB_POOL raw_pool;    /* encode：无 priv 输入的拷贝缓冲池 */
    MB_POOL stream_pool; /* decode：码流送帧缓冲池 */
} rk_codec_priv_t;

/* ---------------------------------------------------------------------------
 * MB 池辅助
 * ------------------------------------------------------------------------- */

static MB_POOL rk_mb_pool_create(uint64_t blk_size, uint32_t cnt) {
    MB_POOL_CONFIG_S cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.u64MBSize = blk_size;
    cfg.u32MBCnt = cnt;
    cfg.enRemapMode = MB_REMAP_MODE_NOCACHE; /* CPU 写后硬件读，免 cache 维护 */
    cfg.enAllocType = MB_ALLOC_TYPE_DMA;
    cfg.bPreAlloc = RK_TRUE;
    return RK_MPI_MB_CreatePool(&cfg);
}

/* ---------------------------------------------------------------------------
 * 编码（VENC）
 * ------------------------------------------------------------------------- */

static int rk_venc_create(rk_codec_priv_t *priv) {
    VENC_CHN_ATTR_S attr;
    VENC_RECV_PIC_PARAM_S recv;
    uint32_t w = priv->fmt.width, h = priv->fmt.height;

    memset(&attr, 0, sizeof(attr));
    attr.stVencAttr.enType = RK_VIDEO_ID_AVC;
    attr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
    attr.stVencAttr.u32Profile = 77; /* main：适合 1080p/4K RTSP 细节压缩 */
    attr.stVencAttr.u32MaxPicWidth = w;
    attr.stVencAttr.u32MaxPicHeight = h;
    attr.stVencAttr.u32PicWidth = w;
    attr.stVencAttr.u32PicHeight = h;
    attr.stVencAttr.u32VirWidth = w;
    attr.stVencAttr.u32VirHeight = h;
    attr.stVencAttr.u32StreamBufCnt = 4;
    attr.stVencAttr.u32BufSize = (RK_U32)((uint64_t)w * h * 3 / 2); /* 单帧码流上限余量 */

    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32Gop = priv->fmt.gop;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = priv->fmt.fps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = priv->fmt.fps;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
    attr.stRcAttr.stH264Cbr.u32BitRate = priv->fmt.bitrate_bps / 1000; /* MPI 单位 kbps */
    attr.stRcAttr.stH264Cbr.u32StatTime = 1;

    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;

    if (RK_MPI_VENC_CreateChn(priv->venc_chn, &attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_codec: VENC CreateChn failed (%ux%u)\n", w, h);
        return -EIO;
    }
    memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1; /* 持续接收 */
    if (RK_MPI_VENC_StartRecvFrame(priv->venc_chn, &recv) != RK_SUCCESS) {
        fprintf(stderr, "rk_codec: VENC StartRecvFrame failed\n");
        RK_MPI_VENC_DestroyChn(priv->venc_chn);
        return -EIO;
    }

    priv->raw_pool = rk_mb_pool_create((uint64_t)w * h * 3 / 2, RK_RAW_POOL_CNT);
    if (priv->raw_pool == MB_INVALID_POOLID) {
        fprintf(stderr, "rk_codec: raw MB pool create failed\n");
        RK_MPI_VENC_StopRecvFrame(priv->venc_chn);
        RK_MPI_VENC_DestroyChn(priv->venc_chn);
        return -ENOMEM;
    }
    priv->encoding = 1;
    return 0;
}

static void rk_venc_destroy(rk_codec_priv_t *priv) {
    if (!priv->encoding)
        return;
    priv->encoding = 0;
    RK_MPI_VENC_StopRecvFrame(priv->venc_chn);
    RK_MPI_VENC_DestroyChn(priv->venc_chn);
    if (priv->raw_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(priv->raw_pool);
        priv->raw_pool = MB_INVALID_POOLID;
    }
}

/* 组 VIDEO_FRAME_INFO_S：优先用 in->priv 的 MB_BLK 零拷贝；否则池内拷贝。
 * owned_blk 非 NULL 表示池内块，SendFrame 后由调用方归还 */
static int rk_venc_build_frame(rk_codec_priv_t *priv, const codec_buffer_t *in,
                               VIDEO_FRAME_INFO_S *vf, MB_BLK *owned_blk) {
    MB_BLK blk = (MB_BLK)in->priv;

    *owned_blk = NULL;
    if (blk == NULL) {
        void *vir;

        if (in->data == NULL)
            return -EINVAL;
        blk = RK_MPI_MB_GetMB(priv->raw_pool, in->size, RK_TRUE);
        if (blk == NULL)
            return -ENOMEM;
        vir = RK_MPI_MB_Handle2VirAddr(blk);
        if (vir == NULL) {
            RK_MPI_MB_ReleaseMB(blk);
            return -EIO;
        }
        memcpy(vir, in->data, in->size);
        *owned_blk = blk;
    }

    memset(vf, 0, sizeof(*vf));
    vf->stVFrame.pMbBlk = blk;
    vf->stVFrame.u32Width = priv->fmt.width;
    vf->stVFrame.u32Height = priv->fmt.height;
    vf->stVFrame.u32VirWidth = priv->fmt.width;
    vf->stVFrame.u32VirHeight = priv->fmt.height;
    vf->stVFrame.enPixelFormat = RK_FMT_YUV420SP;
    vf->stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    vf->stVFrame.u64PTS = in->timestamp_ns / 1000ull; /* MPI PTS 单位 us */
    return 0;
}

static int rk_codec_encode(codec_device_t *dev, const codec_buffer_t *in, codec_buffer_t *out,
                           int timeout_ms) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;
    VIDEO_FRAME_INFO_S vf;
    VENC_STREAM_S stream;
    VENC_PACK_S pack;
    MB_BLK owned_blk = NULL;
    void *data;
    int wait_ms;
    int rc;

    if (in == NULL || out == NULL || out->data == NULL)
        return -EINVAL;
    if (in->priv == NULL && in->data == NULL)
        return -EINVAL;
    if (!priv->encoding)
        return -EINVAL;
    if (in->size > (uint32_t)((size_t)priv->fmt.width * priv->fmt.height * 3 / 2))
        return -EINVAL;

    rc = rk_venc_build_frame(priv, in, &vf, &owned_blk);
    if (rc != 0)
        return rc;

    /* MPI 的 GetStream 不是严格零延迟接口。上层传 0 表示不要求调用方
     * 等待，但硬件编码完成可能晚于 SendFrame 返回；给 VENC 一个单帧内的
     * 小窗口可避免启动首帧和偶发帧被误判为超时。 */
    wait_ms = timeout_ms > 0 ? timeout_ms : 100;
    rc = RK_MPI_VENC_SendFrame(priv->venc_chn, &vf, wait_ms);
    if (owned_blk != NULL)
        RK_MPI_MB_ReleaseMB(owned_blk); /* VENC 已自持引用，池块即还 */
    if (rc != RK_SUCCESS)
        return -EIO;

    /* 同步取回本帧码流（单 AU 一个 pack，对齐 rkipc 用法） */
    memset(&stream, 0, sizeof(stream));
    memset(&pack, 0, sizeof(pack));
    stream.pstPack = &pack;
    if (RK_MPI_VENC_GetStream(priv->venc_chn, &stream, wait_ms) != RK_SUCCESS)
        return -ETIMEDOUT;

    data = (uint8_t *)RK_MPI_MB_Handle2VirAddr(pack.pMbBlk) + pack.u32Offset;
    if (pack.u32Len > out->size) { /* out->size 入参为缓冲容量 */
        rc = -ENOSPC;
    } else {
        memcpy(out->data, data, pack.u32Len);
        out->size = pack.u32Len;
        out->offset = 0;
        out->timestamp_ns = pack.u64PTS * 1000ull;
        if (pack.DataType.enH264EType == H264E_NALU_IDRSLICE ||
            pack.DataType.enH264EType == H264E_NALU_ISLICE)
            out->flags |= CODEC_BUFFER_FLAG_KEYFRAME;
        rc = 0;
    }
    RK_MPI_VENC_ReleaseStream(priv->venc_chn, &stream);
    return rc;
}

/* ---------------------------------------------------------------------------
 * 解码（VDEC，惰性建通道）
 * ------------------------------------------------------------------------- */

static int rk_vdec_create(rk_codec_priv_t *priv) {
    VDEC_CHN_ATTR_S attr;
    uint32_t w = priv->fmt.width, h = priv->fmt.height;

    memset(&attr, 0, sizeof(attr));
    attr.enType = RK_VIDEO_ID_AVC;
    attr.enMode = VIDEO_MODE_FRAME; /* 一次一包一帧 */
    attr.u32PicWidth = w;
    attr.u32PicHeight = h;
    attr.u32PicVirWidth = (w + 15) & ~15u;
    attr.u32PicVirHeight = (h + 15) & ~15u;
    attr.u32StreamBufSize = w * h; /* 码流环形缓冲，远大于单 AU */
    attr.u32FrameBufSize = w * h * 3 / 2;
    attr.u32FrameBufCnt = 4;
    attr.u32FrameBufDepth = 1;
    attr.enPixelFormat = RK_FMT_YUV420SP;
    attr.enCompressMode = COMPRESS_MODE_NONE;
    attr.enLib = VDEC_LIB_MPP;

    if (RK_MPI_VDEC_CreateChn(priv->vdec_chn, &attr) != RK_SUCCESS) {
        fprintf(stderr, "rk_codec: VDEC CreateChn failed (%ux%u)\n", w, h);
        return -EIO;
    }
    if (RK_MPI_VDEC_StartRecvStream(priv->vdec_chn) != RK_SUCCESS) {
        fprintf(stderr, "rk_codec: VDEC StartRecvStream failed\n");
        RK_MPI_VDEC_DestroyChn(priv->vdec_chn);
        return -EIO;
    }

    priv->stream_pool = rk_mb_pool_create(w * h, RK_STREAM_POOL_CNT);
    if (priv->stream_pool == MB_INVALID_POOLID) {
        fprintf(stderr, "rk_codec: stream MB pool create failed\n");
        RK_MPI_VDEC_StopRecvStream(priv->vdec_chn);
        RK_MPI_VDEC_DestroyChn(priv->vdec_chn);
        return -ENOMEM;
    }
    priv->decoding = 1;
    return 0;
}

static void rk_vdec_destroy(rk_codec_priv_t *priv) {
    if (!priv->decoding)
        return;
    priv->decoding = 0;
    RK_MPI_VDEC_StopRecvStream(priv->vdec_chn);
    RK_MPI_VDEC_DestroyChn(priv->vdec_chn);
    if (priv->stream_pool != MB_INVALID_POOLID) {
        RK_MPI_MB_DestroyPool(priv->stream_pool);
        priv->stream_pool = MB_INVALID_POOLID;
    }
}

static int rk_codec_decode(codec_device_t *dev, const codec_buffer_t *in, codec_buffer_t *out,
                           int timeout_ms) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;
    VDEC_STREAM_S st;
    VIDEO_FRAME_INFO_S vfi;
    MB_BLK blk;
    void *vir;
    uint32_t expect = priv->fmt.width * priv->fmt.height * 3 / 2;

    if (in == NULL || out == NULL || in->data == NULL || out->data == NULL)
        return -EINVAL;
    if (!priv->encoding) /* start 未调则编/解都不可用 */
        return -EINVAL;
    if (!priv->decoding && rk_vdec_create(priv) != 0)
        return -EIO;
    if (in->size > priv->fmt.width * priv->fmt.height) /* 送流池块大小上限 */
        return -EINVAL;

    /* 码流拷进 MB 送解码；bBypassMbBlk=FALSE：VDEC 内部自拷，MB 送完即还 */
    blk = RK_MPI_MB_GetMB(priv->stream_pool, in->size, RK_TRUE);
    if (blk == NULL)
        return -ENOMEM;
    vir = RK_MPI_MB_Handle2VirAddr(blk);
    if (vir == NULL) {
        RK_MPI_MB_ReleaseMB(blk);
        return -EIO;
    }
    memcpy(vir, in->data, in->size);

    memset(&st, 0, sizeof(st));
    st.pMbBlk = blk;
    st.u32Len = in->size;
    st.u64PTS = in->timestamp_ns / 1000ull;
    st.bEndOfFrame = RK_TRUE;
    st.bEndOfStream = RK_FALSE;
    st.bBypassMbBlk = RK_FALSE;
    if (RK_MPI_VDEC_SendStream(priv->vdec_chn, &st, timeout_ms) != RK_SUCCESS) {
        RK_MPI_MB_ReleaseMB(blk);
        return -EIO;
    }
    RK_MPI_MB_ReleaseMB(blk);

    /* 取解码帧：解码器有内部缓冲，暂无帧不算错误（-EAGAIN，继续送下一包） */
    memset(&vfi, 0, sizeof(vfi));
    if (RK_MPI_VDEC_GetFrame(priv->vdec_chn, &vfi, timeout_ms) != RK_SUCCESS)
        return -EAGAIN;

    vir = RK_MPI_MB_Handle2VirAddr(vfi.stVFrame.pMbBlk);
    if (vir == NULL) {
        RK_MPI_VDEC_ReleaseFrame(priv->vdec_chn, &vfi);
        return -EIO;
    }
    if (out->size < expect) {
        RK_MPI_VDEC_ReleaseFrame(priv->vdec_chn, &vfi);
        return -ENOSPC;
    }
    if (vfi.stVFrame.u32VirWidth == vfi.stVFrame.u32Width) {
        memcpy(out->data, vir,
               (size_t)vfi.stVFrame.u32Width * vfi.stVFrame.u32Height * 3 / 2);
    } else {
        /* 行对齐不一致：按 stride 逐行打包 */
        const uint8_t *src = (const uint8_t *)vir;
        uint8_t *dst = (uint8_t *)out->data;
        uint32_t row, rows = vfi.stVFrame.u32Height * 3 / 2;
        for (row = 0; row < rows; row++) {
            memcpy(dst, src, vfi.stVFrame.u32Width);
            src += vfi.stVFrame.u32VirWidth;
            dst += vfi.stVFrame.u32Width;
        }
    }
    out->size = expect;
    out->offset = 0;
    out->timestamp_ns = vfi.stVFrame.u64PTS * 1000ull;
    RK_MPI_VDEC_ReleaseFrame(priv->vdec_chn, &vfi);
    return 0;
}

/* ---------------------------------------------------------------------------
 * 设备操作实现
 * ------------------------------------------------------------------------- */

static int rk_codec_get_capabilities(codec_device_t *dev, codec_caps_t *caps) {
    (void)dev;
    if (caps == NULL)
        return -EINVAL;
    memset(caps, 0, sizeof(*caps));
    caps->min_width = 128;
    caps->min_height = 128;
    caps->max_width = RK_VENC_MAX_WIDTH;
    caps->max_height = RK_VENC_MAX_HEIGHT;
    caps->supported_codecs = CODEC_CAPS_H264; /* 编解码均为 H.264 */
    return 0;
}

static int rk_codec_set_format(codec_device_t *dev, const codec_format_t *fmt) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;

    if (fmt == NULL)
        return -EINVAL;
    if (fmt->codec != CODEC_ID_H264)
        return -EINVAL; /* 当前仅实现 H.264 硬编/硬解 */
    if (fmt->pixel_format != 0x3231564e)
        return -EINVAL; /* 原始帧仅接 NV12（'NV12'，相机默认输出） */
    if (fmt->width < 128 || fmt->height < 128 || fmt->width > RK_VENC_MAX_WIDTH ||
        fmt->height > RK_VENC_MAX_HEIGHT)
        return -EINVAL;

    priv->fmt = *fmt;
    return 0;
}

static int rk_codec_get_format(codec_device_t *dev, codec_format_t *fmt) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;
    if (fmt == NULL)
        return -EINVAL;
    *fmt = priv->fmt;
    return 0;
}

static int rk_codec_start(codec_device_t *dev) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;

    if (priv->encoding)
        return -EBUSY;

    if (rk_mpi_sys_acquire() != 0)
        return -EIO;
    priv->sys_acquired = 1;

    if (rk_venc_create(priv) != 0) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
        return -EIO;
    }
    return 0;
}

static int rk_codec_stop(codec_device_t *dev) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;

    rk_vdec_destroy(priv);
    rk_venc_destroy(priv);
    if (priv->sys_acquired) {
        rk_mpi_sys_release();
        priv->sys_acquired = 0;
    }
    return 0;
}

static int rk_codec_flush(codec_device_t *dev) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;
    if (priv->encoding)
        RK_MPI_VENC_ResetChn(priv->venc_chn);
    if (priv->decoding)
        RK_MPI_VDEC_ResetChn(priv->vdec_chn);
    return 0;
}

/* 运行态改码率：本 SDK 的 RK_MPI_VENC_SetRcParam 只承载 QP 参数
 * （VENC_RC_PARAM_S 无码率字段），改码率走 GetChnAttr/SetChnAttr 改
 * stRcAttr.stH264Cbr.u32BitRate（rkipc rk_video_set_max_rate 同款路径）。
 * 只改码率一项，fps/分辨率不动；属性整体回写，其余字段保持现状。 */
static int rk_codec_set_rc_param(codec_device_t *dev, uint32_t bitrate_bps) {
    rk_codec_priv_t *priv = (rk_codec_priv_t *)dev->priv;
    VENC_CHN_ATTR_S attr;

    if (!priv->encoding)
        return -EINVAL;

    memset(&attr, 0, sizeof(attr));
    if (RK_MPI_VENC_GetChnAttr(priv->venc_chn, &attr) != RK_SUCCESS)
        return -EIO;
    attr.stRcAttr.stH264Cbr.u32BitRate = bitrate_bps / 1000; /* MPI 单位 kbps */
    if (RK_MPI_VENC_SetChnAttr(priv->venc_chn, &attr) != RK_SUCCESS)
        return -EIO;
    priv->fmt.bitrate_bps = bitrate_bps; /* 记账：get_format 透出最新值 */
    return 0;
}

static const codec_device_ops_t rk_codec_ops = {
    .get_capabilities = rk_codec_get_capabilities,
    .set_format = rk_codec_set_format,
    .get_format = rk_codec_get_format,
    .start = rk_codec_start,
    .stop = rk_codec_stop,
    .encode = rk_codec_encode,
    .decode = rk_codec_decode,
    .flush = rk_codec_flush,
    .set_rc_param = rk_codec_set_rc_param,
};

/* ---------------------------------------------------------------------------
 * 设备生命周期
 * ------------------------------------------------------------------------- */

static int rk_codec_close(hw_device_t *device) {
    codec_device_t *dev = (codec_device_t *)device;
    rk_codec_priv_t *priv;

    if (dev == NULL)
        return 0;
    priv = (rk_codec_priv_t *)dev->priv;
    if (priv != NULL) {
        rk_codec_stop(dev);
        free(priv);
    }
    free(dev);
    return 0;
}

/* 解析 open id："codec"/"codec0" → 0，"codecN" → N；非法返回 -1 */
static int rk_codec_parse_idx(const char *id) {
    const char *prefix = CODEC_HARDWARE_MODULE_ID; /* "codec" */
    size_t plen = strlen(prefix);
    long idx;
    char *end;

    if (id == NULL || strncmp(id, prefix, plen) != 0)
        return -1;
    if (id[plen] == '\0')
        return 0;
    idx = strtol(id + plen, &end, 10);
    if (*end != '\0' || idx < 0 || idx >= RK_CODEC_MAX_INSTANCES)
        return -1;
    return (int)idx;
}

static int rk_codec_open(const hw_module_t *module, const char *id, hw_device_t **device) {
    codec_device_t *dev;
    rk_codec_priv_t *priv;
    int idx;

    if (device == NULL)
        return -EINVAL;
    idx = rk_codec_parse_idx(id);
    if (idx < 0) {
        fprintf(stderr, "rk_codec: 非法设备 id \"%s\"（期望 codec / codecN）\n",
                id != NULL ? id : "(null)");
        return -EINVAL;
    }

    dev = (codec_device_t *)calloc(1, sizeof(*dev));
    priv = (rk_codec_priv_t *)calloc(1, sizeof(*priv));
    if (dev == NULL || priv == NULL) {
        free(dev);
        free(priv);
        return -ENOMEM;
    }

    priv->idx = idx;
    priv->venc_chn = idx;
    priv->vdec_chn = idx;

    priv->fmt = (codec_format_t){.codec = CODEC_ID_H264,
                                       .width = 1920,
                                       .height = 1080,
                                       .pixel_format = 0x3231564e, /* 'NV12' */
                                       .bitrate_bps = 2 * 1000 * 1000,
                                       .fps = 30,
                                       .gop = 30};
    priv->raw_pool = MB_INVALID_POOLID;
    priv->stream_pool = MB_INVALID_POOLID;

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = CODEC_DEVICE_API_VERSION_1_0;
    dev->common.module = (hw_module_t *)module;
    dev->common.close = rk_codec_close;
    dev->ops = &rk_codec_ops;
    dev->priv = priv;

    *device = (hw_device_t *)dev;
    return 0;
}

/* ---------------------------------------------------------------------------
 * 模块导出（hal.rockchip.rv1126b.so，dlsym("HMI_codec")）
 * ------------------------------------------------------------------------- */

static struct hw_module_methods_t rk_codec_methods = {
    .open = rk_codec_open,
};

struct hw_module_t HMI_codec = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = CODEC_MODULE_API_VERSION_1_0,
    .hal_api_version = HARDWARE_API_VERSION_1_0,
    .id = CODEC_HARDWARE_MODULE_ID,
    .name = "Rockchip RV1126B Codec HAL (MPI VENC/VDEC H.264)",
    .author = "DarkOS",
    .methods = &rk_codec_methods,
};
