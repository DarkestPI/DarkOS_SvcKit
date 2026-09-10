/*
 * RtspConnection：RTSP 请求增量解析 + 方法分发 + 发送缓冲（见 .h 头注）
 */

#include "stream/RtpPacketizer.h"
#include "stream/Sdp.h"
#include "stream/SourceSink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <random>
#include <vector>

#include "base/EventLoop.h"
#include "base/Hash.h"
#include "base/Log.h"
#include "base/TimeUtil.h"

#include "RtspConnection.h"
#include "RtspServerImpl.h"
#include "RtspSession.h"

namespace darkos {

static constexpr const char *kTag = "RtspConn";
static constexpr size_t kMaxRequestBytes = 64 * 1024; /* 防恶意/乱码连接撑爆缓冲 */

namespace {

/* 公开方法清单（OPTIONS 的 Public 头与 405 响应共用） */
constexpr const char *kPublicHeader =
    "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, "
    "SET_PARAMETER\r\n";

/* 16 位十六进制随机 Session id（单线程 reactor，静态 rng 无竞争） */
std::string genSessionId() {
    static std::mt19937 rng(std::random_device{}() ^ (uint32_t)monoNowNs());
    char buf[17];
    uint64_t v = ((uint64_t)rng() << 32) | rng();
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return buf;
}

/* 32 字符十六进制随机 nonce（Digest 挑战用） */
std::string genNonce() {
    static std::mt19937 rng(std::random_device{}() ^ (uint32_t)monoNowNs());
    char buf[33];
    for (int i = 0; i < 32; i += 8)
        snprintf(buf + i, 9, "%08x", (unsigned)rng());
    return buf;
}

std::string toLower(std::string s) {
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    size_t b = s.find_first_not_of(" \t");
    size_t e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

/* 解析 Authorization 参数表（"Digest k1=v1, k2=\"v2\", …" 去掉 Digest
 * 前缀后的部分）：逗号分隔 k=v，v 可带双引号（本表无含逗号的值，够用） */
std::map<std::string, std::string> parseAuthParams(const std::string &s) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        if (comma == std::string::npos)
            comma = s.size();
        std::string item = trim(s.substr(pos, comma - pos));
        pos = comma + 1;
        auto eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = toLower(trim(item.substr(0, eq)));
        std::string v = trim(item.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        if (!k.empty())
            out[k] = v;
    }
    return out;
}

/* url → 去 scheme/host/query/尾斜杠后的路径，如 "live" 或 "live/track0" */
std::string urlToPath(const std::string &url) {
    std::string s = url;
    auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        s = s.substr(scheme + 3);
        auto slash = s.find('/');
        s = slash == std::string::npos ? std::string() : s.substr(slash);
    }
    auto q = s.find('?');
    if (q != std::string::npos)
        s.resize(q);
    while (!s.empty() && s.front() == '/')
        s.erase(0, 1);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

/* url 去 query 与尾斜杠（拼 track URL 的基准） */
std::string cleanUrl(const std::string &url) {
    std::string s = url;
    auto q = s.find('?');
    if (q != std::string::npos)
        s.resize(q);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

/* 打包器的 rtpmapLine/fmtpLine 形如 "96 H264/90000"，剥掉 PT 前缀交给 SdpBuilder
 * （SdpBuilder::addTrack/setFmtp 已单独持有 PT，输出时自己拼 "a=rtpmap:<pt> ..."） */
std::string stripPtPrefix(const char *line) {
    if (line == nullptr)
        return std::string();
    std::string s = line;
    auto sp = s.find(' ');
    if (sp != std::string::npos && sp > 0 &&
        s.find_first_not_of("0123456789") == sp) /* 前缀是纯数字才剥 */
        return s.substr(sp + 1);
    return s;
}

struct TransportSpec {
    bool tcp = false;
    bool hasClientPorts = false;
    uint16_t clientRtpPort = 0;
    uint16_t clientRtcpPort = 0;
    int rtpChannel = 0;
    int rtcpChannel = 1;
};

/* 解析 Transport 头："RTP/AVP;unicast;client_port=A-B" 或
 * "RTP/AVP/TCP;unicast;interleaved=0-1"（字段可能多个以逗号分隔，逐段匹配） */
bool parseTransport(const std::string &value, TransportSpec &out) {
    std::string v = toLower(value);
    if (v.find("rtp/avp/tcp") != std::string::npos)
        out.tcp = true;
    else if (v.find("rtp/avp") != std::string::npos)
        out.tcp = false;
    else
        return false;

    auto parsePair = [&v](const char *key, int &a, int &b) -> bool {
        size_t p = 0;
        while ((p = v.find(key, p)) != std::string::npos) {
            /* 键须出现在段首或 ';' 后，避免 "xclient_port=" 误配 */
            if (p > 0 && v[p - 1] != ';' && v[p - 1] != ',' && v[p - 1] != ' ') {
                p += strlen(key);
                continue;
            }
            const char *num = v.c_str() + p + strlen(key);
            char *end = nullptr;
            long x = strtol(num, &end, 10);
            if (end == num)
                return false;
            long y = x + 1;
            if (*end == '-') {
                long z = strtol(end + 1, nullptr, 10);
                if (z >= 0)
                    y = z;
            }
            if (x < 0 || x > 0xffff || y < 0 || y > 0xffff)
                return false;
            a = (int)x;
            b = (int)y;
            return true;
        }
        return false;
    };

    int a = 0, b = 0;
    if (parsePair("client_port=", a, b)) {
        out.hasClientPorts = true;
        out.clientRtpPort = (uint16_t)a;
        out.clientRtcpPort = (uint16_t)b;
    }
    if (parsePair("interleaved=", a, b)) {
        out.rtpChannel = a;
        out.rtcpChannel = b;
    }
    return true;
}

} // namespace

RtspConnection::RtspConnection(RtspServerImpl *server, int fd, const sockaddr_in &peer)
    : server_(server), loop_(server->loop()), fd_(fd) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    peerDesc_ = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));

    const RtspServerOptions &opts = server_->options();
    if (opts.sessionTimeoutS != 0)
        timeoutNs_ = (uint64_t)opts.sessionTimeoutS * 1000000000ULL;
    lastActivityNs_ = monoNowNs();
}

RtspConnection::~RtspConnection() {
    if (watchdogTimerId_ != 0)
        loop_->cancel(watchdogTimerId_);
    closeAllSessions(); /* 会话先销毁：其 TCP 发送回调引用本连接 */
    if (fd_ >= 0) {
        loop_->unwatchFd(fd_);
        close(fd_);
        fd_ = -1;
    }
}

void RtspConnection::start() {
    rearmWatch();
    if (timeoutNs_ != 0) {
        /* 巡检间隔取超时的 1/3，夹在 [1s, 10s]：默认 60s 超时即 10s 一查 */
        uint64_t interval = timeoutNs_ / 3;
        if (interval < 1000000000ULL)
            interval = 1000000000ULL;
        if (interval > 10000000000ULL)
            interval = 10000000000ULL;
        watchdogTimerId_ =
            loop_->scheduleEvery(interval, interval, [this] { onWatchdog(); });
    }
}

void RtspConnection::onWatchdog() {
    if (timeoutNs_ == 0 || monoNowNs() - lastActivityNs_ <= timeoutNs_)
        return;
    LOGW(kTag, "%s: idle over %us timeout, disconnect", peerDesc_.c_str(),
         (unsigned)(timeoutNs_ / 1000000000ULL));
    disconnect();
}

void RtspConnection::rearmWatch() {
    uint32_t events = EPOLLIN;
    if (txPending_)
        events |= EPOLLOUT;
    if (!loop_->watchFd(fd_, events, [this](uint32_t ev) { onFd(ev); })) {
        LOGE(kTag, "%s: watchFd failed", peerDesc_.c_str());
        disconnect();
        return;
    }
    watched_ = true;
}

/* ------------------------------ 收发 ----------------------------------- */

void RtspConnection::onFd(uint32_t events) {
    if (closed_)
        return;
    watched_ = false; /* 一次性语义：回调触发即摘除 */

    if ((events & (EPOLLERR | EPOLLHUP)) != 0 && (events & EPOLLIN) == 0) {
        disconnect();
        return;
    }
    if ((events & EPOLLOUT) != 0) {
        flushTx();
        if (closed_)
            return;
    }
    if ((events & EPOLLIN) != 0) {
        uint8_t buf[8192];
        ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            rxBuf_.append((const char *)buf, (size_t)n);
            parseRx();
            if (closed_)
                return;
        } else if (n == 0) {
            LOGI(kTag, "%s: client closed", peerDesc_.c_str());
            disconnect();
            return;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOGW(kTag, "%s: recv failed: %s", peerDesc_.c_str(), strerror(errno));
            disconnect();
            return;
        }
    }
    if (!watched_)
        rearmWatch();
}

void RtspConnection::sendBytes(const uint8_t *data, size_t size) {
    if (closed_ || size == 0)
        return;
    /* 慢客户端背压：积压超过高水位即断开（拉流回调栈上持有会话强引用、
     * 连接销毁经 post 延迟，此处断开是安全的）。VLC/NVR 会自动重连 */
    size_t txMax = server_->options().txMaxPendingBytes;
    if (txMax != 0 && txBuf_.size() + size > txMax) {
        LOGW(kTag, "%s: tx backlog %zu(+%zu) exceeds %zu, disconnect slow client",
             peerDesc_.c_str(), txBuf_.size(), size, txMax);
        disconnect();
        return;
    }
    txBuf_.append((const char *)data, size);
    flushTx();
}

void RtspConnection::flushTx() {
    while (!txBuf_.empty()) {
        ssize_t n = send(fd_, txBuf_.data(), txBuf_.size(), MSG_NOSIGNAL);
        if (n > 0) {
            txBuf_.erase(0, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 写阻塞：改注 EPOLLIN|EPOLLOUT（同 fd 只能注册一次，先摘） */
            if (!txPending_) {
                txPending_ = true;
                if (watched_) {
                    loop_->unwatchFd(fd_);
                    watched_ = false;
                }
                rearmWatch();
            }
            return;
        }
        LOGW(kTag, "%s: send failed: %s", peerDesc_.c_str(), strerror(errno));
        disconnect();
        return;
    }
    if (txPending_) {
        /* 发完了：恢复只监听读 */
        txPending_ = false;
        if (watched_) {
            loop_->unwatchFd(fd_);
            watched_ = false;
        }
        rearmWatch();
    }
}

void RtspConnection::disconnect() {
    if (closed_)
        return;
    closed_ = true;
    if (watched_) {
        loop_->unwatchFd(fd_);
        watched_ = false;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    closeAllSessions();
    LOGI(kTag, "%s: disconnected", peerDesc_.c_str());
    server_->removeConnection(this); /* post 延迟销毁本对象 */
}

void RtspConnection::closeAllSessions() {
    for (auto &kv : playingSources_)
        server_->releaseSource(kv.second);
    playingSources_.clear();
    sessions_.clear();
}

/* ------------------------------ 解析 ----------------------------------- */

void RtspConnection::parseRx() {
    for (;;) {
        if (rxBuf_.empty())
            return;

        /* interleaved 帧：'$' + channel + 16 位大端长度 + 数据 */
        if (rxBuf_[0] == '$') {
            if (rxBuf_.size() < 4)
                return;
            int channel = (uint8_t)rxBuf_[1];
            size_t len = ((size_t)(uint8_t)rxBuf_[2] << 8) | (uint8_t)rxBuf_[3];
            if (rxBuf_.size() < 4 + len)
                return;
            dispatchInterleaved(channel, (const uint8_t *)rxBuf_.data() + 4, len);
            rxBuf_.erase(0, 4 + len);
            continue;
        }

        auto hdrEnd = rxBuf_.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) {
            if (rxBuf_.size() > kMaxRequestBytes) {
                LOGW(kTag, "%s: request too large, drop connection", peerDesc_.c_str());
                disconnect();
            }
            return;
        }

        /* 头部块按行解析 */
        Request req;
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < hdrEnd) {
            auto eol = rxBuf_.find("\r\n", pos);
            if (eol == std::string::npos || eol > hdrEnd)
                eol = hdrEnd;
            lines.push_back(rxBuf_.substr(pos, eol - pos));
            pos = eol + 2;
        }
        if (lines.empty()) {
            rxBuf_.erase(0, hdrEnd + 4);
            continue;
        }

        /* 请求行：METHOD URL RTSP/1.0 */
        {
            auto sp1 = lines[0].find(' ');
            auto sp2 = sp1 == std::string::npos ? std::string::npos : lines[0].find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                respond(400, "Bad Request", "0", std::string());
                rxBuf_.erase(0, hdrEnd + 4);
                continue;
            }
            req.method = lines[0].substr(0, sp1);
            req.url = lines[0].substr(sp1 + 1, sp2 - sp1 - 1);
        }
        size_t contentLength = 0;
        for (size_t i = 1; i < lines.size(); i++) {
            auto colon = lines[i].find(':');
            if (colon == std::string::npos)
                continue;
            std::string key = toLower(trim(lines[i].substr(0, colon)));
            std::string val = trim(lines[i].substr(colon + 1));
            req.headers[key] = val;
            if (key == "content-length")
                contentLength = (size_t)strtoul(val.c_str(), nullptr, 10);
        }
        req.cseq = req.headers.count("cseq") != 0 ? req.headers["cseq"] : "0";

        /* 等 Content-Length 指示的体收齐（SETUP 等带体场景兼容） */
        size_t total = hdrEnd + 4 + contentLength;
        if (rxBuf_.size() < total)
            return;
        req.body = rxBuf_.substr(hdrEnd + 4, contentLength);

        handleRequest(req);
        rxBuf_.erase(0, total);
        if (closed_)
            return;
    }
}

void RtspConnection::dispatchInterleaved(int channel, const uint8_t *data, size_t len) {
    touchActivity(); /* 客户端交织帧（RTCP）到达视为保活 */
    for (auto &kv : sessions_) {
        if (kv.second->isTcp() && channel == kv.second->rtcpChannel()) {
            kv.second->onInterleavedFrame(channel, data, len);
            return;
        }
    }
    /* 偶数通道是客户端→服务器的 RTP（出流场景不出现）；无会话的帧丢弃 */
    LOGD(kTag, "%s: drop interleaved frame channel=%d len=%zu", peerDesc_.c_str(), channel, len);
}

/* ------------------------------ 方法 ----------------------------------- */

void RtspConnection::handleRequest(const Request &req) {
    LOGD(kTag, "%s: %s %s (CSeq %s)", peerDesc_.c_str(), req.method.c_str(), req.url.c_str(),
         req.cseq.c_str());
    touchActivity();

    /* 认证（启用时）：OPTIONS 保持开放供客户端探测能力，其余方法先过挑战 */
    const RtspServerOptions &opts = server_->options();
    if (!authOk_ && !opts.authUser.empty() && !opts.authPasswd.empty() &&
        req.method != "OPTIONS") {
        if (!checkAuthorization(req))
            return; /* 已回 401 挑战，等客户端带凭证重试 */
        authOk_ = true;
        LOGI(kTag, "%s: digest auth ok (user=%s)", peerDesc_.c_str(), opts.authUser.c_str());
    }

    if (req.method == "OPTIONS")
        onOptions(req);
    else if (req.method == "DESCRIBE")
        onDescribe(req);
    else if (req.method == "SETUP")
        onSetup(req);
    else if (req.method == "PLAY")
        onPlay(req);
    else if (req.method == "PAUSE")
        onPause(req);
    else if (req.method == "TEARDOWN")
        onTeardown(req);
    else if (req.method == "GET_PARAMETER" || req.method == "SET_PARAMETER")
        onGetOrSetParameter(req);
    else if (req.method == "ANNOUNCE" || req.method == "RECORD")
        /* IPC 摄像机是纯源，无推流/录制接入场景；明确 501 而非笼统 405 */
        respond(501, "Not Implemented", req.cseq, std::string());
    else
        respond(405, "Method Not Allowed", req.cseq, kPublicHeader);
}

void RtspConnection::onOptions(const Request &req) {
    respond(200, "OK", req.cseq, kPublicHeader);
}

bool RtspConnection::resolveStream(const std::string &url, std::string &baseUrlOut,
                                   const RtspStreamInfo *&infoOut) {
    std::string base = cleanUrl(url);
    std::string path = urlToPath(base);
    infoOut = server_->findStream(path);
    if (infoOut != nullptr) {
        baseUrlOut = base;
        return true;
    }
    /* 带 track 后缀（live/track0）：剥掉最后一段再匹配 */
    auto slash = path.rfind('/');
    if (slash != std::string::npos) {
        infoOut = server_->findStream(path.substr(0, slash));
        if (infoOut != nullptr) {
            auto urlSlash = base.rfind('/');
            baseUrlOut = urlSlash == std::string::npos ? base : base.substr(0, urlSlash);
            return true;
        }
    }
    return false;
}

void RtspConnection::onDescribe(const Request &req) {
    std::string baseUrl;
    const RtspStreamInfo *info = nullptr;
    if (!resolveStream(req.url, baseUrl, info)) {
        respond(404, "Stream Not Found", req.cseq, std::string());
        return;
    }
    if (info->sdpPacketizer == nullptr) {
        respond(500, "Internal Server Error", req.cseq, std::string());
        return;
    }

    /* SDP 每流只生成一次并缓存：栈上临时 SdpBuilder 反复生成会在静态状态表
     * 中串入旧 track（地址复用，见 RtspServerImpl.h 的 sdpBuilder 注释），
     * 导致 SDP 出现重复 m= 段、客户端看到幽灵流。track URL 取首个 DESCRIBE
     * 的 baseUrl（同一路流的 URL 固定，可接受）。 */
    if (info->sdp.empty()) {
        const char *media = (info->codec == MediaCodec::kH264 || info->codec == MediaCodec::kH265)
                                ? "video"
                                : "audio";
        std::string trackUrl = baseUrl + "/track0";

        auto builder = std::make_unique<SdpBuilder>();
        builder->setOrigin("darkos", "127.0.0.1");
        builder->setSessionName(baseUrl.substr(baseUrl.rfind('/') + 1));
        SdpBuilder::Track &track =
            builder->addTrack(media, 96, stripPtPrefix(info->sdpPacketizer->rtpmapLine()));
        const char *fmtp = info->sdpPacketizer->fmtpLine();
        if (fmtp != nullptr)
            track.setFmtp(stripPtPrefix(fmtp));
        track.setControl(trackUrl);

        info->sdp = builder->build();
        info->sdpBuilder = std::move(builder);
    }

    respond(200, "OK", req.cseq, "Content-Base: " + baseUrl + "/\r\n", info->sdp,
            "application/sdp");
}

void RtspConnection::onSetup(const Request &req) {
    std::string baseUrl;
    const RtspStreamInfo *info = nullptr;
    if (!resolveStream(req.url, baseUrl, info)) {
        respond(404, "Stream Not Found", req.cseq, std::string());
        return;
    }
    auto it = req.headers.find("transport");
    if (it == req.headers.end()) {
        respond(400, "Bad Request", req.cseq, std::string());
        return;
    }
    TransportSpec spec;
    if (!parseTransport(it->second, spec)) {
        respond(461, "Unsupported Transport", req.cseq, std::string());
        return;
    }

    auto session = std::make_shared<RtspSession>(loop_, this, genSessionId(), info->source,
                                                 info->codec);
    std::string transportResp;
    if (spec.tcp) {
        session->setupTcp(spec.rtpChannel, spec.rtcpChannel);
        char buf[128];
        snprintf(buf, sizeof(buf), "RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08X",
                 spec.rtpChannel, spec.rtcpChannel, session->ssrc());
        transportResp = buf;
    } else {
        if (!spec.hasClientPorts) {
            respond(461, "Unsupported Transport", req.cseq, std::string());
            return;
        }
        /* 客户端地址取连接对端 IP，端口用 client_port 协商值 */
        struct sockaddr_in clientAddr = {};
        clientAddr.sin_family = AF_INET;
        socklen_t len = sizeof(clientAddr);
        if (getpeername(fd_, (struct sockaddr *)&clientAddr, &len) != 0) {
            respond(500, "Internal Server Error", req.cseq, std::string());
            return;
        }
        std::string err;
        if (!session->setupUdp(clientAddr, spec.clientRtpPort, spec.clientRtcpPort, err)) {
            respond(500, "Internal Server Error", req.cseq, std::string());
            return;
        }
        char buf[160];
        snprintf(buf, sizeof(buf), "RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u;ssrc=%08X",
                 spec.clientRtpPort, spec.clientRtcpPort, session->serverRtpPort(),
                 session->serverRtcpPort(), session->ssrc());
        transportResp = buf;
    }

    sessions_[session->id()] = session;
    LOGI(kTag, "%s: setup session %s (%s)", peerDesc_.c_str(), session->id().c_str(),
         spec.tcp ? "tcp" : "udp");
    /* timeout 值与超时巡检一致（会话保活语义见 readme） */
    char sessionHdr[48];
    snprintf(sessionHdr, sizeof(sessionHdr), ";timeout=%u",
             (unsigned)server_->options().sessionTimeoutS);
    respond(200, "OK", req.cseq,
            "Transport: " + transportResp + "\r\nSession: " + session->id() + sessionHdr +
                "\r\n");
}

void RtspConnection::onPlay(const Request &req) {
    std::string sessionId;
    auto it = req.headers.find("session");
    if (it != req.headers.end())
        sessionId = trim(it->second).substr(0, trim(it->second).find(';'));
    auto sit = sessions_.find(sessionId);
    if (sessionId.empty() || sit == sessions_.end()) {
        respond(454, "Session Not Found", req.cseq, std::string());
        return;
    }
    RtspSession *session = sit->second.get();

    if (!session->isPlaying()) {
        std::string baseUrl;
        const RtspStreamInfo *info = nullptr;
        if (!resolveStream(req.url, baseUrl, info)) {
            respond(404, "Stream Not Found", req.cseq, std::string());
            return;
        }
        /* 多客户端共享同一路流：引用计数 +1（Source fan-out，每个会话
         * 独立打包/独立 RTP 序号），PAUSE/TEARDOWN/断连时 -1 */
        server_->acquireSource(info->source);
        if (!session->play()) {
            server_->releaseSource(info->source);
            respond(500, "Internal Server Error", req.cseq, std::string());
            return;
        }
        playingSources_[sessionId] = info->source;

        /* RTP-Info（RFC 2326 §12.33）：seq 取发送端下一包序号（精确）；
         * rtptime 为 RTP 时钟当前值的近似——等 IDR 起播时首包随实际关键帧
         * 到达，误差不超过一个 GOP 周期 */
        char rtpInfo[512];
        snprintf(rtpInfo, sizeof(rtpInfo),
                 "Session: %s\r\nRTP-Info: url=%s/track0;seq=%u;rtptime=%u\r\n",
                 sessionId.c_str(), baseUrl.c_str(), session->nextRtpSeq(),
                 session->nextRtpTimestamp());
        respond(200, "OK", req.cseq, rtpInfo);
        return;
    }
    respond(200, "OK", req.cseq, "Session: " + sessionId + "\r\n");
}

void RtspConnection::onPause(const Request &req) {
    std::string sessionId;
    auto it = req.headers.find("session");
    if (it != req.headers.end())
        sessionId = trim(it->second).substr(0, trim(it->second).find(';'));
    auto sit = sessions_.find(sessionId);
    if (sessionId.empty() || sit == sessions_.end()) {
        respond(454, "Session Not Found", req.cseq, std::string());
        return;
    }
    sit->second->pause();
    auto pit = playingSources_.find(sessionId);
    if (pit != playingSources_.end()) {
        server_->releaseSource(pit->second);
        playingSources_.erase(pit);
    }
    respond(200, "OK", req.cseq, "Session: " + sessionId + "\r\n");
}

void RtspConnection::onTeardown(const Request &req) {
    std::string sessionId;
    auto it = req.headers.find("session");
    if (it != req.headers.end())
        sessionId = trim(it->second).substr(0, trim(it->second).find(';'));
    auto sit = sessions_.find(sessionId);
    if (sessionId.empty() || sit == sessions_.end()) {
        respond(454, "Session Not Found", req.cseq, std::string());
        return;
    }
    auto pit = playingSources_.find(sessionId);
    if (pit != playingSources_.end()) {
        server_->releaseSource(pit->second);
        playingSources_.erase(pit);
    }
    sessions_.erase(sit); /* 会话销毁：挂起的拉流回调经 weak_ptr 失效 */
    respond(200, "OK", req.cseq, "Session: " + sessionId + "\r\n");
}

/* ------------------------------ 参数/认证 ------------------------------ */

void RtspConnection::onGetOrSetParameter(const Request &req) {
    if (req.method == "GET_PARAMETER") {
        /* VLC 等客户端周期发 GET_PARAMETER 保活：回 200 空体避免误判掉线 */
        respond(200, "OK", req.cseq, std::string());
        return;
    }

    /* SET_PARAMETER（RFC 2326 §10.5）：本服务器没有可设置参数——空体（纯
     * 保活）回 200；出现任何参数说明客户端想设置不支持的能力，回 406 */
    if (req.body.find_first_not_of(" \t\r\n") == std::string::npos) {
        respond(200, "OK", req.cseq, std::string());
        return;
    }
    LOGW(kTag, "%s: SET_PARAMETER with unsupported body, reject 406", peerDesc_.c_str());
    respond(406, "Not Acceptable", req.cseq, std::string());
}

void RtspConnection::sendAuthChallenge(const Request &req) {
    const RtspServerOptions &opts = server_->options();
    pendingNonce_ = genNonce();
    std::string hdr = "WWW-Authenticate: Digest realm=\"" + opts.authRealm + "\", nonce=\"" +
                      pendingNonce_ + "\", algorithm=MD5, qop=\"auth\"\r\n";
    respond(401, "Unauthorized", req.cseq, hdr);
}

bool RtspConnection::checkAuthorization(const Request &req) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) {
        sendAuthChallenge(req);
        return false;
    }

    /* 只支持 Digest（挑战只下发 Digest，客户端发 Basic 也重新挑战） */
    std::string scheme = toLower(it->second.substr(0, it->second.find(' ')));
    if (scheme != "digest") {
        sendAuthChallenge(req);
        return false;
    }
    auto params = parseAuthParams(trim(it->second.substr(scheme.size())));

    const RtspServerOptions &opts = server_->options();
    const std::string &username = params["username"];
    const std::string &realm = params["realm"];
    const std::string &nonce = params["nonce"];
    const std::string &response = params["response"];
    /* RFC 2617：HA2 的 uri 取客户端 Authorization 头里的值（可能是绝对
     * URL，与请求行的写法未必一致），缺省退回请求行 URL */
    const std::string &uri = params.count("uri") != 0 ? params["uri"] : req.url;

    if (username.empty() || nonce.empty() || response.empty() || nonce != pendingNonce_) {
        sendAuthChallenge(req); /* 无凭证/nonce 不匹配（含重放旧挑战）：重新挑战 */
        return false;
    }

    /* RFC 2617 §3.2.2.1：qop 有无两条路径都支持（客户端选择） */
    std::string ha1 = md5Hex(username + ":" + realm + ":" + opts.authPasswd);
    std::string ha2 = md5Hex(req.method + ":" + uri);
    std::string expect;
    if (params.count("qop") != 0 && !params["qop"].empty())
        expect = md5Hex(ha1 + ":" + nonce + ":" + params["nc"] + ":" + params["cnonce"] +
                        ":" + params["qop"] + ":" + ha2);
    else
        expect = md5Hex(ha1 + ":" + nonce + ":" + ha2);

    if (username != opts.authUser || expect != response) {
        sendAuthChallenge(req);
        return false;
    }
    return true;
}

/* ------------------------------ 响应 ----------------------------------- */

void RtspConnection::respond(int code, const char *reason, const std::string &cseq,
                             const std::string &extraHeaders, const std::string &body,
                             const char *contentType) {
    std::string r = "RTSP/1.0 " + std::to_string(code) + " " + reason + "\r\n";
    r += "CSeq: " + (cseq.empty() ? "0" : cseq) + "\r\n";
    r += "Server: DarkOS-RTSP/1.0\r\n";
    r += extraHeaders;
    if (contentType != nullptr)
        r += "Content-Type: " + std::string(contentType) + "\r\n";
    if (!body.empty())
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    r += body;
    sendText(r);
}

} // namespace darkos
