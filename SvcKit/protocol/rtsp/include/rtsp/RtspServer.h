#pragma once

/* ---------------------------------------------------------------------------
 * RtspServer：RTSP 服务器（SvcKit/Protocol/rtsp，M1 + M1.5 补全）
 *
 * 标准基线（详见本目录 readme.md）：
 * - 信令：RTSP/1.0（RFC 2326）；选择性吸收 RFC 7826（RTSP 2.0）中不改变
 *   版本号的勘误（会话超时语义、RTP-Info 规则）——线上保持 1.0 是为了
 *   与 VLC/ffmpeg/live555 等主流客户端互操作
 * - 认证：HTTP Digest（RFC 2617，MD5 + qop="auth"，默认关闭）
 * - 媒体：RTP/RTCP（RFC 3550）
 *
 * 能力（详见本目录 readme.md）：
 * - 方法：OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / TEARDOWN /
 *   GET_PARAMETER / SET_PARAMETER；ANNOUNCE/RECORD 回 501（IPC 是纯源）
 * - 传输：RTP/AVP UDP 单播 + RTP over TCP interleaved（组播不做）
 * - PLAY 等关键帧起播（GOP 中途接入不花屏）；会话空闲超时断链
 * - 多客户端共享同一路流（Source fan-out：一帧编码，多会话各自打包发送，
 *   每会话独立 RTP 序号/时间戳）
 * - 慢客户端背压：TCP 发送积压超过 txMaxPendingBytes 即断开该客户端
 *   （VLC/NVR 会自动重连），防止弱网客户端把固件内存拖爆
 * - 单轨流（一路视频或一路音频）
 *
 * 用法：
 *   EventLoop *loop = EventLoop::create();
 *   RtspServer *server = RtspServer::create(loop, 8554);
 *   server->addStream("live", source, MediaCodec::kH264); // rtsp://<ip>:8554/live
 *   if (server->start() != 0) { ... }
 *   loop->run();
 *
 * 线程约定：create/addStream/start 与所有内部回调都运行在 loop 线程。
 * Source 生命周期由调用方保证，须覆盖服务器整个运行期。
 * ------------------------------------------------------------------------- */

#include <cstdint>
#include <string>

#include "stream/MediaPacket.h" /* MediaCodec */

namespace darkos {

class EventLoop;
class Source;

/* 服务器选项（默认值即出厂行为；认证默认关闭） */
struct RtspServerOptions {
    uint16_t port = 8554;          /* 监听端口 */
    uint32_t sessionTimeoutS = 60; /* 会话空闲超时：请求/RTCP 超过此时长
                                    * 无活动则 teardown（0 = 不超时） */
    std::string authUser;          /* Digest 认证账号；user/passwd 均非空才启用
                                    * （默认留空 = 认证关闭） */
    std::string authPasswd;
    std::string authRealm = "darkos";           /* 挑战 realm */
    size_t txMaxPendingBytes = 2 * 1024 * 1024; /* TCP 发送积压高水位（响应 +
                                                 * interleaved 帧合计）：超过即断开
                                                 * 该慢客户端（0 = 不限制）。默认
                                                 * 2MB ≈ 5Mbps 码流约 3 秒 */
};

class RtspServer {
  public:
    /* 创建服务器（默认端口 8554）。返回对象由调用方 delete。 */
    static RtspServer *create(EventLoop *loop, uint16_t port = 8554);
    /* 带选项创建（认证/超时等）。 */
    static RtspServer *create(EventLoop *loop, const RtspServerOptions &opts);

    virtual ~RtspServer() = default;

    /* 注册一路流：path 不带斜杠（如 "live"），codec 决定 SDP 与打包器。
     * source 生命周期须覆盖服务器运行期；path 重复返回 false。 */
    virtual bool addStream(const char *path, Source *source, MediaCodec codec) = 0;

    /* 开始在 loop 上监听（幂等，可重复调用）。返回 0 成功，负 errno 失败。 */
    virtual int start() = 0;
};

} // namespace darkos
