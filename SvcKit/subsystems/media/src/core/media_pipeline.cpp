#include "media_pipeline.h"

#include "media_audio_codec.h"
#include "media_audio_source.h"
#include "media_factory.h"
#include "media_video_codec.h"
#include "media_video_source.h"

#include <svc_log.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace darkos::media {
namespace {

constexpr char kTag[] = "svc_media";

std::uint64_t monotonicNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

enum class PushResult { Queued, DroppedOldest, DroppedNewest, Closed };

template <typename T> class BoundedQueue final {
public:
  void reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    values_.clear();
    closed_ = false;
    changed_.notify_all();
  }

  void close() {
    const std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    changed_.notify_all();
  }

  PushResult push(T value, const MediaQueueConfig &config) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_)
      return PushResult::Closed;
    if (config.capacity == 0)
      return PushResult::DroppedNewest;
    if (values_.size() >= config.capacity) {
      if (config.policy == BackpressurePolicy::DropOldest) {
        values_.pop_front();
        values_.push_back(std::move(value));
        changed_.notify_one();
        return PushResult::DroppedOldest;
      } else {
        return PushResult::DroppedNewest;
      }
    }
    values_.push_back(std::move(value));
    changed_.notify_one();
    return PushResult::Queued;
  }

  bool pop(T &value) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return closed_ || !values_.empty(); });
    if (values_.empty())
      return false;
    value = std::move(values_.front());
    values_.pop_front();
    changed_.notify_all();
    return true;
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<T> values_;
  bool closed_{false};
};

template <typename PacketPtr, typename Sink> class SinkFanout final {
public:
  using ErrorReporter = std::function<void(int, const std::string &)>;

  explicit SinkFanout(ErrorReporter reporter)
      : reporter_(std::move(reporter)) {}

  ~SinkFanout() { stop(); }

  int add(std::shared_ptr<Sink> sink, const MediaQueueConfig &queue,
          SinkId &id) {
    if (sink == nullptr || queue.capacity == 0)
      return -EINVAL;
    auto entry = std::make_shared<Entry>();
    entry->sink = std::move(sink);
    entry->queueConfig = queue;

    const std::lock_guard<std::mutex> lock(mutex_);
    entry->id = nextId_++;
    if (running_) {
      const int rc = startEntry(entry);
      if (rc != 0)
        return rc;
    }
    entries_.push_back(entry);
    id = entry->id;
    return 0;
  }

  int remove(SinkId id) {
    std::shared_ptr<Entry> removed;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (auto iterator = entries_.begin(); iterator != entries_.end();
           ++iterator) {
        if ((*iterator)->id == id) {
          if ((*iterator)->worker.joinable() &&
              (*iterator)->worker.get_id() == std::this_thread::get_id())
            return -EDEADLK;
          removed = *iterator;
          entries_.erase(iterator);
          break;
        }
      }
    }
    if (removed == nullptr)
      return -ENOENT;
    return stopEntry(removed);
  }

  bool ownsCurrentThread() {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::thread::id current = std::this_thread::get_id();
    for (const auto &entry : entries_) {
      if (entry->worker.joinable() && entry->worker.get_id() == current)
        return true;
    }
    return false;
  }

  int start() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
      return -EALREADY;
    std::vector<std::shared_ptr<Entry>> started;
    for (const auto &entry : entries_) {
      const int rc = startEntry(entry);
      if (rc != 0) {
        for (auto iterator = started.rbegin(); iterator != started.rend();
             ++iterator)
          stopEntry(*iterator);
        return rc;
      }
      started.push_back(entry);
    }
    running_ = true;
    return 0;
  }

  int stop() {
    std::vector<std::shared_ptr<Entry>> entries;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!running_)
        return 0;
      running_ = false;
      entries = entries_;
    }
    int result = 0;
    for (const auto &entry : entries) {
      const int rc = stopEntry(entry);
      if (result == 0 && rc != 0)
        result = rc;
    }
    return result;
  }

  void dispatch(PacketPtr packet) {
    std::vector<std::shared_ptr<Entry>> entries;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!running_)
        return;
      entries = entries_;
    }
    for (const auto &entry : entries) {
      const PushResult result = entry->queue.push(packet, entry->queueConfig);
      if (result == PushResult::DroppedOldest ||
          result == PushResult::DroppedNewest)
        reporter_(-ENOBUFS, "sink queue dropped a packet");
    }
  }

private:
  struct Entry {
    SinkId id{0};
    std::shared_ptr<Sink> sink;
    MediaQueueConfig queueConfig;
    BoundedQueue<PacketPtr> queue;
    std::thread worker;
    std::atomic<bool> started{false};
  };

  int startEntry(const std::shared_ptr<Entry> &entry) {
    entry->queue.reset();
    int rc = 0;
    try {
      rc = entry->sink->start();
    } catch (...) {
      rc = -EFAULT;
    }
    if (rc != 0)
      return rc;
    entry->started = true;
    try {
      entry->worker = std::thread([this, entry] { consumeLoop(entry); });
    } catch (...) {
      entry->started = false;
      entry->sink->stop();
      return -EAGAIN;
    }
    return 0;
  }

  int stopEntry(const std::shared_ptr<Entry> &entry) {
    if (!entry->started.exchange(false))
      return 0;
    entry->queue.close();
    if (entry->worker.joinable()) {
      if (entry->worker.get_id() == std::this_thread::get_id())
        return -EDEADLK;
      entry->worker.join();
    }
    try {
      return entry->sink->stop();
    } catch (...) {
      return -EFAULT;
    }
  }

  void consumeLoop(const std::shared_ptr<Entry> &entry) noexcept {
    PacketPtr packet;
    while (entry->queue.pop(packet)) {
      int rc = 0;
      try {
        rc = entry->sink->consume(std::move(packet));
      } catch (const std::exception &exception) {
        reporter_(-EFAULT, std::string("sink threw: ") + exception.what());
        continue;
      } catch (...) {
        reporter_(-EFAULT, "sink threw an unknown exception");
        continue;
      }
      if (rc != 0)
        reporter_(rc, "sink consume failed");
    }
  }

  ErrorReporter reporter_;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Entry>> entries_;
  SinkId nextId_{1};
  bool running_{false};
};

class NodeMediaPipeline final : public MediaPipeline {
public:
  NodeMediaPipeline(VideoPipelineConfig videoConfig,
                    std::unique_ptr<VideoSource> videoSource,
                    std::unique_ptr<VideoEncoder> videoEncoder,
                    std::unique_ptr<AudioSource> audioSource,
                    std::unique_ptr<AudioEncoder> audioEncoder,
                    MediaQueueConfig audioQueue)
      : videoQueueConfig_(videoConfig.inputQueue),
        audioQueueConfig_(audioQueue), videoSource_(std::move(videoSource)),
        videoEncoder_(std::move(videoEncoder)),
        audioSource_(std::move(audioSource)),
        audioEncoder_(std::move(audioEncoder)),
        videoFanout_([this](int code, const std::string &message) {
          onSinkError(code, "video_sink", message);
        }),
        audioFanout_([this](int code, const std::string &message) {
          onSinkError(code, "audio_sink", message);
        }) {
    pushEvent(MediaEventType::StateChanged, 0, "pipeline", "created",
              PipelineState::Created);
  }

  ~NodeMediaPipeline() override { stop(); }

  int addVideoSink(std::shared_ptr<VideoPacketSink> sink,
                   const MediaQueueConfig &queue, SinkId &id) override {
    return videoFanout_.add(std::move(sink), queue, id);
  }

  int addAudioSink(std::shared_ptr<AudioPacketSink> sink,
                   const MediaQueueConfig &queue, SinkId &id) override {
    if (audioSource_ == nullptr)
      return -ENOTSUP;
    return audioFanout_.add(std::move(sink), queue, id);
  }

  int removeVideoSink(SinkId id) override { return videoFanout_.remove(id); }
  int removeAudioSink(SinkId id) override { return audioFanout_.remove(id); }

  int start() override {
    const std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running())
      return -EALREADY;
    setState(PipelineState::Starting, 0, "starting");
    videoQueue_.reset();
    audioQueue_.reset();
    accepting_.store(true);

    int rc = videoEncoder_->start();
    if (rc != 0)
      return failStart(rc, "video_encoder");
    videoEncoderStarted_ = true;
    if (audioEncoder_ != nullptr) {
      rc = audioEncoder_->start();
      if (rc != 0)
        return failStart(rc, "audio_encoder");
      audioEncoderStarted_ = true;
    }
    rc = videoFanout_.start();
    if (rc != 0)
      return failStart(rc, "video_fanout");
    videoFanoutStarted_ = true;
    rc = audioFanout_.start();
    if (rc != 0)
      return failStart(rc, "audio_fanout");
    audioFanoutStarted_ = true;

    try {
      videoWorker_ = std::thread(&NodeMediaPipeline::videoEncodeLoop, this);
      if (audioEncoder_ != nullptr)
        audioWorker_ = std::thread(&NodeMediaPipeline::audioEncodeLoop, this);
    } catch (...) {
      return failStart(-EAGAIN, "encode_worker");
    }

    if (audioSource_ != nullptr) {
      rc = audioSource_->start(
          [this](AudioFramePtr frame) { onAudioFrame(std::move(frame)); },
          [this](int code, const std::string &message) {
            onSourceError(code, "audio_source", message);
          });
      if (rc != 0)
        return failStart(rc, "audio_source");
      audioSourceStarted_ = true;
    }
    rc = videoSource_->start(
        [this](VideoFramePtr frame) { onVideoFrame(std::move(frame)); },
        [this](int code, const std::string &message) {
          onSourceError(code, "video_source", message);
        });
    if (rc != 0)
      return failStart(rc, "video_source");
    videoSourceStarted_ = true;
    setState(PipelineState::Running, 0, "running");
    return 0;
  }

  int stop() override {
    if (videoFanout_.ownsCurrentThread() || audioFanout_.ownsCurrentThread())
      return -EDEADLK;
    const std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const PipelineState current = state_.load();
    if (current == PipelineState::Created || current == PipelineState::Stopped)
      return 0;
    setState(PipelineState::Stopping, 0, "stopping");
    const int result = teardown();
    setState(result == 0 ? PipelineState::Stopped : PipelineState::Failed,
             result, result == 0 ? "stopped" : "stop failed");
    return result;
  }

  PipelineState state() const noexcept override { return state_.load(); }

  bool running() const noexcept override {
    const PipelineState current = state_.load();
    return current == PipelineState::Running ||
           current == PipelineState::Degraded;
  }

  PipelineStats stats() const noexcept override {
    PipelineStats result;
    result.capturedVideoFrames = capturedVideoFrames_.load();
    result.encodedVideoPackets = encodedVideoPackets_.load();
    result.droppedVideoFrames = droppedVideoFrames_.load();
    result.videoEncodeErrors = videoEncodeErrors_.load();
    result.capturedAudioFrames = capturedAudioFrames_.load();
    result.encodedAudioPackets = encodedAudioPackets_.load();
    result.droppedAudioFrames = droppedAudioFrames_.load();
    result.audioEncodeErrors = audioEncodeErrors_.load();
    result.sinkErrors = sinkErrors_.load();
    return result;
  }

  int waitEvent(MediaEvent &event, int timeoutMs) override {
    std::unique_lock<std::mutex> lock(eventMutex_);
    if (timeoutMs < 0) {
      eventAvailable_.wait(lock, [&] { return !events_.empty(); });
    } else if (!eventAvailable_.wait_for(lock,
                                         std::chrono::milliseconds(timeoutMs),
                                         [&] { return !events_.empty(); })) {
      return -ETIMEDOUT;
    }
    event = std::move(events_.front());
    events_.pop_front();
    return 0;
  }

private:
  int failStart(int code, const char *component) {
    accepting_.store(false);
    teardown();
    pushEvent(MediaEventType::SourceError, code, component, "start failed",
              PipelineState::Failed);
    setState(PipelineState::Failed, code, "start failed");
    return code;
  }

  int teardown() {
    accepting_.store(false);
    int result = 0;
    auto remember = [&result](int rc) {
      if (result == 0 && rc != 0)
        result = rc;
    };
    if (videoSourceStarted_) {
      remember(videoSource_->stop());
      videoSourceStarted_ = false;
    }
    if (audioSourceStarted_) {
      remember(audioSource_->stop());
      audioSourceStarted_ = false;
    }
    videoQueue_.close();
    audioQueue_.close();
    if (videoWorker_.joinable())
      videoWorker_.join();
    if (audioWorker_.joinable())
      audioWorker_.join();
    if (videoEncoderStarted_) {
      remember(videoEncoder_->stop());
      videoEncoderStarted_ = false;
    }
    if (audioEncoderStarted_) {
      remember(audioEncoder_->stop());
      audioEncoderStarted_ = false;
    }
    if (videoFanoutStarted_) {
      remember(videoFanout_.stop());
      videoFanoutStarted_ = false;
    }
    if (audioFanoutStarted_) {
      remember(audioFanout_.stop());
      audioFanoutStarted_ = false;
    }
    return result;
  }

  void onVideoFrame(VideoFramePtr frame) noexcept {
    if (!accepting_.load() || frame == nullptr)
      return;
    try {
      ++capturedVideoFrames_;
      const PushResult result =
          videoQueue_.push(std::move(frame), videoQueueConfig_);
      if (result == PushResult::DroppedOldest ||
          result == PushResult::DroppedNewest) {
        ++droppedVideoFrames_;
        pushEvent(MediaEventType::VideoFrameDropped, -ENOBUFS, "video_queue",
                  "video input queue dropped a frame", state_.load());
      }
    } catch (...) {
      ++droppedVideoFrames_;
      pushEvent(MediaEventType::VideoFrameDropped, -ENOMEM, "video_queue",
                "video input queue allocation failed", state_.load());
    }
  }

  void onAudioFrame(AudioFramePtr frame) noexcept {
    if (!accepting_.load() || frame == nullptr)
      return;
    try {
      ++capturedAudioFrames_;
      const PushResult result =
          audioQueue_.push(std::move(frame), audioQueueConfig_);
      if (result == PushResult::DroppedOldest ||
          result == PushResult::DroppedNewest) {
        ++droppedAudioFrames_;
        pushEvent(MediaEventType::AudioFrameDropped, -ENOBUFS, "audio_queue",
                  "audio input queue dropped a frame", state_.load());
      }
    } catch (...) {
      ++droppedAudioFrames_;
      pushEvent(MediaEventType::AudioFrameDropped, -ENOMEM, "audio_queue",
                "audio input queue allocation failed", state_.load());
    }
  }

  void videoEncodeLoop() noexcept {
    VideoFramePtr frame;
    while (videoQueue_.pop(frame)) {
      VideoPacketPtr packet;
      int rc = 0;
      try {
        rc = videoEncoder_->encode(*frame, packet);
      } catch (const std::bad_alloc &) {
        rc = -ENOMEM;
      } catch (...) {
        rc = -EFAULT;
      }
      if (rc != 0) {
        ++videoEncodeErrors_;
        pushEvent(MediaEventType::VideoEncodeError, rc, "video_encoder",
                  "video encode failed", state_.load());
        markDegraded();
        continue;
      }
      if (packet == nullptr || packet->buffer == nullptr ||
          packet->buffer->size() == 0)
        continue;
      ++encodedVideoPackets_;
      try {
        videoFanout_.dispatch(std::move(packet));
      } catch (...) {
        onSinkError(-ENOMEM, "video_fanout", "video fanout allocation failed");
      }
    }
  }

  void audioEncodeLoop() noexcept {
    AudioFramePtr frame;
    while (audioQueue_.pop(frame)) {
      AudioPacketPtr packet;
      int rc = 0;
      try {
        rc = audioEncoder_->encode(*frame, packet);
      } catch (const std::bad_alloc &) {
        rc = -ENOMEM;
      } catch (...) {
        rc = -EFAULT;
      }
      if (rc != 0) {
        ++audioEncodeErrors_;
        pushEvent(MediaEventType::AudioEncodeError, rc, "audio_encoder",
                  "audio encode failed", state_.load());
        markDegraded();
        continue;
      }
      if (packet == nullptr || packet->buffer == nullptr ||
          packet->buffer->size() == 0)
        continue;
      ++encodedAudioPackets_;
      try {
        audioFanout_.dispatch(std::move(packet));
      } catch (...) {
        onSinkError(-ENOMEM, "audio_fanout", "audio fanout allocation failed");
      }
    }
  }

  void onSourceError(int code, const char *component,
                     const std::string &message) noexcept {
    pushEvent(MediaEventType::SourceError, code, component, message,
              state_.load());
    markDegraded();
  }

  void onSinkError(int code, const char *component,
                   const std::string &message) noexcept {
    ++sinkErrors_;
    pushEvent(MediaEventType::SinkError, code, component, message,
              state_.load());
    markDegraded();
  }

  void markDegraded() noexcept {
    PipelineState expected = PipelineState::Running;
    if (state_.compare_exchange_strong(expected, PipelineState::Degraded))
      pushEvent(MediaEventType::StateChanged, 0, "pipeline", "degraded",
                PipelineState::Degraded);
  }

  void setState(PipelineState value, int code, const std::string &message) {
    state_.store(value);
    pushEvent(MediaEventType::StateChanged, code, "pipeline", message, value);
  }

  void pushEvent(MediaEventType type, int code, std::string component,
                 std::string message, PipelineState eventState) noexcept {
    try {
      const std::lock_guard<std::mutex> lock(eventMutex_);
      if (events_.size() >= 128)
        events_.pop_front();
      events_.push_back(MediaEvent{type, eventState, code, monotonicNowNs(),
                                   std::move(component), std::move(message)});
      eventAvailable_.notify_one();
    } catch (...) {
      SVC_LOGE(kTag, "failed to record media event");
    }
  }

  MediaQueueConfig videoQueueConfig_;
  MediaQueueConfig audioQueueConfig_;
  std::unique_ptr<VideoSource> videoSource_;
  std::unique_ptr<VideoEncoder> videoEncoder_;
  std::unique_ptr<AudioSource> audioSource_;
  std::unique_ptr<AudioEncoder> audioEncoder_;
  BoundedQueue<VideoFramePtr> videoQueue_;
  BoundedQueue<AudioFramePtr> audioQueue_;
  SinkFanout<VideoPacketPtr, VideoPacketSink> videoFanout_;
  SinkFanout<AudioPacketPtr, AudioPacketSink> audioFanout_;
  std::thread videoWorker_;
  std::thread audioWorker_;
  std::atomic<PipelineState> state_{PipelineState::Created};
  std::atomic<bool> accepting_{false};
  std::mutex lifecycleMutex_;
  bool videoSourceStarted_{false};
  bool audioSourceStarted_{false};
  bool videoEncoderStarted_{false};
  bool audioEncoderStarted_{false};
  bool videoFanoutStarted_{false};
  bool audioFanoutStarted_{false};

  std::atomic<std::uint64_t> capturedVideoFrames_{0};
  std::atomic<std::uint64_t> encodedVideoPackets_{0};
  std::atomic<std::uint64_t> droppedVideoFrames_{0};
  std::atomic<std::uint64_t> videoEncodeErrors_{0};
  std::atomic<std::uint64_t> capturedAudioFrames_{0};
  std::atomic<std::uint64_t> encodedAudioPackets_{0};
  std::atomic<std::uint64_t> droppedAudioFrames_{0};
  std::atomic<std::uint64_t> audioEncodeErrors_{0};
  std::atomic<std::uint64_t> sinkErrors_{0};

  std::mutex eventMutex_;
  std::condition_variable eventAvailable_;
  std::deque<MediaEvent> events_;
};

/*
 * 编码包输入管线。
 *
 * 编码视频源由 Camera/Codec HAL 持有内部通道；这个类只负责音频采集/编码
 * 以及把已经产生的 VideoPacket 分发给各个 Sink。
 * 因此视频不会再经过 4K 原始帧队列，也不会触发通用 VideoEncoder 的
 * SendFrame 路径。
 */
class PacketMediaPipeline final : public MediaPipeline {
public:
  PacketMediaPipeline(std::unique_ptr<AudioSource> audioSource,
                      std::unique_ptr<AudioEncoder> audioEncoder,
                      std::unique_ptr<AudioPacketSource> audioPacketSource,
                      MediaQueueConfig audioQueue)
      : audioQueueConfig_(audioQueue), audioSource_(std::move(audioSource)),
        audioEncoder_(std::move(audioEncoder)),
        audioPacketSource_(std::move(audioPacketSource)),
        videoFanout_([this](int code, const std::string &message) {
          onSinkError(code, "video_sink", message);
        }),
        audioFanout_([this](int code, const std::string &message) {
          onSinkError(code, "audio_sink", message);
        }) {
    pushEvent(MediaEventType::StateChanged, 0, "pipeline", "created",
              PipelineState::Created);
  }

  ~PacketMediaPipeline() override { stop(); }

  int addVideoSink(std::shared_ptr<VideoPacketSink> sink,
                   const MediaQueueConfig &queue, SinkId &id) override {
    return videoFanout_.add(std::move(sink), queue, id);
  }

  int addAudioSink(std::shared_ptr<AudioPacketSink> sink,
                   const MediaQueueConfig &queue, SinkId &id) override {
    if (audioSource_ == nullptr && audioPacketSource_ == nullptr)
      return -ENOTSUP;
    return audioFanout_.add(std::move(sink), queue, id);
  }

  int removeVideoSink(SinkId id) override { return videoFanout_.remove(id); }
  int removeAudioSink(SinkId id) override { return audioFanout_.remove(id); }

  int pushVideoPacket(VideoPacketPtr packet) override {
    if (!accepting_.load())
      return -EPIPE;
    if (packet == nullptr || packet->buffer == nullptr ||
        packet->buffer->size() == 0)
      return -EINVAL;
    try {
      ++capturedVideoFrames_;
      ++encodedVideoPackets_;
      videoFanout_.dispatch(std::move(packet));
    } catch (...) {
      onSinkError(-ENOMEM, "video_fanout", "video fanout allocation failed");
      return -ENOMEM;
    }
    return 0;
  }

  int start() override {
    const std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (running())
      return -EALREADY;
    setState(PipelineState::Starting, 0, "starting");
    audioQueue_.reset();
    encodedAudioQueue_.reset();
    accepting_.store(true);

    int rc = 0;
    if (audioPacketSource_ == nullptr) {
      rc = audioEncoder_ != nullptr ? audioEncoder_->start() : -ENOTSUP;
      if (rc != 0)
        return failStart(rc, "audio_encoder");
      audioEncoderStarted_ = true;
    }

    rc = videoFanout_.start();
    if (rc != 0)
      return failStart(rc, "video_fanout");
    videoFanoutStarted_ = true;
    rc = audioFanout_.start();
    if (rc != 0)
      return failStart(rc, "audio_fanout");
    audioFanoutStarted_ = true;

    try {
      audioWorker_ = std::thread(&PacketMediaPipeline::audioEncodeLoop, this);
    } catch (...) {
      return failStart(-EAGAIN, "audio_encode_worker");
    }

    if (audioPacketSource_ != nullptr) {
      rc = audioPacketSource_->start(
          [this](AudioPacketPtr packet) { onEncodedAudioPacket(std::move(packet)); },
          [this](int code, const std::string &message) {
            onSourceError(code, "encoded_audio_source", message);
          });
      if (rc != 0)
        return failStart(rc, "encoded_audio_source");
      encodedAudioSourceStarted_ = true;
    } else if (audioSource_ != nullptr) {
      rc = audioSource_->start(
          [this](AudioFramePtr frame) { onAudioFrame(std::move(frame)); },
          [this](int code, const std::string &message) {
            onSourceError(code, "audio_source", message);
          });
      if (rc != 0)
        return failStart(rc, "audio_source");
      audioSourceStarted_ = true;
    }
    setState(PipelineState::Running, 0, "running");
    return 0;
  }

  int stop() override {
    if (videoFanout_.ownsCurrentThread() || audioFanout_.ownsCurrentThread())
      return -EDEADLK;
    const std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const PipelineState current = state_.load();
    if (current == PipelineState::Created || current == PipelineState::Stopped)
      return 0;
    setState(PipelineState::Stopping, 0, "stopping");
    const int result = teardown();
    setState(result == 0 ? PipelineState::Stopped : PipelineState::Failed,
             result, result == 0 ? "stopped" : "stop failed");
    return result;
  }

  PipelineState state() const noexcept override { return state_.load(); }

  bool running() const noexcept override {
    const PipelineState current = state_.load();
    return current == PipelineState::Running ||
           current == PipelineState::Degraded;
  }

  PipelineStats stats() const noexcept override {
    PipelineStats result;
    result.capturedVideoFrames = capturedVideoFrames_.load();
    result.encodedVideoPackets = encodedVideoPackets_.load();
    result.droppedVideoFrames = droppedVideoFrames_.load();
    result.videoEncodeErrors = videoEncodeErrors_.load();
    result.capturedAudioFrames = capturedAudioFrames_.load();
    result.encodedAudioPackets = encodedAudioPackets_.load();
    result.droppedAudioFrames = droppedAudioFrames_.load();
    result.audioEncodeErrors = audioEncodeErrors_.load();
    result.sinkErrors = sinkErrors_.load();
    return result;
  }

  int waitEvent(MediaEvent &event, int timeoutMs) override {
    std::unique_lock<std::mutex> lock(eventMutex_);
    if (timeoutMs < 0) {
      eventAvailable_.wait(lock, [&] { return !events_.empty(); });
    } else if (!eventAvailable_.wait_for(
                   lock, std::chrono::milliseconds(timeoutMs),
                   [&] { return !events_.empty(); })) {
      return -ETIMEDOUT;
    }
    event = std::move(events_.front());
    events_.pop_front();
    return 0;
  }

private:
  int failStart(int code, const char *component) {
    accepting_.store(false);
    teardown();
    pushEvent(MediaEventType::SourceError, code, component, "start failed",
              PipelineState::Failed);
    setState(PipelineState::Failed, code, "start failed");
    return code;
  }

  int teardown() {
    accepting_.store(false);
    int result = 0;
    auto remember = [&result](int rc) {
      if (result == 0 && rc != 0)
        result = rc;
    };
    if (audioSourceStarted_) {
      remember(audioSource_->stop());
      audioSourceStarted_ = false;
    }
    if (encodedAudioSourceStarted_) {
      remember(audioPacketSource_->stop());
      encodedAudioSourceStarted_ = false;
    }
    audioQueue_.close();
    encodedAudioQueue_.close();
    if (audioWorker_.joinable())
      audioWorker_.join();
    if (audioEncoderStarted_) {
      remember(audioEncoder_->stop());
      audioEncoderStarted_ = false;
    }
    if (videoFanoutStarted_) {
      remember(videoFanout_.stop());
      videoFanoutStarted_ = false;
    }
    if (audioFanoutStarted_) {
      remember(audioFanout_.stop());
      audioFanoutStarted_ = false;
    }
    return result;
  }

  void onAudioFrame(AudioFramePtr frame) noexcept {
    if (!accepting_.load() || frame == nullptr)
      return;
    try {
      ++capturedAudioFrames_;
      const PushResult result =
          audioQueue_.push(std::move(frame), audioQueueConfig_);
      if (result == PushResult::DroppedOldest ||
          result == PushResult::DroppedNewest) {
        ++droppedAudioFrames_;
        pushEvent(MediaEventType::AudioFrameDropped, -ENOBUFS, "audio_queue",
                  "audio input queue dropped a frame", state_.load());
      }
    } catch (...) {
      ++droppedAudioFrames_;
      pushEvent(MediaEventType::AudioFrameDropped, -ENOMEM, "audio_queue",
                "audio input queue allocation failed", state_.load());
    }
  }

  void onEncodedAudioPacket(AudioPacketPtr packet) noexcept {
    if (!accepting_.load() || packet == nullptr)
      return;
    try {
      ++capturedAudioFrames_;
      const PushResult result =
          encodedAudioQueue_.push(std::move(packet), audioQueueConfig_);
      if (result == PushResult::DroppedOldest ||
          result == PushResult::DroppedNewest) {
        ++droppedAudioFrames_;
        pushEvent(MediaEventType::AudioFrameDropped, -ENOBUFS,
                  "encoded_audio_queue", "encoded audio queue dropped a packet",
                  state_.load());
      }
    } catch (...) {
      ++droppedAudioFrames_;
      pushEvent(MediaEventType::AudioFrameDropped, -ENOMEM,
                "encoded_audio_queue", "encoded audio queue allocation failed",
                state_.load());
    }
  }

  void audioEncodeLoop() noexcept {
    if (audioPacketSource_ != nullptr) {
      AudioPacketPtr packet;
      while (encodedAudioQueue_.pop(packet)) {
        if (packet == nullptr || packet->buffer == nullptr ||
            packet->buffer->size() == 0)
          continue;
        ++encodedAudioPackets_;
        try {
          audioFanout_.dispatch(std::move(packet));
        } catch (...) {
          onSinkError(-ENOMEM, "audio_fanout",
                      "audio fanout allocation failed");
        }
      }
      return;
    }
    AudioFramePtr frame;
    while (audioQueue_.pop(frame)) {
      AudioPacketPtr packet;
      int rc = 0;
      try {
        rc = audioEncoder_->encode(*frame, packet);
      } catch (const std::bad_alloc &) {
        rc = -ENOMEM;
      } catch (...) {
        rc = -EFAULT;
      }
      if (rc != 0) {
        ++audioEncodeErrors_;
        pushEvent(MediaEventType::AudioEncodeError, rc, "audio_encoder",
                  "audio encode failed", state_.load());
        markDegraded();
        continue;
      }
      if (packet == nullptr || packet->buffer == nullptr ||
          packet->buffer->size() == 0)
        continue;
      ++encodedAudioPackets_;
      try {
        audioFanout_.dispatch(std::move(packet));
      } catch (...) {
        onSinkError(-ENOMEM, "audio_fanout", "audio fanout allocation failed");
      }
    }
  }

  void onSourceError(int code, const char *component,
                     const std::string &message) noexcept {
    pushEvent(MediaEventType::SourceError, code, component, message,
              state_.load());
    markDegraded();
  }

  void onSinkError(int code, const char *component,
                   const std::string &message) noexcept {
    ++sinkErrors_;
    pushEvent(MediaEventType::SinkError, code, component, message,
              state_.load());
    markDegraded();
  }

  void markDegraded() noexcept {
    PipelineState expected = PipelineState::Running;
    if (state_.compare_exchange_strong(expected, PipelineState::Degraded))
      pushEvent(MediaEventType::StateChanged, 0, "pipeline", "degraded",
                PipelineState::Degraded);
  }

  void setState(PipelineState value, int code, const std::string &message) {
    state_.store(value);
    pushEvent(MediaEventType::StateChanged, code, "pipeline", message, value);
  }

  void pushEvent(MediaEventType type, int code, std::string component,
                 std::string message, PipelineState eventState) noexcept {
    try {
      const std::lock_guard<std::mutex> lock(eventMutex_);
      if (events_.size() >= 128)
        events_.pop_front();
      events_.push_back(MediaEvent{type, eventState, code, monotonicNowNs(),
                                   std::move(component), std::move(message)});
      eventAvailable_.notify_one();
    } catch (...) {
      SVC_LOGE(kTag, "failed to record media event");
    }
  }

  MediaQueueConfig audioQueueConfig_;
  std::unique_ptr<AudioSource> audioSource_;
  std::unique_ptr<AudioEncoder> audioEncoder_;
  std::unique_ptr<AudioPacketSource> audioPacketSource_;
  BoundedQueue<AudioFramePtr> audioQueue_;
  BoundedQueue<AudioPacketPtr> encodedAudioQueue_;
  SinkFanout<VideoPacketPtr, VideoPacketSink> videoFanout_;
  SinkFanout<AudioPacketPtr, AudioPacketSink> audioFanout_;
  std::thread audioWorker_;
  std::atomic<PipelineState> state_{PipelineState::Created};
  std::atomic<bool> accepting_{false};
  std::mutex lifecycleMutex_;
  bool audioSourceStarted_{false};
  bool encodedAudioSourceStarted_{false};
  bool audioEncoderStarted_{false};
  bool videoFanoutStarted_{false};
  bool audioFanoutStarted_{false};

  std::atomic<std::uint64_t> capturedVideoFrames_{0};
  std::atomic<std::uint64_t> encodedVideoPackets_{0};
  std::atomic<std::uint64_t> droppedVideoFrames_{0};
  std::atomic<std::uint64_t> videoEncodeErrors_{0};
  std::atomic<std::uint64_t> capturedAudioFrames_{0};
  std::atomic<std::uint64_t> encodedAudioPackets_{0};
  std::atomic<std::uint64_t> droppedAudioFrames_{0};
  std::atomic<std::uint64_t> audioEncodeErrors_{0};
  std::atomic<std::uint64_t> sinkErrors_{0};

  std::mutex eventMutex_;
  std::condition_variable eventAvailable_;
  std::deque<MediaEvent> events_;
};

std::unique_ptr<MediaPipeline>
createPipelineNodes(const VideoPipelineConfig &videoConfig,
                    const AudioPipelineConfig *audioConfig,
                    std::string &error) {
  if (videoConfig.inputQueue.capacity == 0) {
    error = "video input queue capacity must be greater than zero";
    return nullptr;
  }
  auto videoSource = createPlatformVideoSource(videoConfig.capture, error);
  if (videoSource == nullptr)
    return nullptr;
  auto videoEncoder =
      createPlatformVideoEncoder(videoConfig.encoder, videoSource->format(),
                                 videoConfig.encodedBufferCapacity, error);
  if (videoEncoder == nullptr)
    return nullptr;

  std::unique_ptr<AudioSource> audioSource;
  std::unique_ptr<AudioEncoder> audioEncoder;
  MediaQueueConfig audioQueue;
  if (audioConfig != nullptr) {
    if (audioConfig->inputQueue.capacity == 0) {
      error = "audio input queue capacity must be greater than zero";
      return nullptr;
    }
    audioSource = createPlatformAudioSource(audioConfig->capture, error);
    if (audioSource == nullptr)
      return nullptr;
    audioEncoder =
        createAudioEncoder(audioConfig->encoder, audioSource->format(), error);
    if (audioEncoder == nullptr)
      return nullptr;
    audioQueue = audioConfig->inputQueue;
  }
  return std::make_unique<NodeMediaPipeline>(
      videoConfig, std::move(videoSource), std::move(videoEncoder),
      std::move(audioSource), std::move(audioEncoder), audioQueue);
}

} // namespace

std::unique_ptr<MediaPipeline>
createMediaPipeline(const VideoPipelineConfig &config, std::string &error) {
  return createPipelineNodes(config, nullptr, error);
}

std::unique_ptr<MediaPipeline>
createMediaPipeline(const MediaPipelineConfig &config, std::string &error) {
  return createPipelineNodes(config.video, &config.audio, error);
}

std::unique_ptr<MediaPipeline>
createPacketMediaPipeline(const AudioPipelineConfig &config, std::string &error) {
  if (config.inputQueue.capacity == 0) {
    error = "audio input queue capacity must be greater than zero";
    return nullptr;
  }
  error.clear();
  auto audioPacketSource =
      createPlatformAudioPacketSource(config.capture, config.encoder, error);
  if (audioPacketSource != nullptr) {
    return std::make_unique<PacketMediaPipeline>(
        nullptr, nullptr, std::move(audioPacketSource), config.inputQueue);
  }
  if (!error.empty())
    return nullptr;

  auto audioSource = createPlatformAudioSource(config.capture, error);
  if (audioSource == nullptr)
    return nullptr;
  auto audioEncoder =
      createAudioEncoder(config.encoder, audioSource->format(), error);
  if (audioEncoder == nullptr)
    return nullptr;
  return std::make_unique<PacketMediaPipeline>(
      std::move(audioSource), std::move(audioEncoder), nullptr,
      config.inputQueue);
}

} // namespace darkos::media
