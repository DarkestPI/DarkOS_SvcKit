#ifndef DARKOS_VS_VENC_STREAM_H
#define DARKOS_VS_VENC_STREAM_H

#include <codec/ICodec.h>

int vs_venc_packet_acquire(int venc_chn, codec_buffer_t *packet, int timeout_ms);

#endif
