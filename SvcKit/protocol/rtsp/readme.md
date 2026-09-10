# 通用流传输协议 RTSP

自研 RTSP 服务器。**已实现**（验证：tests/rtsp_probe 信令自测 45 项 +
tests/live_probe 端到端出流；live_probe 为 host_x86 合成相机 → x264 →
ffmpeg/ffprobe 拉流，UDP 与 TCP interleaved 均验证）。

## 标准基线

| 方面 | 标准 | 说明 |
|---|---|---|
| 信令 | **RFC 2326**（RTSP 1.0） | 线上协议保持 1.0：VLC/ffmpeg/live555/GStreamer/ONVIF 设备实际说的都是 1.0；RTSP 2.0（RFC 7826，2016）市场占有近乎为零且需显式版本协商，升级即失去互操作性 |
| 勘误吸收 | RFC 7826 选择性采纳 | 只取不改变版本号的修正：会话超时语义（timeout 按"最后一次请求或 RTCP"计）、RTP-Info 填真实值 |
| 认证 | RFC 2617（HTTP Digest） | MD5 + qop="auth"（无 qop 路径兼容老客户端），默认关闭（`RtspServerOptions` 配账号口令启用） |
| 媒体 | RFC 3550（RTP/RTCP）、RFC 6184（H.264 FU-A） | 打包器在 SvcKit/stream |

MD5 为 `SvcKit/base/include/base/Hash.h` 的自研 RFC 1321 实现（rtsp_probe
用官方测试向量验证）——固件暂不为此引入 OpenSSL/mbedTLS；将来加密场景
（DTLS/SRTP）引入底库时整体替换。

## 能力清单

- 方法：OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / TEARDOWN /
  GET_PARAMETER / SET_PARAMETER
  - GET_PARAMETER：保活语义，200 空体
  - SET_PARAMETER：空体 = 保活 200；本服务器无可设置参数，带参回 406
  - ANNOUNCE / RECORD：**501**（IPC 摄像机是纯源，无推流接入场景）
- 传输：RTP/AVP UDP 单播 + RTP over TCP interleaved（组播不做）
- **等 IDR 起播**：PLAY（含 PAUSE 后重新 PLAY）丢弃非关键帧，从首个
  IDR 开始发送——GOP 中途接入不花屏；代价是最多等一个 GOP 周期
- RTP-Info：seq 取发送端下一包序号（精确），rtptime 为 RTP 时钟当前值
  近似（等 IDR 时首包随实际关键帧到达，误差 ≤ 一个 GOP）
- 会话空闲超时：请求与客户端 RTCP（UDP RR / TCP interleaved）刷新活动
  时间，超时（默认 60s，`RtspServerOptions::sessionTimeoutS` 可配）断链；
  SETUP 响应的 `timeout=` 与实际一致
- 认证：Digest 挑战只对非 OPTIONS 方法下发（OPTIONS 保持开放供能力
  探测）；同一连接认证一次即放行；支持 qop=auth 与无 qop 两路径

## 剩余限制

- 单轨（视频）；多客户端共享同一路流（Source fan-out：一次编码，
  各会话独立打包/独立 RTP 序号）；TCP 发送积压超过
  `RtspServerOptions::txMaxPendingBytes`（默认 2MB）的慢客户端被断开
- nonce 不做计数防重放（每次挑战换新 nonce，重放旧 nonce 会被拒绝）；
  加密底库引入后可升级 nonce-count 校验
- Digest 仅 MD5 算法（RFC 2617 范围）；MD5-SESS/SHA-256（RFC 7616）不做

参考：live555（RTSPServer.cpp / RTSPCommon.cpp）仅作阅读对照，不链接。

## 运行时对象图

```bash
              RTSP 客户端（VLC / ffmpeg / ffprobe）
                 |
                 |  一条 TCP：RTSP 信令文本 + '$' 交织 RTP/RTCP
                 v
+-------------------------------------------------------------------+
|                           RtspServerImpl                          |
|                （监听 8554 / 流注册表 / 播放引用计数表）                   |
|                                                                   |
| +---------------------------------------------------------------+ |
| |                        RtspConnection                         | |
| |               （解析 / 分发 / 认证 / 超时）                     | |
| |                                                               | |
| |  +---------------------------------------------------------+  | |
| |  |                     RtspSession                         |  | |
| |  |            （传输 / 拉流闭环 / 等IDR）                    |  | |
| |  |                                                         |  | |
| |  |   +---------------+ +----------------+ +--------------+  |  | |
| |  |   | RtspRtpSender | | RtpPacketizer | | RtcpSession |  |  | |
| |  |   +---------------+ +----------------+ +--------------+  |  | |
| |  |                                                         |  | |
| |  +---------------------------------------------------------+  | |
| +---------------------------------------------------------------+ |
|                                                                   |
+-------------------------------------------------------------------+
              ^
              |  getNextFrame（拉模型：先欠单，帧到才交货）
       Source（相机+编码，在 Services 层，不在本目录）
```

## 运行时对象图

```bash
      +------------------------------+
      |  Source（MediaService）      |
      |  相机+编码 → 出帧（推拉转换） |
      +------------------------------+
                     |
                     | (1) getNextFrame(cb)：先挂起等帧（拉模型）
                     v
      +------------------------------+
      |  RtspSession：拉流闭环核心   |
      |  门控：PAUSE丢 / 等IDR跳过   |
      +------------------------------+
                     |
                     | (2) packetize(pkt)
                     v
      +------------------------------+
      |  RtpPacketizer（stream 层）  |
      |  AnnexB拆NAL/补SPS-PPS/FU-A  |
      +------------------------------+
                     |
                     | (3) sendRtp(payload, ts, marker)
                     v
      +------------------------------+
      |  RtspRtpSender               |
      |  12字节RTP头：seq/ssrc/ts    |
      +------------------------------+
                   / \
                  /   \
             UDP /     \ TCP
                v       v
           sendmsg   '$'交织帧 → Connection::sendBytes
                |        |
                v        v
        客户端 client_port   同一条 RTSP TCP 连接

  (4) 发完一帧 loop_->post(pullNext) → 回到 (1) 要下一帧，闭环
      （post 而非递归：同步 Source 立即回帧会无限递归爆栈）
```




可以，下面用几张纯 ASCII 图把 RTSP 这层拆开讲。看懂的关键是抓住三个对象（Server / Connection / Session）和一条闭环（拉流）。

**图 1：分层——rtsp 代码在整条链路的哪个位置**

```
        外部宿主 / live_probe（应用接线或独立验证程序）
         │  RtspServer::create(loop, opts)
         │  addStream("live", source, MediaCodec::kH264)
         │  start()
         ▼
 ── SvcKit/Protocol/rtsp ── 信令层（RTSP 文本协议 + 出流编排）
     RtspServer(Impl)   监听 socket、流注册表、播放引用计数表
     RtspConnection     一条 TCP 连接：请求解析/方法分发/认证/超时
     RtspSession        一次播放会话：传输协商 + 拉流闭环（等 IDR 在这）
     RtspRtpSender      RTP 头组装 + UDP/TCP interleaved 双出口
         │ 只依赖 ↓（不碰帧源，不感知相机）
 ── SvcKit/stream ── 媒体层（协议无关，给任何协议复用）
     Source/Sink/MediaPacket   拉模型帧源抽象
     RtpPacketizer(+H264)      FU-A 打包、SPS/PPS 补齐
     RtcpSession / Sdp / AnnexB
         │ 只依赖 ↓
 ── SvcKit/base ── 地基
     EventLoop（单线程 reactor）/ Log / BufferPool / Hash(MD5)
```

**图 2：运行时对象图——谁持有谁（一对一读懂 4 个类）**

```
 RtspServer（对外门面，实现在 RtspServerImpl）
 │
 ├─ streams_        "live" ──► RtspStreamInfo{ source, codec, 缓存的 SDP }
 │
 ├─ connections_    每个 accept 进来的 TCP 连接一个 RtspConnection
 │
 │      RtspConnection（一条 TCP 连接）
 │      │  增量解析请求；"$"开头的是交织帧，分流给会话的 RTCP
 │      │  [M1.5] Digest 认证门 + 空闲超时巡检住在这里
 │      │
 │      └─ sessions_   sessionId ──► RtspSession（一次播放）
 │                                 │  SETUP 时建传输，此时创建：
 │                                 │    sender_     RtspRtpSender（出口）
 │                                 │  PLAY 时懒创建：
 │                                 │    packetizer_ stream 层打包器
 │                                 │    rtcp_       SR/RR 统计
 │                                 │  [M1.5] awaitIdr_ 等 IDR 门控在这
 │
 └─ sourceOwners_   Source* ──► 播放会话数   引用计数：多客户端 fan-out
```

**图 3：`handleRequest()` 是总机——按方法名转接（读代码从这进）**

```
 RtspConnection::handleRequest()
   ├─ 认证门（启用时；OPTIONS 放行，其余先 401 挑战/校验）
   ├─ OPTIONS            → onOptions           回 Public 方法清单
   ├─ DESCRIBE           → onDescribe          查流 + 生成/取缓存 SDP
   ├─ SETUP              → onSetup             建 RtspSession（UDP 或 TCP 传输）
   ├─ PLAY               → onPlay              占源 + play() + 回真实 RTP-Info
   ├─ PAUSE              → onPause             停拉流（会话保留）
   ├─ TEARDOWN           → onTeardown          还源 + 销毁会话
   ├─ GET/SET_PARAMETER  → onGetOrSetParameter 保活 200 / 带参 406
   └─ ANNOUNCE/RECORD    → 501（摄像机是纯源，无收流场景）
```

**图 4：信令时序——一次完整拉流**

```
 客户端                       服务器内部发生的事
 ─────────────────────────────────────────────────────────────
 OPTIONS   ──►  onOptions                       ◄── 200 Public: ...
 DESCRIBE  ──►  onDescribe（查 streams_）       ◄── 200 + SDP
 SETUP     ──►  onSetup：new RtspSession，      ◄── 200 Session: <id>;timeout=60
                按 Transport 头 setupUdp/Tcp         Transport: server_port=... 或 interleaved=0-1
 PLAY      ──►  onPlay：acquireSource 占源，    ◄── 200 RTP-Info: url;seq;rtptime
                play() 置 awaitIdr_=true
             [等首个关键帧到达，才开始发送]
                ◄═══ RTP 媒体流（见图 5）═══
 PAUSE     ──►  onPause：拉流循环停             ◄── 200
 TEARDOWN  ──►  onTeardown：releaseSource，     ◄── 200
                sessions_.erase → 会话析构
```

**图 5：拉模型闭环——PLAY 之后一帧的旅程（全层最核心的一段代码，`RtspSession::pullNext`）**

```
 ① pullNext()：向 Source 挂 getNextFrame(cb) —— 先欠单，帧到才交货
        │
        ▼
 ② 帧到达 → 回调 cb(MediaPacket{ data, ptsNs, keyframe })
        │
        ▼
 ③ 两道门：PAUSE 中 → 丢弃不续单；
           awaitIdr_ 且非关键帧 → 跳过打包（但继续要下一帧）
        │
        ▼
 ④ packetizer_->packetize(pkt)            [stream 层]
     AnnexB 拆 NAL → IDR 前补 SPS/PPS → H264 FU-A 切片
        │
        ▼
 ⑤ sender_->sendRtp(payload, ts, marker)  [RtspRtpSender]
     补 12 字节 RTP 头（seq++、随机 ssrc、pts→RTP 时间戳）
     ├─ UDP 模式：sendmsg 直发客户端 client_port
     └─ TCP 模式：'$'交织帧 → RtspConnection::sendBytes 复用同一条连接
        │
        ▼
 ⑥ loop_->post(pullNext) → 回到 ① 要下一帧
     （post 续拉而非直接递归：同步 Source 立即回帧会无限递归爆栈）
```

**图 6：线程模型**

```
 EventLoop::run() 所在线程 = 一切回调的执行者：
   listen fd 可读 → onAccept → new RtspConnection
   连接 fd 可读   → 解析请求 / 发响应 / 收交织 RTCP
   rtcp fd 可读   → RTCP RR 统计（也是保活信号）
   定时器到点     → RTCP SR 周期上报 / 空闲超时巡检
 另一个线程只有相机采集/编码：帧先进 BufferPool，
 经 post 汇入 EventLoop 线程才回调 ② —— 所以 rtsp 层内部无锁
```

**建议的读码顺序**：`RtspServer.h`（对外的全部契约，47 行）→ `RtspConnection::handleRequest`（图 3 的总机）→ `onSetup`/`onPlay`（会话怎么建、怎么占源）→ `RtspSession::pullNext`（图 5，闭环心脏）→ 最后才是认证/超时这些 `RtspConnection` 里的横切逻辑。

如果这几张图对你有用，我可以把它们并进 `SvcKit/Protocol/rtsp/readme.md`，让下一个人不用再问一遍。
