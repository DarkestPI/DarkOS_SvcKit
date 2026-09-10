#pragma once

/* ---------------------------------------------------------------------------
 * RtspConnection：一个 RTSP 客户端连接（rtsp 层内部）
 *
 * 职责：
 * - 挂在 EventLoop 的 fd 事件上（一次性语义，每次回调后重新注册；
 *   同一 fd 只能注册一次，写阻塞时改注 EPOLLIN|EPOLLOUT 组合掩码）；
 * - 增量解析 "\r\n\r\n" 分隔的 RTSP 请求（兼容 Content-Length 体），
 *   "$" 开头的 interleaved 帧复用同一字节流，分流给会话的 RTCP；
 * - 方法分发：OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN +
 *   GET_PARAMETER/SET_PARAMETER（保活与参数语义）；ANNOUNCE/RECORD
 *   回 501（IPC 为纯源，无收流场景）；
 * - Digest 认证（RFC 2617，启用时）：除 OPTIONS 外的方法先过 401
 *   挑战/校验，同一连接认证一次；
 * - 会话空闲超时：请求与客户端 RTCP 刷新活动时间，超时断链
 *   （语义按 RFC 7826 勘误：SETUP 响应的 timeout= 不再是装饰）；
 * - 断连（读到 0/出错/EPOLLHUP）时清理全部会话并延迟销毁自身。
 * ------------------------------------------------------------------------- */

#include <netinet/in.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/TimeUtil.h" /* monoNowNs（touchActivity 内联用） */

namespace darkos {

class EventLoop;
class RtspServerImpl;
class RtspSession;
class Source;
struct RtspStreamInfo;

class RtspConnection {
public:
    RtspConnection(RtspServerImpl *server, int fd, const sockaddr_in &peer);
    ~RtspConnection();

    void start(); /* 挂 EPOLLIN 开始收请求（含超时巡检定时器） */

    /* 发送出口：响应文本与会话的 TCP interleaved 帧都走这里（保证顺序） */
    void sendBytes(const uint8_t *data, size_t size);
    void sendText(const std::string &text) {
        sendBytes((const uint8_t *)text.data(), text.size());
    }

    /* 会话保活：请求/RTCP 等活动到达时刷新空闲计时 */
    void touchActivity() { lastActivityNs_ = monoNowNs(); }

    int fd() const { return fd_; }

private:
    struct Request {
        std::string method;
        std::string url;
        std::map<std::string, std::string> headers; /* 键为小写 */
        std::string body;
        std::string cseq; /* 快捷取用（缺省 "0"） */
    };

    void onFd(uint32_t events);
    void rearmWatch();
    void flushTx();
    void disconnect();
    void closeAllSessions();

    void parseRx();
    void dispatchInterleaved(int channel, const uint8_t *data, size_t len);
    void handleRequest(const Request &req);

    /* 各方法处理 */
    void onOptions(const Request &req);
    void onDescribe(const Request &req);
    void onSetup(const Request &req);
    void onPlay(const Request &req);
    void onPause(const Request &req);
    void onTeardown(const Request &req);
    void onGetOrSetParameter(const Request &req); /* GET/SET_PARAMETER 统一入口 */

    /* ---- 认证（RFC 2617 Digest，服务器启用时）---- */
    void sendAuthChallenge(const Request &req); /* 401 + WWW-Authenticate */
    bool checkAuthorization(const Request &req); /* 校验 Authorization 应答 */

    void onWatchdog(); /* 空闲超时巡检 */

    void respond(int code, const char *reason, const std::string &cseq,
                 const std::string &extraHeaders, const std::string &body = std::string(),
                 const char *contentType = nullptr);

    /* 由 url 解析出已注册的流与流基准 URL（剥掉 track 后缀） */
    bool resolveStream(const std::string &url, std::string &baseUrlOut,
                       const RtspStreamInfo *&infoOut);

    RtspServerImpl *server_; /* 不持有：服务器生命周期覆盖连接 */
    EventLoop *loop_;
    int fd_;
    std::string peerDesc_; /* "ip:port"，日志用 */

    bool closed_ = false;
    bool watched_ = false;   /* 当前 fd 已挂在 loop 上 */
    bool txPending_ = false; /* txBuf_ 非空且在等 EPOLLOUT */

    /* 会话空闲超时（0 = 不巡检） */
    uint64_t timeoutNs_ = 0;
    uint64_t watchdogTimerId_ = 0;
    uint64_t lastActivityNs_ = 0;

    /* Digest 认证状态：同一连接认证一次即放行 */
    bool authOk_ = false;
    std::string pendingNonce_; /* 最近一次 401 挑战下发的 nonce */

    std::string rxBuf_; /* 增量解析缓冲（请求 + interleaved 帧混排） */
    std::string txBuf_; /* 写阻塞时的积压（响应 + TCP interleaved 帧），
                         * 超过高水位（options().txMaxPendingBytes）断开慢客户端 */

    /* sessionId -> 会话；playingSources_ 记录播放关系（sessionId -> Source*），
     * 用于 PAUSE/TEARDOWN/断连时归还服务器的播放引用计数 */
    std::map<std::string, std::shared_ptr<RtspSession>> sessions_;
    std::map<std::string, Source *> playingSources_;
};

} // namespace darkos
