#pragma once

/* ---------------------------------------------------------------------------
 * RtspRtpSender：stream::RtpSender 的 rtsp 层实现（模块内部类）
 *
 * 职责：给打包器交出的 payload 补 12 字节 RTP 头（V=2、PT=96、seq 自增、
 * 随机 ssrc、时间戳/marker 用逐包参数），再按 SETUP 协商的传输发出：
 * - UDP 单播：sendmsg 到客户端 client_port；
 * - TCP interleaved：拼 "$"+channel+len(大端) 交织帧写回 RTSP 连接。
 * 每发一包喂 RtcpSession::onRtpSent 统计（SR 用）。
 * ------------------------------------------------------------------------- */

#include "stream/RtpPacketizer.h"

#include <netinet/in.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace darkos {

class RtcpSession;

class RtspRtpSender : public RtpSender {
public:
    /* TCP interleaved 写出口：把完整交织帧（含 '$' 头）写入 RTSP 连接 */
    using FrameSink = std::function<void(const uint8_t *data, size_t size)>;

    explicit RtspRtpSender(uint32_t clockRateHz);
    ~RtspRtpSender() override = default;

    /* 传输目标二选一（SETUP 时按 Transport 头确定） */
    void setUdpTarget(int rtpFd, const sockaddr_in &clientRtp);
    void setTcpSink(FrameSink sink, int rtpChannel);

    void setRtcpSession(RtcpSession *rtcp) { rtcp_ = rtcp; }

    uint32_t ssrc() const { return ssrc_; }
    uint32_t clockRateHz() const { return clockRateHz_; }
    /* 下一个 RTP 包将使用的序号（PLAY 响应 RTP-Info 的 seq 用，精确值） */
    uint16_t nextSeq() const { return seq_; }

    bool sendRtp(const uint8_t *payload, size_t payloadSize, uint32_t rtpTimestamp,
                 bool marker) override;

private:
    static constexpr uint8_t kPayloadType = 96; /* 动态 PT，与 SDP 中一致 */

    uint32_t clockRateHz_;
    uint16_t seq_;  /* 随机初值，逐包自增 */
    uint32_t ssrc_; /* 随机 */
    RtcpSession *rtcp_ = nullptr; /* 不持有，仅统计回调 */

    bool useTcp_ = false;
    /* UDP 目标 */
    int udpFd_ = -1;
    sockaddr_in udpPeer_{};
    /* TCP 目标 */
    FrameSink tcpSink_;
    int tcpChannel_ = 0;

    std::vector<uint8_t> scratch_; /* TCP 组帧暂存（复用，避免逐包分配） */
};

} // namespace darkos
