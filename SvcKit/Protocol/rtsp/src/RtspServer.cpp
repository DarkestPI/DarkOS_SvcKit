/*
 * RtspServerImpl：TCP 监听 + 连接/流注册表 + 播放引用计数表（见 .h 头注）
 */

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "RtspConnection.h"
#include "RtspServerImpl.h"
#include "base/EventLoop.h"
#include "base/Log.h"
#include "stream/SourceSink.h"

namespace darkos {

static constexpr const char *kTag = "RtspServer";

RtspServerImpl::RtspServerImpl(EventLoop *loop, const RtspServerOptions &opts)
    : loop_(loop), opts_(opts) {
    if (opts_.port == 0)
        opts_.port = 8554;
    if (!opts_.authUser.empty() && opts_.authPasswd.empty())
        LOGW(kTag, "auth user set but passwd empty, digest auth disabled");
}

RtspServerImpl::~RtspServerImpl() {
    /* 先断全部连接（其析构会归还 sourceOwners_ 占用），再收监听 socket */
    connections_.clear();
    if (listenFd_ >= 0) {
        loop_->unwatchFd(listenFd_);
        close(listenFd_);
        listenFd_ = -1;
    }
}

bool RtspServerImpl::addStream(const char *path, Source *source, MediaCodec codec) {
    if (path == nullptr || source == nullptr || path[0] == '\0' || path[0] == '/')
        return false;
    if (streams_.count(path) != 0) {
        LOGW(kTag, "stream \"%s\" already registered", path);
        return false;
    }
    RtspStreamInfo info;
    info.source = source;
    info.codec = codec;
    /* SDP 生成用打包器：DESCRIBE 取 rtpmapLine/fmtpLine。编码暂无实现时
     * 注册仍成功，DESCRIBE 回 500（打包器实现随 stream 层逐步落地） */
    info.sdpPacketizer.reset(createRtpPacketizer(codec));
    if (info.sdpPacketizer == nullptr)
        LOGW(kTag, "stream \"%s\": no packetizer for codec %u yet", path, (unsigned)codec);
    streams_.emplace(path, std::move(info));
    LOGI(kTag, "stream registered: rtsp://<ip>:%u/%s", opts_.port, path);
    return true;
}

int RtspServerImpl::start() {
    if (listenFd_ >= 0)
        return 0;
    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        int rc = -errno;
        LOGE(kTag, "create listen socket failed: %s", strerror(-rc));
        return rc;
    }
    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(opts_.port);
    if (bind(listenFd_, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listenFd_, 8) != 0) {
        int rc = -errno;
        LOGE(kTag, "bind/listen port %u failed: %s", opts_.port, strerror(-rc));
        close(listenFd_);
        listenFd_ = -1;
        return rc;
    }
    if (!loop_->watchFd(listenFd_, EPOLLIN, [this](uint32_t) { onAccept(); })) {
        LOGE(kTag, "watch listen fd failed");
        close(listenFd_);
        listenFd_ = -1;
        return -EIO;
    }
    LOGI(kTag, "RTSP server listening on 0.0.0.0:%u", opts_.port);
    return 0;
}

void RtspServerImpl::onAccept() {
    for (;;) {
        struct sockaddr_in peer = {};
        socklen_t len = sizeof(peer);
        int cfd = accept4(listenFd_, (struct sockaddr *)&peer, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0)
            break; /* EAGAIN：本轮 accept 完 */
        auto conn = std::make_unique<RtspConnection>(this, cfd, peer);
        RtspConnection *raw = conn.get();
        connections_.emplace(raw, std::move(conn));
        raw->start();
    }
    /* 一次性语义：重新注册 */
    loop_->watchFd(listenFd_, EPOLLIN, [this](uint32_t) { onAccept(); });
}

const RtspStreamInfo *RtspServerImpl::findStream(const std::string &path) const {
    auto it = streams_.find(path);
    return it == streams_.end() ? nullptr : &it->second;
}

void RtspServerImpl::acquireSource(Source *source) {
    ++sourceOwners_[source];
}

void RtspServerImpl::releaseSource(Source *source) {
    auto it = sourceOwners_.find(source);
    if (it == sourceOwners_.end())
        return;
    if (--it->second == 0)
        sourceOwners_.erase(it);
}

void RtspServerImpl::removeConnection(RtspConnection *conn) {
    auto it = connections_.find(conn);
    if (it == connections_.end())
        return;
    /* fd 回调还持有 this 裸指针：post 到事件循环下一轮再销毁。
     * post 的 Task 是 std::function（要求可拷贝），故先转 shared_ptr 再进 lambda */
    std::shared_ptr<RtspConnection> holder(std::move(it->second));
    connections_.erase(it);
    loop_->post([holder]() mutable { holder.reset(); });
}

RtspServer *RtspServer::create(EventLoop *loop, uint16_t port) {
    if (loop == nullptr)
        return nullptr;
    RtspServerOptions opts;
    opts.port = port;
    return new RtspServerImpl(loop, opts);
}

RtspServer *RtspServer::create(EventLoop *loop, const RtspServerOptions &opts) {
    if (loop == nullptr)
        return nullptr;
    return new RtspServerImpl(loop, opts);
}

} // namespace darkos
