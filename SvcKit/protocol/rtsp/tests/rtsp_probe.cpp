/*
 * rtsp_probe：RtspServer 信令自测（本机回环，不起真流）
 *
 * 覆盖：
 *   OPTIONS / DESCRIBE(SDP) / SETUP(UDP+TCP interleaved) / PLAY / PAUSE /
 *   TEARDOWN 的响应格式；多客户端 fan-out（同源第二客户端 PLAY 正常出流、
 *   TCP 双客户端各自收到 interleaved RTP）；慢客户端背压（不读数据的客户端
 *   在发送积压超限后被服务器断开）；未知方法 405；GET_PARAMETER 保活 200；
 *   SET_PARAMETER 空体 200 / 带参 406；ANNOUNCE 501；MD5（RFC 1321 向量）；
 *   Digest 认证（401 挑战/错凭证拒绝/对凭证放行，qop 与无 qop 两路径）；
 *   会话空闲超时断链与保活刷新。
 *
 * 结构：EventLoop + RtspServer 跑在独立线程，主线程用阻塞 socket 发请求断言。
 * 链接真 stream 层实现（打包器/RTCP/SDP），interleaved 数据与背压用例
 * 走真实打包路径（早期 stream 并行开发时用弱符号桩兜底，实现入库后移除）。
 *
 * 用法：./rtsp_probe（退出码 0 = 全部通过）
 */

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#include "base/EventLoop.h"
#include "base/Hash.h"
#include "base/Log.h"
#include "base/Thread.h"
#include "base/TimeUtil.h"
#include "rtsp/RtspServer.h"
#include "stream/RtcpSession.h"
#include "stream/RtpPacketizer.h"
#include "stream/Sdp.h"
#include "stream/SourceSink.h"

using namespace darkos;

static constexpr const char *kTag = "rtsp_probe";
static constexpr uint16_t kPort = 18554;        /* 主服务器：无认证、默认超时 */
static constexpr uint16_t kAuthPort = 18555;    /* 认证服务器：Digest 开启 */
static constexpr uint16_t kTimeoutPort = 18556; /* 超时服务器：2s 空闲断链 */
static constexpr uint16_t kBpPort = 18557;      /* 背压服务器：1KB 测试水位 */
static const char *kAuthUser = "admin";
static const char *kAuthPass = "secret";

static int g_failures = 0;
#define CHECK(cond, name)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("[PASS] %s\n", name);                                                           \
        } else {                                                                                   \
            printf("[FAIL] %s\n", name);                                                           \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* ------------------------------ 假帧源 ---------------------------------- */

/* 合法 Annex-B 帧（SPS + PPS + IDR，内容随意但 NAL 头合法） */
static const uint8_t kFakeFrame[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F, 0x8C, 0x8D, 0x40, 0x50, 0x00, 0x00, 0x00, 0x01,
    0x68, 0xCE, 0x3C, 0x80, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x21, 0x7F, 0x5D, 0xA0,
};

/* 拉模型假源：getNextFrame 挂起（多消费者 fan-out），定时器到点广播一帧。
 * idrPadBytes > 0 时追加一段大 IDR 载荷——背压用例需要大帧快速填满积压 */
class FakeSource : public Source {
  public:
    explicit FakeSource(EventLoop *loop, uint64_t intervalMs = 40, size_t idrPadBytes = 0)
        : loop_(loop) {
        frame_.assign(kFakeFrame, kFakeFrame + sizeof(kFakeFrame));
        if (idrPadBytes > 0) {
            const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
            frame_.insert(frame_.end(), startCode, startCode + sizeof(startCode));
            frame_.push_back(0x65); /* IDR NAL 头，载荷内容随意 */
            /* 不能补 0：Annex-B 规定 NAL 尾部连续 0 是 trailing_zero_8bits，
             * splitNals() 会将其裁掉，背压用例就仍然只是几十字节的小帧。 */
            frame_.insert(frame_.end(), idrPadBytes, 0x55);
        }
        uint64_t ns = intervalMs * 1000 * 1000;
        loop_->scheduleEvery(ns, ns, [this] { tick(); });
    }
    void getNextFrame(FrameCallback cb) override {
        pending_.push_back(std::move(cb));
    }
    SubscriptionId subscribe(FrameCallback) override {
        return -1;
    }
    void unsubscribe(SubscriptionId) override {}

  private:
    void tick() {
        if (pending_.empty())
            return;
        MediaPacket pkt;
        pkt.data = frame_.data();
        pkt.size = frame_.size();
        pkt.ptsNs = monoNowNs();
        pkt.keyframe = true;
        pkt.codec = MediaCodec::kH264;
        std::vector<FrameCallback> cbs = std::move(pending_);
        pending_.clear();
        for (auto &cb : cbs)
            cb(pkt);
    }

    EventLoop *loop_;
    std::vector<uint8_t> frame_;
    std::vector<FrameCallback> pending_;
};

/* --------------------------- 阻塞式测试客户端 ---------------------------- */

static int connectToServer(uint16_t port = kPort) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOGE(kTag, "connect failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static bool sendRequest(int fd, const std::string &req) {
    size_t off = 0;
    while (off < req.size()) {
        ssize_t n = send(fd, req.data() + off, req.size() - off, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

/* 读一个完整响应：头部到 \r\n\r\n，再按 Content-Length 补读体 */
static std::string readResponse(int fd) {
    std::string buf;
    char tmp[4096];
    for (;;) {
        auto hdrEnd = buf.find("\r\n\r\n");
        if (hdrEnd != std::string::npos) {
            size_t contentLength = 0;
            auto cl = buf.find("Content-Length:");
            if (cl != std::string::npos && cl < hdrEnd)
                contentLength = (size_t)strtoul(buf.c_str() + cl + 15, nullptr, 10);
            if (buf.size() >= hdrEnd + 4 + contentLength)
                return buf;
        }
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            printf("[FAIL] read response: %s\n", n == 0 ? "connection closed" : strerror(errno));
            g_failures++;
            return buf;
        }
        buf.append(tmp, (size_t)n);
    }
}

static std::string sessionIdOf(const std::string &resp) {
    auto p = resp.find("Session:");
    if (p == std::string::npos)
        return std::string();
    p += 8;
    while (p < resp.size() && resp[p] == ' ')
        p++;
    size_t e = p;
    while (e < resp.size() && (isxdigit((unsigned char)resp[e]) != 0))
        e++;
    return resp.substr(p, e - p);
}

static std::string url() {
    return "rtsp://127.0.0.1:" + std::to_string(kPort) + "/live";
}

static std::string urlOf(uint16_t port) {
    return "rtsp://127.0.0.1:" + std::to_string(port) + "/live";
}

/* 从响应里提取 key 后双引号中的值（nonce 等） */
static std::string extractQuoted(const std::string &s, const std::string &key) {
    auto p = s.find(key + "\"");
    if (p == std::string::npos)
        return std::string();
    p += key.size() + 1;
    auto e = s.find('"', p);
    return e == std::string::npos ? std::string() : s.substr(p, e - p);
}

/* RFC 2617 §3.2.2.1 应答值（qop 为空走无 qop 路径） */
static std::string digestResponse(const std::string &user, const std::string &realm,
                                  const std::string &passwd, const std::string &method,
                                  const std::string &uri, const std::string &nonce,
                                  const std::string &nc, const std::string &cnonce,
                                  const std::string &qop) {
    std::string ha1 = md5Hex(user + ":" + realm + ":" + passwd);
    std::string ha2 = md5Hex(method + ":" + uri);
    if (!qop.empty())
        return md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
    return md5Hex(ha1 + ":" + nonce + ":" + ha2);
}

/* 拼一条 Authorization 头（nc/cnonce 固定测试值） */
static std::string authHeader(const std::string &user, const std::string &passwd,
                              const std::string &method, const std::string &uri,
                              const std::string &nonce, const std::string &qop) {
    std::string h = "Authorization: Digest username=\"" + user + "\", realm=\"darkos\", nonce=\"" +
                    nonce + "\", uri=\"" + uri + "\"";
    if (!qop.empty())
        h += ", response=\"" +
             digestResponse(user, "darkos", passwd, method, uri, nonce, "00000001", "0a4f010b",
                            qop) +
             "\", algorithm=MD5, cnonce=\"0a4f010b\", nc=00000001, qop=" + qop;
    else
        h += ", response=\"" +
             digestResponse(user, "darkos", passwd, method, uri, nonce, "", "", "") +
             "\", algorithm=MD5";
    return h + "\r\n";
}

int main() {
    /* ---- MD5（RFC 1321 A.5 官方测试向量，认证的地基） ---- */
    struct Md5Vec {
        const char *in;
        const char *want;
    };
    const Md5Vec md5Vecs[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"a", "0cc175b9c0f1b6a831c399e269772661"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
        {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
    };
    for (const Md5Vec &v : md5Vecs) {
        std::string name = std::string("md5(\"") + v.in + "\") RFC 1321 vector";
        CHECK(md5Hex(v.in) == v.want, name.c_str());
    }

    EventLoop *loop = EventLoop::create();
    RtspServer *server = RtspServer::create(loop, kPort);
    FakeSource source(loop);
    if (!server->addStream("live", &source, MediaCodec::kH264)) {
        LOGE(kTag, "addStream failed");
        return 1;
    }
    CHECK(server->start() == 0, "main server listens");

    /* 认证服务器：Digest 开启（realm 默认 darkos） */
    RtspServerOptions authOpts;
    authOpts.port = kAuthPort;
    authOpts.authUser = kAuthUser;
    authOpts.authPasswd = kAuthPass;
    RtspServer *authServer = RtspServer::create(loop, authOpts);
    FakeSource authSource(loop);
    authServer->addStream("live", &authSource, MediaCodec::kH264);
    CHECK(authServer->start() == 0, "auth server listens");

    /* 超时服务器：2s 空闲断链（巡检 1s 粒度） */
    RtspServerOptions timeoutOpts;
    timeoutOpts.port = kTimeoutPort;
    timeoutOpts.sessionTimeoutS = 2;
    RtspServer *timeoutServer = RtspServer::create(loop, timeoutOpts);
    FakeSource timeoutSource(loop);
    timeoutServer->addStream("live", &timeoutSource, MediaCodec::kH264);
    CHECK(timeoutServer->start() == 0, "timeout server listens");

    /* 背压服务器：水位小于单个 RTP 帧，且关闭空闲超时，确保断链只能来自背压。 */
    RtspServerOptions bpOpts;
    bpOpts.port = kBpPort;
    bpOpts.sessionTimeoutS = 0;
    bpOpts.txMaxPendingBytes = 1024;
    RtspServer *bpServer = RtspServer::create(loop, bpOpts);
    FakeSource bpSource(loop, 5 /* ms */, 32 * 1024);
    bpServer->addStream("live", &bpSource, MediaCodec::kH264);
    CHECK(bpServer->start() == 0, "backpressure server listens");

    Thread loopThread("probe.loop", [loop] { loop->run(); });
    usleep(200 * 1000); /* 等监听就绪 */

    const std::string base = url();

    /* ---- 连接 1：UDP 全流程 ---- */
    int c1 = connectToServer();
    CHECK(c1 >= 0, "connect #1");

    CHECK(sendRequest(c1, "OPTIONS " + base + " RTSP/1.0\r\nCSeq: 1\r\n\r\n"), "send OPTIONS");
    std::string r = readResponse(c1);
    CHECK(r.find("200 OK") != std::string::npos && r.find("Public:") != std::string::npos &&
              r.find("DESCRIBE") != std::string::npos,
          "OPTIONS 200 + Public");

    CHECK(sendRequest(c1, "DESCRIBE " + base +
                              " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n"),
          "send DESCRIBE");
    r = readResponse(c1);
    CHECK(r.find("200 OK") != std::string::npos &&
              r.find("Content-Type: application/sdp") != std::string::npos &&
              r.find("v=0") != std::string::npos,
          "DESCRIBE 200 + SDP");

    CHECK(sendRequest(c1, "SETUP " + base +
                              "/track0 RTSP/1.0\r\nCSeq: 3\r\n"
                              "Transport: RTP/AVP;unicast;client_port=50000-50001\r\n\r\n"),
          "send SETUP(udp)");
    r = readResponse(c1);
    std::string s1 = sessionIdOf(r);
    CHECK(r.find("200 OK") != std::string::npos && r.find("server_port=") != std::string::npos &&
              r.find("client_port=50000-50001") != std::string::npos && !s1.empty(),
          "SETUP(udp) 200 + server_port + Session");

    CHECK(sendRequest(c1, "PLAY " + base + " RTSP/1.0\r\nCSeq: 4\r\nSession: " + s1 + "\r\n\r\n"),
          "send PLAY");
    r = readResponse(c1);
    {
        auto rtpp = r.find("rtptime=");
        uint32_t rtptime =
            rtpp == std::string::npos ? 0 : (uint32_t)strtoul(r.c_str() + rtpp + 8, nullptr, 10);
        CHECK(r.find("200 OK") != std::string::npos && r.find("RTP-Info:") != std::string::npos &&
                  r.find(";seq=") != std::string::npos && rtptime != 0,
              "PLAY 200 + RTP-Info (real seq/rtptime)");
    }

    /* ---- 连接 2：同源并发第二个 PLAY（多客户端 fan-out） ---- */
    int c2 = connectToServer();
    CHECK(c2 >= 0, "connect #2");
    sendRequest(c2, "SETUP " + base +
                        "/track0 RTSP/1.0\r\nCSeq: 1\r\n"
                        "Transport: RTP/AVP;unicast;client_port=50002-50003\r\n\r\n");
    r = readResponse(c2);
    std::string s2 = sessionIdOf(r);
    CHECK(r.find("200 OK") != std::string::npos && !s2.empty(), "SETUP #2 200");
    sendRequest(c2, "PLAY " + base + " RTSP/1.0\r\nCSeq: 2\r\nSession: " + s2 + "\r\n\r\n");
    r = readResponse(c2);
    CHECK(r.find("200 OK") != std::string::npos && r.find("RTP-Info:") != std::string::npos,
          "PLAY #2 200 while #1 streaming (fan-out)");

    /* ---- 连接 1：PAUSE → 只停本会话（#2 不受影响）；TEARDOWN → 释放引用 ---- */
    sendRequest(c1, "PAUSE " + base + " RTSP/1.0\r\nCSeq: 5\r\nSession: " + s1 + "\r\n\r\n");
    r = readResponse(c1);
    CHECK(r.find("200 OK") != std::string::npos, "PAUSE 200");

    sendRequest(c1, "PLAY " + base + " RTSP/1.0\r\nCSeq: 6\r\nSession: " + s1 + "\r\n\r\n");
    r = readResponse(c1);
    CHECK(r.find("200 OK") != std::string::npos, "PLAY after PAUSE 200");

    sendRequest(c1, "TEARDOWN " + base + " RTSP/1.0\r\nCSeq: 7\r\nSession: " + s1 + "\r\n\r\n");
    r = readResponse(c1);
    CHECK(r.find("200 OK") != std::string::npos, "TEARDOWN 200");

    sendRequest(c2, "PLAY " + base + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + s2 + "\r\n\r\n");
    r = readResponse(c2);
    CHECK(r.find("200 OK") != std::string::npos, "PLAY #2 idempotent while playing");

    sendRequest(c2, "PAUSE " + base + " RTSP/1.0\r\nCSeq: 4\r\nSession: " + s2 + "\r\n\r\n");
    r = readResponse(c2);
    CHECK(r.find("200 OK") != std::string::npos, "PAUSE #2 200");

    /* ---- 连接 3/4：TCP interleaved 双客户端并发，各自收到 RTP 数据 ---- */
    int c3 = connectToServer();
    CHECK(c3 >= 0, "connect #3");
    sendRequest(c3, "SETUP " + base +
                        "/track0 RTSP/1.0\r\nCSeq: 1\r\n"
                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    r = readResponse(c3);
    std::string s3 = sessionIdOf(r);
    CHECK(r.find("200 OK") != std::string::npos && r.find("interleaved=0-1") != std::string::npos &&
              !s3.empty(),
          "SETUP(tcp) 200 + interleaved echo");
    sendRequest(c3, "PLAY " + base + " RTSP/1.0\r\nCSeq: 2\r\nSession: " + s3 + "\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("200 OK") != std::string::npos, "PLAY(tcp) 200");

    int c4 = connectToServer();
    CHECK(c4 >= 0, "connect #4");
    sendRequest(c4, "SETUP " + base +
                        "/track0 RTSP/1.0\r\nCSeq: 1\r\n"
                        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    r = readResponse(c4);
    std::string s4 = sessionIdOf(r);
    CHECK(r.find("200 OK") != std::string::npos && !s4.empty(), "SETUP #4(tcp) 200");
    sendRequest(c4, "PLAY " + base + " RTSP/1.0\r\nCSeq: 2\r\nSession: " + s4 + "\r\n\r\n");
    r = readResponse(c4);
    CHECK(r.find("200 OK") != std::string::npos, "PLAY #4(tcp) 200 (fan-out)");

    /* 双客户端各自收到 interleaved 帧（'$' + channel + 长度 + RTP）。
     * readResponse 可能吃掉半帧，这里不要求首字节是 '$'，只要求缓冲内
     * 出现帧定界符 '$'（后续帧边界必然落进 512 字节窗口） */
    {
        char tmp[512];
        ssize_t n3 = recv(c3, tmp, sizeof(tmp), 0);
        CHECK(n3 > 0 && memchr(tmp, '$', (size_t)n3) != nullptr,
              "client #3 receives interleaved RTP");
        ssize_t n4 = recv(c4, tmp, sizeof(tmp), 0);
        CHECK(n4 > 0 && memchr(tmp, '$', (size_t)n4) != nullptr,
              "client #4 receives interleaved RTP (fan-out)");
    }

    sendRequest(c3, "TEARDOWN " + base + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + s3 + "\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("200 OK") != std::string::npos, "TEARDOWN(tcp) 200");
    sendRequest(c4, "TEARDOWN " + base + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + s4 + "\r\n\r\n");
    r = readResponse(c4);
    CHECK(r.find("200 OK") != std::string::npos, "TEARDOWN #4 200");

    /* ---- 杂项：未知方法 405；GET/SET_PARAMETER；ANNOUNCE 501；454 ---- */
    sendRequest(c3, "FOO " + base + " RTSP/1.0\r\nCSeq: 4\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("405") != std::string::npos, "unknown method -> 405");

    sendRequest(c3, "GET_PARAMETER " + base + " RTSP/1.0\r\nCSeq: 5\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("200 OK") != std::string::npos, "GET_PARAMETER 200 (keepalive)");

    sendRequest(c3, "SET_PARAMETER " + base + " RTSP/1.0\r\nCSeq: 6\r\nContent-Length: 0\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("200 OK") != std::string::npos, "SET_PARAMETER empty body 200 (keepalive)");

    sendRequest(c3, "SET_PARAMETER " + base +
                        " RTSP/1.0\r\nCSeq: 7\r\nContent-Type: text/parameters\r\n"
                        "Content-Length: 17\r\n\r\nbarparm: whatever");
    r = readResponse(c3);
    CHECK(r.find("406") != std::string::npos, "SET_PARAMETER unknown param -> 406");

    sendRequest(c3, "ANNOUNCE " + base + " RTSP/1.0\r\nCSeq: 8\r\nContent-Length: 0\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("501") != std::string::npos, "ANNOUNCE -> 501 (source-only device)");

    sendRequest(c3, "PLAY " + base + " RTSP/1.0\r\nCSeq: 9\r\nSession: deadbeef\r\n\r\n");
    r = readResponse(c3);
    CHECK(r.find("454") != std::string::npos, "PLAY with bad session -> 454");

    sendRequest(c2, "TEARDOWN " + base + " RTSP/1.0\r\nCSeq: 5\r\nSession: " + s2 + "\r\n\r\n");
    r = readResponse(c2);
    CHECK(r.find("200 OK") != std::string::npos, "TEARDOWN #2 200");

    close(c1);
    close(c2);
    close(c3);
    close(c4);

    /* ---- 认证服务器：Digest 挑战/校验 ---- */
    {
        const std::string authUrl = urlOf(kAuthPort);
        int ca = connectToServer(kAuthPort);
        CHECK(ca >= 0, "connect to auth server");

        sendRequest(ca, "OPTIONS " + authUrl + " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
        r = readResponse(ca);
        CHECK(r.find("200 OK") != std::string::npos, "auth: OPTIONS open without credentials");

        sendRequest(ca, "DESCRIBE " + authUrl +
                            " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
        r = readResponse(ca);
        CHECK(r.find("401") != std::string::npos &&
                  r.find("WWW-Authenticate: Digest") != std::string::npos &&
                  !extractQuoted(r, "nonce=").empty(),
              "auth: DESCRIBE without credentials -> 401 + Digest challenge");

        /* 错口令：401（且换发新 nonce） */
        std::string nonce = extractQuoted(r, "nonce=");
        sendRequest(ca, "DESCRIBE " + authUrl +
                            " RTSP/1.0\r\nCSeq: 3\r\nAccept: application/sdp\r\n" +
                            authHeader(kAuthUser, "wrongpass", "DESCRIBE", authUrl, nonce, "auth") +
                            "\r\n");
        r = readResponse(ca);
        CHECK(r.find("401") != std::string::npos, "auth: wrong password -> 401");

        /* 对口令（qop=auth 路径）：200 + SDP。注意用最新一次挑战的 nonce */
        nonce = extractQuoted(r, "nonce=");
        sendRequest(
            ca, "DESCRIBE " + authUrl + " RTSP/1.0\r\nCSeq: 4\r\nAccept: application/sdp\r\n" +
                    authHeader(kAuthUser, kAuthPass, "DESCRIBE", authUrl, nonce, "auth") + "\r\n");
        r = readResponse(ca);
        CHECK(r.find("200 OK") != std::string::npos && r.find("v=0") != std::string::npos,
              "auth: correct digest (qop=auth) -> 200 + SDP");

        /* 认证一次后同连接放行 */
        sendRequest(ca, "DESCRIBE " + authUrl +
                            " RTSP/1.0\r\nCSeq: 5\r\nAccept: application/sdp\r\n\r\n");
        r = readResponse(ca);
        CHECK(r.find("200 OK") != std::string::npos,
              "auth: subsequent request passes on same connection");
        close(ca);

        /* 新连接走无 qop 路径（老客户端兼容） */
        int cb = connectToServer(kAuthPort);
        CHECK(cb >= 0, "connect to auth server #2");
        sendRequest(cb, "DESCRIBE " + authUrl +
                            " RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n");
        r = readResponse(cb);
        nonce = extractQuoted(r, "nonce=");
        CHECK(r.find("401") != std::string::npos, "auth #2: challenged");
        sendRequest(cb,
                    "DESCRIBE " + authUrl + " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n" +
                        authHeader(kAuthUser, kAuthPass, "DESCRIBE", authUrl, nonce, "") + "\r\n");
        r = readResponse(cb);
        CHECK(r.find("200 OK") != std::string::npos, "auth: correct digest (no qop) -> 200");
        close(cb);
    }

    /* ---- 超时服务器：保活刷新 + 空闲断链 ---- */
    {
        const std::string tUrl = urlOf(kTimeoutPort);
        int ct = connectToServer(kTimeoutPort);
        CHECK(ct >= 0, "connect to timeout server");

        sendRequest(ct, "OPTIONS " + tUrl + " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
        r = readResponse(ct);
        CHECK(r.find("200 OK") != std::string::npos, "timeout: OPTIONS 200");

        usleep(1500 * 1000); /* 1.5s < 2s：保活刷新 */
        sendRequest(ct, "OPTIONS " + tUrl + " RTSP/1.0\r\nCSeq: 2\r\n\r\n");
        r = readResponse(ct);
        CHECK(r.find("200 OK") != std::string::npos, "timeout: keepalive refreshes idle clock");

        usleep(4000 * 1000); /* 4s > 2s 无活动：服务端应断链 */
        char tmp[64];
        ssize_t n = recv(ct, tmp, sizeof(tmp), 0);
        CHECK(n == 0, "timeout: idle over limit -> server disconnects");
        close(ct);
    }

    /* ---- 背压服务器：PLAY 后不读数据，积压超水位被服务器断开 ---- */
    {
        const std::string bpUrl = urlOf(kBpPort);
        int cp = connectToServer(kBpPort);
        CHECK(cp >= 0, "backpressure: connect");
        int smallRcv = 4096; /* 收小接收缓冲，缩短内核缓冲填满时间 */
        setsockopt(cp, SOL_SOCKET, SO_RCVBUF, &smallRcv, sizeof(smallRcv));

        sendRequest(cp, "SETUP " + bpUrl +
                            "/track0 RTSP/1.0\r\nCSeq: 1\r\n"
                            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
        r = readResponse(cp);
        std::string sp = sessionIdOf(r);
        CHECK(r.find("200 OK") != std::string::npos && !sp.empty(), "backpressure: SETUP(tcp) 200");
        sendRequest(cp, "PLAY " + bpUrl + " RTSP/1.0\r\nCSeq: 2\r\nSession: " + sp + "\r\n\r\n");
        r = readResponse(cp);
        CHECK(r.find("200 OK") != std::string::npos, "backpressure: PLAY 200");

        /* 首个约 1400B RTP interleaved 帧超过 1KB 测试水位，必须立即断开；
         * sessionTimeoutS=0 排除了空闲超时造成假阳性。 */
        usleep(100 * 1000);
        char tmp[512];
        ssize_t n;
        do {
            n = recv(cp, tmp, sizeof(tmp), MSG_DONTWAIT);
        } while (n > 0);
        CHECK(n == 0, "backpressure: client disconnected when tx high-water is exceeded");
        close(cp);
    }

    loop->post([loop] { loop->quit(); });
    loopThread.join();
    delete server;
    delete authServer;
    delete timeoutServer;
    delete bpServer;
    delete loop;

    printf(g_failures == 0 ? "rtsp_probe: ALL PASS\n" : "rtsp_probe: %d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
