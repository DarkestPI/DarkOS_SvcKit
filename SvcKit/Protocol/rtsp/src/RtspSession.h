#pragma once

/* ---------------------------------------------------------------------------
 * RtspSession：一路流在一个客户端上的播放会话（rtsp 层内部）
 *
 * 生命周期：SETUP 创建并确定传输（UDP 双 socket / TCP interleaved），
 * PLAY 开始拉流（Source::getNextFrame → packetize → sendRtp 的拉模型闭环），
 * PAUSE 停拉流，TEARDOWN/断连销毁。
 *
 * M1 单轨：一个会话对应一个 track；拉流回调经 weak_ptr 防护，会话销毁后
 * Source 挂起请求到达的帧被安全丢弃。
 * ------------------------------------------------------------------------- */

#include "stream/MediaPacket.h"

#include <netinet/in.h>

#include <cstdint>
#include <memory>
#include <string>

namespace darkos {

class EventLoop;
class RtspConnection;
class RtcpSession;
class RtpPacketizer;
class RtspRtpSender;
class Source;

class RtspSession : public std::enable_shared_from_this<RtspSession> {
public:
    RtspSession(EventLoop *loop, RtspConnection *conn, std::string id, Source *source,
                MediaCodec codec);
    ~RtspSession();

    /* SETUP：建立传输。UDP 失败（socket 耗尽等）返回 false 并填 errReason。 */
    bool setupTcp(int rtpChannel, int rtcpChannel);
    bool setupUdp(const sockaddr_in &clientAddr, uint16_t clientRtpPort,
                  uint16_t clientRtcpPort, std::string &errReason);

    /* PLAY：开始拉流。失败（编码不支持等）返回 false。 */
    bool play();
    /* PAUSE：停止拉流循环（挂起请求到达的帧丢弃，不欠新请求） */
    void pause();

    /* TCP 模式：连接层把奇数通道的交织帧（对端 RTCP）分流进来 */
    void onInterleavedFrame(int channel, const uint8_t *data, size_t size);

    const std::string &id() const { return id_; }
    bool isTcp() const { return tcp_; }
    bool isPlaying() const { return playing_; }
    int rtcpChannel() const { return rtcpChannel_; }
    uint16_t serverRtpPort() const { return serverRtpPort_; }
    uint16_t serverRtcpPort() const { return serverRtcpPort_; }
    uint32_t ssrc() const;

    /* PLAY 响应 RTP-Info 用：下一包序号（精确）与 RTP 时钟当前值
     * （近似：等 IDR 起播时首包时间戳随实际关键帧到达，误差 ≤ 一个 GOP） */
    uint16_t nextRtpSeq() const;
    uint32_t nextRtpTimestamp() const;

    /* 会话保活：RTCP 等活动到达时通知连接刷新空闲计时 */
    void touch();

private:
    void pullNext();
    bool ensurePipeline(); /* PLAY 时懒建打包器（每会话专属） */
    void onRtcpReadable(); /* UDP 模式：RTCP socket 收循环 */
    void sendRtcp(const uint8_t *data, size_t size);

    EventLoop *loop_;
    RtspConnection *conn_; /* 不持有：会话是连接的成员，先销毁 */
    std::string id_;
    Source *source_; /* 不持有，生命周期由注册方保证 */
    MediaCodec codec_;
    uint32_t clockRateHz_;

    bool tcp_ = false;
    int rtpChannel_ = 0;
    int rtcpChannel_ = 1;

    /* UDP 传输：RTP/RTCP 各一个 socket */
    int rtpFd_ = -1;
    int rtcpFd_ = -1;
    uint16_t serverRtpPort_ = 0;
    uint16_t serverRtcpPort_ = 0;
    sockaddr_in clientRtp_{};
    sockaddr_in clientRtcp_{};

    /* 播放管线（PLAY 时齐套） */
    RtspRtpSender *sender_ = nullptr; /* SETUP 时创建（要有传输参数） */
    RtpPacketizer *packetizer_ = nullptr;
    RtcpSession *rtcp_ = nullptr;

    bool playing_ = false;
    bool pullPending_ = false; /* 有挂起的 getNextFrame 请求 */
    bool awaitIdr_ = false;   /* 等 IDR 起播：非关键帧丢弃，首个关键帧后放行 */
};

} // namespace darkos
