/*
 * RtspSession：传输建立 + 拉流闭环 + RTCP 通道（见 .h 头注）
 */

#include "stream/RtcpSession.h"
#include "stream/RtpPacketizer.h"
#include "stream/SourceSink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/EventLoop.h"
#include "base/Log.h"
#include "base/TimeUtil.h"

#include "RtspConnection.h"
#include "RtspRtpSender.h"
#include "RtspSession.h"

namespace darkos {

static constexpr const char *kTag = "RtspSession";

/* 编码 → RTP 时钟频率（RFC 3551 / 3640 / 6184 / 7798） */
static uint32_t clockRateFor(MediaCodec codec) {
    switch (codec) {
    case MediaCodec::kG711A:
    case MediaCodec::kG711U:
        return 8000;
    default:
        return 90000; /* H.264/H.265/AAC */
    }
}

RtspSession::RtspSession(EventLoop *loop, RtspConnection *conn, std::string id, Source *source,
                         MediaCodec codec)
    : loop_(loop), conn_(conn), id_(std::move(id)), source_(source), codec_(codec),
      clockRateHz_(clockRateFor(codec)) {}

RtspSession::~RtspSession() {
    playing_ = false;
    if (rtcpFd_ >= 0) {
        loop_->unwatchFd(rtcpFd_);
        close(rtcpFd_);
    }
    if (rtpFd_ >= 0)
        close(rtpFd_);
    delete packetizer_;
    delete rtcp_;
    delete sender_;
    LOGI(kTag, "session %s teardown", id_.c_str());
}

uint32_t RtspSession::ssrc() const {
    return sender_ != nullptr ? sender_->ssrc() : 0;
}

uint16_t RtspSession::nextRtpSeq() const {
    return sender_ != nullptr ? sender_->nextSeq() : 0;
}

uint32_t RtspSession::nextRtpTimestamp() const {
    /* 打包器以 ptsNs（CLOCK_MONOTONIC）按 clockRate 换算 RTP 时间戳，
     * 取当前时刻的映射值近似"下一包"时间戳 */
    return nsToRtpTicks(monoNowNs(), clockRateHz_);
}

void RtspSession::touch() {
    conn_->touchActivity();
}

bool RtspSession::setupTcp(int rtpChannel, int rtcpChannel) {
    tcp_ = true;
    rtpChannel_ = rtpChannel;
    rtcpChannel_ = rtcpChannel;

    sender_ = new RtspRtpSender(clockRateHz_);
    RtspConnection *conn = conn_;
    sender_->setTcpSink([conn](const uint8_t *data, size_t size) { conn->sendBytes(data, size); },
                        rtpChannel_);

    rtcp_ = createRtcpSession(sender_->ssrc(), clockRateHz_);
    if (rtcp_ != nullptr) {
        sender_->setRtcpSession(rtcp_);
        std::weak_ptr<RtspSession> weak = weak_from_this();
        rtcp_->setSendCallback([weak](const uint8_t *data, size_t size) {
            auto self = weak.lock();
            if (self)
                self->sendRtcp(data, size);
        });
    }
    return true;
}

bool RtspSession::setupUdp(const sockaddr_in &clientAddr, uint16_t clientRtpPort,
                           uint16_t clientRtcpPort, std::string &errReason) {
    tcp_ = false;
    rtpFd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    rtcpFd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (rtpFd_ < 0 || rtcpFd_ < 0) {
        errReason = strerror(errno);
        LOGE(kTag, "session %s: create udp socket failed: %s", id_.c_str(), errReason.c_str());
        return false;
    }
    /* 端口 0 让内核分配，getsockname 取回响应给客户端（server_port） */
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (bind(rtpFd_, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        bind(rtcpFd_, (struct sockaddr *)&local, sizeof(local)) != 0) {
        errReason = strerror(errno);
        LOGE(kTag, "session %s: bind failed: %s", id_.c_str(), errReason.c_str());
        return false;
    }
    socklen_t len = sizeof(local);
    if (getsockname(rtpFd_, (struct sockaddr *)&local, &len) == 0)
        serverRtpPort_ = ntohs(local.sin_port);
    if (getsockname(rtcpFd_, (struct sockaddr *)&local, &len) == 0)
        serverRtcpPort_ = ntohs(local.sin_port);

    clientRtp_ = clientAddr;
    clientRtp_.sin_port = htons(clientRtpPort);
    clientRtcp_ = clientAddr;
    clientRtcp_.sin_port = htons(clientRtcpPort);

    sender_ = new RtspRtpSender(clockRateHz_);
    sender_->setUdpTarget(rtpFd_, clientRtp_);

    rtcp_ = createRtcpSession(sender_->ssrc(), clockRateHz_);
    if (rtcp_ != nullptr) {
        sender_->setRtcpSession(rtcp_);
        std::weak_ptr<RtspSession> weak = weak_from_this();
        rtcp_->setSendCallback([weak](const uint8_t *data, size_t size) {
            auto self = weak.lock();
            if (self)
                self->sendRtcp(data, size);
        });
    }

    /* RTCP 收：一次性语义，回调里重新 watch */
    loop_->watchFd(rtcpFd_, EPOLLIN, [this](uint32_t) { onRtcpReadable(); });
    return true;
}

void RtspSession::onRtcpReadable() {
    uint8_t buf[2048];
    for (;;) {
        ssize_t n = recvfrom(rtcpFd_, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n > 0) {
            touch(); /* 客户端 RTCP 到达视为会话保活 */
            if (rtcp_ != nullptr)
                rtcp_->onRtcpPacket(buf, (size_t)n);
        } else {
            break; /* EAGAIN：本轮收完；出错则等下一轮回调 */
        }
    }
    loop_->watchFd(rtcpFd_, EPOLLIN, [this](uint32_t) { onRtcpReadable(); });
}

void RtspSession::onInterleavedFrame(int channel, const uint8_t *data, size_t size) {
    if (channel == rtcpChannel_ && rtcp_ != nullptr)
        rtcp_->onRtcpPacket(data, size);
    /* 偶数通道是客户端→服务器的 RTP（出流场景不会出现），忽略 */
}

void RtspSession::sendRtcp(const uint8_t *data, size_t size) {
    if (tcp_) {
        /* TCP：交织帧走 rtcpChannel（RTP 通道 + 1）写回 RTSP 连接 */
        if (size > 0xffff)
            return;
        uint8_t hdr[4];
        hdr[0] = '$';
        hdr[1] = (uint8_t)rtcpChannel_;
        hdr[2] = (uint8_t)(size >> 8);
        hdr[3] = (uint8_t)(size & 0xff);
        conn_->sendBytes(hdr, sizeof(hdr));
        conn_->sendBytes(data, size);
    } else {
        if (sendto(rtcpFd_, data, size, MSG_NOSIGNAL, (struct sockaddr *)&clientRtcp_,
                   sizeof(clientRtcp_)) < 0)
            LOGW(kTag, "session %s: send rtcp failed: %s", id_.c_str(), strerror(errno));
    }
}

bool RtspSession::ensurePipeline() {
    if (packetizer_ != nullptr)
        return true;
    packetizer_ = createRtpPacketizer(codec_);
    if (packetizer_ == nullptr) {
        LOGE(kTag, "session %s: no packetizer for codec %u", id_.c_str(), (unsigned)codec_);
        return false;
    }
    packetizer_->attach(sender_, clockRateHz_);
    return true;
}

bool RtspSession::play() {
    if (playing_)
        return true;
    if (!ensurePipeline())
        return false;
    playing_ = true;
    /* 等 IDR 起播：客户端在 GOP 中途接入（PLAY/PAUSE 后重新 PLAY）时，
     * 先丢弃非关键帧，首个关键帧到达才开始发送，避免客户端收到无法
     * 解码的 P 帧花屏（参考 live555 行为；PAUSE 后解码器状态可能过期，
     * 重新 PLAY 同样置位） */
    awaitIdr_ = true;
    LOGI(kTag, "session %s: play (%s, await idr)", id_.c_str(), tcp_ ? "tcp" : "udp");
    pullNext();
    return true;
}

void RtspSession::pause() {
    if (!playing_)
        return;
    playing_ = false;
    LOGI(kTag, "session %s: pause", id_.c_str());
    /* 挂起的 getNextFrame 请求无法撤销：帧到达时被回调丢弃，不欠新请求 */
}

void RtspSession::pullNext() {
    if (!playing_ || pullPending_)
        return;
    pullPending_ = true;
    std::weak_ptr<RtspSession> weak = weak_from_this();
    source_->getNextFrame([weak](const MediaPacket &pkt) {
        auto self = weak.lock();
        if (!self)
            return; /* 会话已销毁（TEARDOWN/断连），帧安全丢弃 */
        self->pullPending_ = false;
        if (!self->playing_)
            return; /* PAUSE 期间到达的帧丢弃 */
        if (self->awaitIdr_ && !pkt.keyframe) {
            /* 等关键帧期间的非关键帧跳过打包，但拉流循环照常续期 */
        } else {
            if (self->awaitIdr_) {
                self->awaitIdr_ = false;
                LOGI(kTag, "session %s: keyframe arrived, streaming starts",
                     self->id_.c_str());
            }
            self->packetizer_->packetize(pkt);
        }
        /* 拉模型闭环：帧交付后再要下一帧。经 post 续拉而不是直接递归——
         * 同步 Source（回调内立即交付）会造成无限递归爆栈 */
        self->loop_->post([weak] {
            auto s = weak.lock();
            if (s)
                s->pullNext();
        });
    });
}

} // namespace darkos
