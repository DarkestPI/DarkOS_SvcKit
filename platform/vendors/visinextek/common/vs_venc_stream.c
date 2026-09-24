#include "vs_venc_stream.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vs_mal_venc.h>

int vs_venc_packet_acquire(int venc_chn, codec_buffer_t *packet, int timeout_ms) {
    struct pollfd wait_fd;
    vs_venc_chn_status_s status;
    vs_venc_stream_s stream;
    vs_venc_pack_s *packs;
    uint32_t i, j, copied = 0, required = 0, capacity;
    int fd, rc = 0;

    if (packet == NULL || packet->data == NULL || packet->size == 0 || timeout_ms < -1)
        return -EINVAL;
    fd = vs_mal_venc_chn_fd_get(venc_chn);
    if (fd < 0)
        return -EIO;
    wait_fd.fd = fd;
    wait_fd.events = POLLIN;
    wait_fd.revents = 0;
    rc = poll(&wait_fd, 1, timeout_ms);
    if (rc == 0)
        return -ETIMEDOUT;
    if (rc < 0)
        return errno == EINTR ? -EINTR : -EIO;
    if ((wait_fd.revents & POLLIN) == 0)
        return -EIO;

    memset(&status, 0, sizeof(status));
    if (vs_mal_venc_chn_status_get(venc_chn, &status) != VS_SUCCESS)
        return -EIO;
    if (status.left_stream_frame_num == 0 || status.cur_pack_num == 0)
        return -EAGAIN;
    packs = calloc(status.cur_pack_num, sizeof(*packs));
    if (packs == NULL)
        return -ENOMEM;

    memset(&stream, 0, sizeof(stream));
    stream.p_pack = packs;
    stream.pack_num = status.cur_pack_num;
    if (vs_mal_venc_stream_acquire(venc_chn, &stream, 0) != VS_SUCCESS) {
        free(packs);
        return -EAGAIN;
    }

    capacity = packet->size;
    for (i = 0; i < stream.pack_num; ++i) {
        if (UINT32_MAX - required < packs[i].length) {
            rc = -EOVERFLOW;
            goto release;
        }
        required += packs[i].length;
    }
    if (required > capacity) {
        packet->size = required;
        rc = -ENOSPC;
        goto release;
    }

    packet->flags = 0;
    for (i = 0; i < stream.pack_num; ++i) {
        vs_venc_pack_s *pack = &packs[i];
        uint32_t segmented = 0;
        if (pack->data_num > VENC_MAX_NALU_NUM_IN_ONE_PACK) {
            rc = -EIO;
            goto release;
        }
        for (j = 0; j < pack->data_num; ++j) {
            const vs_venc_pack_info_s *info = &pack->pack_info[j];
            if (info->pack_len > pack->length - segmented) {
                rc = -EIO;
                goto release;
            }
            memcpy((uint8_t *)packet->data + copied + segmented,
                   pack->virt_addr + info->pack_offset, info->pack_len);
            segmented += info->pack_len;
            if (info->pack_type.h264_type == E_VENC_NALU_H264_IDRSLICE ||
                info->pack_type.h264_type == E_VENC_NALU_H264_ISLICE)
                packet->flags |= CODEC_BUFFER_FLAG_KEYFRAME;
        }
        memcpy((uint8_t *)packet->data + copied + segmented,
               pack->virt_addr + pack->offset, pack->length - segmented);
        copied += pack->length;
        if (pack->nalu_type.h264_type == E_VENC_NALU_H264_IDRSLICE ||
            pack->nalu_type.h264_type == E_VENC_NALU_H264_ISLICE)
            packet->flags |= CODEC_BUFFER_FLAG_KEYFRAME;
        if (i == 0)
            packet->timestamp_ns = (uint64_t)pack->pts * 1000ull;
    }
    packet->offset = 0;
    packet->size = copied;
release:
    vs_mal_venc_stream_release(venc_chn, &stream);
    free(packs);
    return rc;
}
