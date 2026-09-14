/*
 * live_probe：端到端出流验证（M1 真实码流联调）
 *
 * 链路：host_x86 相机（默认合成测试图案；DARKOS_CAMERA_DEVICE=/dev/video0 时
 *     采集 UVC 真实摄像头，YUYV→NV12）
 *     → x264 软编（host_x86 codec HAL，阻塞 encode，输出 Annex-B H.264）
 *     → HalFrameSource（推/拉转换：采集线程拷贝进 BufferPool，post 到
 *       EventLoop 线程交付挂起的 getNextFrame 请求；池空/无请求即丢帧）
 *     → RtspServer（rtsp://0.0.0.0:8554/live）
 *
 * 运行：
 *   DARKOS_HAL_VARIANT=host_x86 \
 *   DARKOS_HAL_LIBRARY_PATH=<build>/oem/lib \
 *   [DARKOS_CAMERA_DEVICE=/dev/video0] <build>/oem/bin/live_probe
 *
 * 拉流验证：
 *   ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live -t 10 -f null -
 *
 * SIGINT/SIGTERM 优雅退出：主线程 sigprocmask 屏蔽后由独立线程 sigwait，
 * 经 loop->post 触发 quit；回收顺序 server → source(停采集) → loop。
 */

#include <camera/ICameraDevice.h>
#include <hardware/hardware.h>
#include <media/ICodec.h>

#include <atomic>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "base/BufferPool.h"
#include "base/EventLoop.h"
#include <svc_log.h>
#include "base/Thread.h"
#include "rtsp/RtspServer.h"
#include "stream/SourceSink.h"

using namespace darkos;

static constexpr const char *kTag = "live_probe";
static constexpr uint16_t kRtspPort = 8554;

/* 采集/编码参数：低分辨率流，够验证即可 */
static constexpr uint32_t kWidth = 640;
static constexpr uint32_t kHeight = 480;
static constexpr uint32_t kFps = 30;
static constexpr uint32_t kBitrate = 512 * 1000;
static constexpr uint32_t kGop = 15; /* 1s 一个关键帧，拉流端起播快 */

/* ---------------------------------------------------------------------------
 * HalFrameSource：HAL 推模型 → Source 拉模型 的转换器（多客户端 fan-out）
 *
 * 相机帧回调运行在采集线程：就地调阻塞 encode 得 Annex-B H.264，
 * 拷贝进 BufferPool 的 Buffer（MediaPacket.buf 持有续命），再 post 到
 * EventLoop 线程交付。getNextFrame 支持多个并发挂起请求（多客户端共享
 * 一路流），帧到达时广播给全部挂起请求；无挂起请求或池空时丢帧，
 * 绝不阻塞采集线程。
 * ------------------------------------------------------------------------- */
class HalFrameSource : public Source {
  public:
    HalFrameSource(EventLoop *loop, camera_device_t *camera, codec_device_t *codec, uint32_t width,
                   uint32_t height)
        : loop_(loop), camera_(camera), codec_(codec), width_(width), height_(height) {
        pool_.reset(BufferPool::create(width * height, 8));
        /* 编码输出缓冲：H.264 码流远小于一帧原始数据，此容量恒够用 */
        scratch_ = (uint8_t *)malloc((size_t)width * height * 2);
    }

    ~HalFrameSource() override {
        free(scratch_);
    }

    int start() {
        if (pool_ == nullptr || scratch_ == nullptr)
            return -1;
        media_codec_format_t fmt = {};
        fmt.codec = MEDIA_CODEC_H264;
        fmt.width = width_;
        fmt.height = height_;
        fmt.pixel_format = CAMERA_PIX_FMT_NV12;
        fmt.bitrate = kBitrate;
        fmt.fps = kFps;
        fmt.gop = kGop;
        int rc = codec_->ops->set_format(codec_, &fmt);
        if (rc != 0) {
            SVC_LOGE(kTag, "codec set_format failed: %d", rc);
            return rc;
        }
        rc = codec_->ops->start(codec_);
        if (rc != 0) {
            SVC_LOGE(kTag, "codec start failed: %d", rc);
            return rc;
        }
        rc = camera_->ops->set_frame_callback(camera_, &HalFrameSource::onFrame, this);
        if (rc != 0)
            return rc;
        rc = camera_->ops->start(camera_);
        if (rc != 0) {
            SVC_LOGE(kTag, "camera start failed: %d", rc);
            return rc;
        }
        return 0;
    }

    /* 先停相机（join 采集线程，此后不再有回调），再停编码器 */
    void stop() {
        camera_->ops->stop(camera_);
        codec_->ops->stop(codec_);
    }

    void getNextFrame(FrameCallback cb) override {
        pending_.push_back(std::move(cb));
    }

    SubscriptionId subscribe(FrameCallback) override {
        return -1;
    } /* 未用推模式 */
    void unsubscribe(SubscriptionId) override {}

    uint64_t captured() const {
        return captured_.load();
    }
    uint64_t delivered() const {
        return delivered_.load();
    }
    uint64_t droppedPool() const {
        return droppedPool_.load();
    }
    uint64_t droppedIdle() const {
        return droppedIdle_.load();
    }

  private:
    /* 采集线程上下文：编码 + 拷贝入池 + post 到 loop 线程 */
    static int onFrame(void *ctx, const camera_frame_t *frame) {
        auto *self = (HalFrameSource *)ctx;
        self->captured_++;

        media_buffer_t in = {};
        in.fd = -1;
        in.data = frame->data;
        in.size = frame->size;
        in.timestamp_ns = frame->timestamp_ns;

        media_buffer_t pkt = {};
        pkt.fd = -1;
        pkt.data = self->scratch_;
        pkt.size = (uint32_t)((size_t)self->width_ * self->height_ * 2);

        int rc = self->codec_->ops->encode(self->codec_, &in, &pkt, 0);
        if (rc != 0 || pkt.size == 0) {
            SVC_LOGE(kTag, "encode failed: %d", rc);
            return 0;
        }

        BufferPool::BufferRef buf = self->pool_->acquire();
        if (buf == nullptr) {
            self->droppedPool_++; /* 池空丢帧，不阻塞采集线程 */
            return 0;
        }
        if (pkt.size > buf->capacity) { /* 防御：池块容量按原始帧开，不应发生 */
            self->droppedPool_++;
            return 0;
        }
        memcpy(buf->bytes(), pkt.data, pkt.size);

        MediaPacket mp;
        mp.data = buf->bytes();
        mp.size = pkt.size;
        mp.ptsNs = pkt.timestamp_ns;
        mp.keyframe = (pkt.flags & MEDIA_BUF_FLAG_KEYFRAME) != 0;
        mp.codec = MediaCodec::kH264;
        mp.buf = std::move(buf);

        self->loop_->post([self, mp] { self->deliver(mp); });
        return 0;
    }

    /* loop 线程：广播给全部挂起的拉请求（多客户端共享同一帧）；无请求（未 PLAY）即丢 */
    void deliver(const MediaPacket &mp) {
        if (pending_.empty()) {
            droppedIdle_++;
            return;
        }
        std::vector<FrameCallback> cbs = std::move(pending_);
        pending_.clear();
        delivered_++;
        for (auto &cb : cbs)
            cb(mp);
    }

    EventLoop *loop_;
    camera_device_t *camera_;
    codec_device_t *codec_;
    uint32_t width_; /* 相机协商后的实际分辨率 */
    uint32_t height_;
    std::unique_ptr<BufferPool> pool_;
    uint8_t *scratch_ = nullptr;
    std::vector<FrameCallback> pending_; /* 仅 loop 线程访问 */

    std::atomic<uint64_t> captured_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> droppedPool_{0};
    std::atomic<uint64_t> droppedIdle_{0};
};

int main() {
    /* 屏蔽 SIGINT/SIGTERM，交给 sigwait 线程（须在创建任何线程前设置） */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    const hw_module_t *camera_mod = nullptr;
    const hw_module_t *codec_mod = nullptr;
    if (hw_get_module(CAMERA_HARDWARE_MODULE_ID, &camera_mod) != 0 ||
        hw_get_module(MEDIA_CODEC_HARDWARE_MODULE_ID, &codec_mod) != 0) {
        SVC_LOGE(kTag, "hw_get_module failed（检查 DARKOS_HAL_VARIANT / DARKOS_HAL_LIBRARY_PATH）");
        return 1;
    }
    SVC_LOGI(kTag, "HAL: %s | %s", camera_mod->name, codec_mod->name);

    camera_device_t *camera = nullptr;
    codec_device_t *codec = nullptr;
    if (camera_open(camera_mod, &camera) != 0 || codec_open(codec_mod, &codec) != 0) {
        SVC_LOGE(kTag, "open camera/codec device failed");
        return 1;
    }

    camera_format_t cam_fmt = {};
    cam_fmt.width = kWidth;
    cam_fmt.height = kHeight;
    cam_fmt.pixel_format = CAMERA_PIX_FMT_NV12;
    cam_fmt.fps = kFps;
    if (camera->ops->set_format(camera, &cam_fmt) != 0) {
        SVC_LOGE(kTag, "camera set_format failed");
        return 1;
    }
    /* 取回实际生效的格式：UVC 摄像头可能就近调整了分辨率/帧率，
     * 编码器与缓冲必须按实际值配置 */
    if (camera->ops->get_format(camera, &cam_fmt) != 0) {
        SVC_LOGE(kTag, "camera get_format failed");
        return 1;
    }
    SVC_LOGI(kTag, "camera format: %ux%u@%ufps", cam_fmt.width, cam_fmt.height, cam_fmt.fps);

    EventLoop *loop = EventLoop::create();
    auto *source = new HalFrameSource(loop, camera, codec, cam_fmt.width, cam_fmt.height);
    if (source->start() != 0)
        return 1;

    RtspServer *server = RtspServer::create(loop, kRtspPort);
    if (!server->addStream("live", source, MediaCodec::kH264)) {
        SVC_LOGE(kTag, "addStream failed");
        return 1;
    }
    if (server->start() != 0) {
        SVC_LOGE(kTag, "RTSP server start failed");
        return 1;
    }
    SVC_LOGI(kTag, "RTSP serving: rtsp://0.0.0.0:%u/live (%ux%u@%ufps H.264)", kRtspPort, cam_fmt.width,
         cam_fmt.height, cam_fmt.fps);

    /* 周期打印帧统计，验证拉流期间交付不中断 */
    loop->scheduleEvery(5ull * 1000 * 1000 * 1000, 5ull * 1000 * 1000 * 1000, [source] {
        SVC_LOGI(kTag, "stats: captured=%llu delivered=%llu dropPool=%llu dropIdle=%llu",
             (unsigned long long)source->captured(), (unsigned long long)source->delivered(),
             (unsigned long long)source->droppedPool(), (unsigned long long)source->droppedIdle());
    });

    Thread sigThread("probe.sig", [loop, &sigs] {
        int sig = 0;
        sigwait(&sigs, &sig);
        SVC_LOGI(kTag, "signal %d received, quitting", sig);
        loop->post([loop] { loop->quit(); });
    });

    loop->run();

    /* 优雅退出：先拆服务器（停止拉帧），再停采集，最后回收 loop */
    delete server;
    source->stop();
    delete source;
    delete loop;
    sigThread.join();

    codec_close(codec);
    camera_close(camera);
    SVC_LOGI(kTag, "bye");
    return 0;
}
