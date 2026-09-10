#pragma once

/* ---------------------------------------------------------------------------
 * RtspServerImpl / RtspStreamInfo：服务器内部共享声明（rtsp 层内部头）
 *
 * RtspServer.cpp 实现服务器本体；RtspConnection.cpp 经本头访问流注册表
 * 与播放引用计数表。
 * ------------------------------------------------------------------------- */

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "rtsp/RtspServer.h"
#include "stream/MediaPacket.h"
#include "stream/RtpPacketizer.h"
#include "stream/Sdp.h"

namespace darkos {

class RtspConnection;
class Source;

/* 一路已注册的流 */
struct RtspStreamInfo {
    Source *source = nullptr; /* 不持有，生命周期由注册方保证 */
    MediaCodec codec = MediaCodec::kH264;
    /* SDP 生成专用的打包器实例（只取 rtpmapLine/fmtpLine，不打包）。
     * 注册流时创建；为 nullptr 说明该编码暂无打包器，DESCRIBE 回 500。 */
    std::unique_ptr<RtpPacketizer> sdpPacketizer;

    /* SDP 缓存：首个 DESCRIBE 惰性生成，之后直接复用（SDP 内容只取决于
     * 注册时的编码参数）。sdpBuilder 必须随流持久持有且堆分配：SdpBuilder
     * 实现以 this 为键在文件级静态表存状态（见 stream/src/sdp/Sdp.cpp，
     * 公开头无析构钩子），栈上临时 builder 销毁后地址被复用时会继承旧
     * track，导致 SDP 重复 m= 段。mutable：经 const findStream 惰性填充。 */
    mutable std::unique_ptr<SdpBuilder> sdpBuilder;
    mutable std::string sdp;
};

class RtspServerImpl : public RtspServer {
  public:
    RtspServerImpl(EventLoop *loop, const RtspServerOptions &opts);
    ~RtspServerImpl() override;

    bool addStream(const char *path, Source *source, MediaCodec codec) override;
    int start() override;

    /* ---- 供 RtspConnection 使用 ---- */
    EventLoop *loop() const {
        return loop_;
    }
    const RtspServerOptions &options() const {
        return opts_;
    }
    const RtspStreamInfo *findStream(const std::string &path) const;

    /* 播放引用计数：PLAY 前 +1，PAUSE/TEARDOWN/断连时 -1——多客户端
     * 共享同一路流（Source fan-out），归零即无人观看。 */
    void acquireSource(Source *source);
    void releaseSource(Source *source);

    /* 连接断开时摘除并延迟销毁（fd 回调正持有裸指针，post 到下一轮销毁） */
    void removeConnection(RtspConnection *conn);

  private:
    void onAccept();

    EventLoop *loop_;
    RtspServerOptions opts_;
    int listenFd_ = -1;

    std::map<std::string, RtspStreamInfo> streams_; /* path -> 流 */
    std::map<RtspConnection *, std::unique_ptr<RtspConnection>> connections_;
    std::unordered_map<Source *, size_t> sourceOwners_; /* source -> 播放会话数 */
};

} // namespace darkos
