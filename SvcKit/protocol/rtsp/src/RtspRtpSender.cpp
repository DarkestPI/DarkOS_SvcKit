/*
 * RtspRtpSender：RTP 头组包 + UDP/TCP interleaved 双出口（见 .h 头注）
 */

#include "stream/RtcpSession.h"

#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>

#include <random>

#include "base/Log.h"
#include "base/TimeUtil.h"

#include "RtspRtpSender.h"

namespace darkos {

static constexpr const char *kTag = "RtspRtpSender";

RtspRtpSender::RtspRtpSender(uint32_t clockRateHz) : clockRateHz_(clockRateHz) {
    /* seq/ssrc 随机初值（RFC 3550：初始序号应随机，防跨会话串包） */
    std::mt19937 rng(std::random_device{}() ^ (uint32_t)monoNowNs());
    seq_ = (uint16_t)rng();
    ssrc_ = rng();
}

void RtspRtpSender::setUdpTarget(int rtpFd, const sockaddr_in &clientRtp) {
    useTcp_ = false;
    udpFd_ = rtpFd;
    udpPeer_ = clientRtp;
}

void RtspRtpSender::setTcpSink(FrameSink sink, int rtpChannel) {
    useTcp_ = true;
    tcpSink_ = std::move(sink);
    tcpChannel_ = rtpChannel;
}

bool RtspRtpSender::sendRtp(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp,
                            bool marker) {
    if (payload == nullptr && payloadSize > 0)
        return false;

    /* 12 字节 RTP 头：V=2/P=0/X=0/CC=0，M 与 PT 在第二字节 */
    uint8_t hdr[12];
    hdr[0] = 0x80;
    hdr[1] = (uint8_t)(kPayloadType | (marker ? 0x80 : 0x00));
    uint16_t seq = seq_++;
    hdr[2] = (uint8_t)(seq >> 8);
    hdr[3] = (uint8_t)(seq & 0xff);
    hdr[4] = (uint8_t)(rtpTimestamp >> 24);
    hdr[5] = (uint8_t)(rtpTimestamp >> 16);
    hdr[6] = (uint8_t)(rtpTimestamp >> 8);
    hdr[7] = (uint8_t)(rtpTimestamp & 0xff);
    hdr[8] = (uint8_t)(ssrc_ >> 24);
    hdr[9] = (uint8_t)(ssrc_ >> 16);
    hdr[10] = (uint8_t)(ssrc_ >> 8);
    hdr[11] = (uint8_t)(ssrc_ & 0xff);

    bool ok = true;
    if (useTcp_) {
        /* "$" + channel + 16 位大端长度 + RTP 包，一次性写入 RTSP 连接 */
        uint16_t pktLen = (uint16_t)(sizeof(hdr) + payloadSize);
        scratch_.resize(4 + pktLen);
        scratch_[0] = '$';
        scratch_[1] = (uint8_t)tcpChannel_;
        scratch_[2] = (uint8_t)(pktLen >> 8);
        scratch_[3] = (uint8_t)(pktLen & 0xff);
        memcpy(scratch_.data() + 4, hdr, sizeof(hdr));
        if (payloadSize > 0)
            memcpy(scratch_.data() + 4 + sizeof(hdr), payload, payloadSize);
        if (tcpSink_)
            tcpSink_(scratch_.data(), scratch_.size());
    } else {
        /* UDP：iov 两段（头 + payload），零拷贝发出 */
        struct iovec iov[2];
        iov[0].iov_base = hdr;
        iov[0].iov_len = sizeof(hdr);
        iov[1].iov_base = const_cast<uint8_t *>(payload);
        iov[1].iov_len = payloadSize;
        struct msghdr msg = {};
        msg.msg_name = &udpPeer_;
        msg.msg_namelen = sizeof(udpPeer_);
        msg.msg_iov = iov;
        msg.msg_iovlen = 2;
        ok = sendmsg(udpFd_, &msg, MSG_NOSIGNAL) >= 0;
        if (!ok)
            LOGW(kTag, "sendmsg failed: %s", strerror(errno));
    }

    if (ok && rtcp_ != nullptr)
        rtcp_->onRtpSent(rtpTimestamp, payloadSize);
    return ok;
}

} // namespace darkos
