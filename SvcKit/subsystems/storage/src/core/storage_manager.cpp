#include "storage_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>

namespace darkos::storage {
namespace {

std::uint64_t nowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string safeLabel(const std::string &input) {
  std::string output;
  output.reserve(std::min<std::size_t>(input.size(), 48));
  for (const unsigned char value : input) {
    if (output.size() == 48)
      break;
    output.push_back((value >= 'a' && value <= 'z') ||
                             (value >= 'A' && value <= 'Z') ||
                             (value >= '0' && value <= '9') || value == '-' ||
                             value == '_'
                         ? static_cast<char>(value)
                         : '_');
  }
  return output.empty() ? "recording" : output;
}

const char *extension(media::VideoCodec codec) {
  switch (codec) {
  case media::VideoCodec::H265:
    return ".h265";
  case media::VideoCodec::Mjpeg:
    return ".mjpg";
  case media::VideoCodec::H264:
    return ".h264";
  }
  return ".video";
}

struct State {
  explicit State(StorageConfig value) : config(std::move(value)) {}
  StorageConfig config;
  mutable std::mutex mutex;
  std::vector<RecordingInfo> recordings;
  std::atomic<std::uint64_t> sequence{1};
  std::uint64_t deleted{0};
  std::uint64_t writeErrors{0};
};

bool persistIndexLocked(const State &state) {
  const auto path = state.config.root / "recordings.index";
  const auto temporary = state.config.root / "recordings.index.tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output)
    return false;
  for (const auto &value : state.recordings) {
    output << std::quoted(value.id) << ' ' << std::quoted(value.label) << ' '
           << std::quoted(value.alarmId) << ' '
           << std::quoted(value.path.filename().string()) << ' '
           << static_cast<unsigned>(value.codec) << ' '
           << value.startTimestampNs << ' ' << value.endTimestampNs << ' '
           << value.bytes << ' ' << value.packets << '\n';
  }
  output.close();
  if (!output)
    return false;
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
  }
  return !error;
}

void loadIndex(State &state) {
  std::ifstream input(state.config.root / "recordings.index");
  RecordingInfo value;
  std::string filename;
  unsigned codec = 0;
  while (input >> std::quoted(value.id) >> std::quoted(value.label) >>
         std::quoted(value.alarmId) >> std::quoted(filename) >> codec >>
         value.startTimestampNs >> value.endTimestampNs >> value.bytes >>
         value.packets) {
    value.path = state.config.root / std::filesystem::path(filename).filename();
    value.codec = static_cast<media::VideoCodec>(codec);
    std::error_code error;
    const auto actualSize = std::filesystem::file_size(value.path, error);
    if (!error) {
      value.bytes = actualSize;
      state.recordings.push_back(value);
    }
  }
}

int cleanupLocked(State &state) {
  auto managedBytes = [&] {
    std::uint64_t total = 0;
    for (const auto &value : state.recordings)
      total += value.bytes;
    return total;
  };
  auto needsCleanup = [&] {
    if (state.config.maxBytes != 0 && managedBytes() > state.config.maxBytes)
      return true;
    if (state.config.minimumFreeBytes != 0) {
      std::error_code error;
      const auto space = std::filesystem::space(state.config.root, error);
      return !error && space.available < state.config.minimumFreeBytes;
    }
    return false;
  };
  bool changed = false;
  while (!state.recordings.empty() && needsCleanup()) {
    const auto oldest = std::min_element(
        state.recordings.begin(), state.recordings.end(),
        [](const RecordingInfo &left, const RecordingInfo &right) {
          return left.startTimestampNs < right.startTimestampNs;
        });
    std::error_code error;
    std::filesystem::remove(oldest->path, error);
    if (error)
      return -error.value();
    state.recordings.erase(oldest);
    ++state.deleted;
    changed = true;
  }
  if (changed && !persistIndexLocked(state))
    return -EIO;
  return 0;
}

class RecorderImpl final : public Recorder {
public:
  RecorderImpl(std::shared_ptr<State> state, std::string label,
               std::string alarmId)
      : state_(std::move(state)), label_(std::move(label)),
        alarmId_(std::move(alarmId)) {}
  ~RecorderImpl() override { stop(); }

  int start() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
      return -EALREADY;
    running_ = true;
    return 0;
  }

  int stop() override {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ && !output_.is_open())
      return 0;
    running_ = false;
    return finishSegmentLocked();
  }

  int consume(media::VideoPacketPtr packet) override {
    if (!packet || !packet->buffer || packet->buffer->size() == 0)
      return -EINVAL;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
      return -ESHUTDOWN;
    if (output_.is_open() && current_.codec != packet->codec)
      return -EPROTO;
    if (output_.is_open() && rotatePending_ && packet->keyframe) {
      const int result = finishSegmentLocked();
      if (result != 0)
        return result;
    }
    // H.26x segments must start at a random-access point; alarm recorders may
    // be attached while the encoder is in the middle of a GOP.
    if (!output_.is_open() && !packet->keyframe &&
        (packet->codec == media::VideoCodec::H264 ||
         packet->codec == media::VideoCodec::H265))
      return 0;
    if (!output_.is_open()) {
      const int result = openSegmentLocked(*packet);
      if (result != 0)
        return result;
    }
    output_.write(reinterpret_cast<const char *>(packet->buffer->data()),
                  static_cast<std::streamsize>(packet->buffer->size()));
    if (!output_) {
      const std::lock_guard<std::mutex> stateLock(state_->mutex);
      ++state_->writeErrors;
      return -EIO;
    }
    current_.bytes += packet->buffer->size();
    ++current_.packets;
    current_.endTimestampNs = packet->timestampNs;
    bytesWritten_ += packet->buffer->size();
    rotatePending_ = state_->config.segmentMaxBytes != 0 &&
                     current_.bytes >= state_->config.segmentMaxBytes;
    return 0;
  }

  std::string currentRecordingId() const override {
    const std::lock_guard<std::mutex> lock(mutex_);
    return current_.id;
  }

  std::uint64_t bytesWritten() const noexcept override {
    return bytesWritten_.load();
  }

private:
  int openSegmentLocked(const media::VideoPacket &packet) {
    const auto sequence = state_->sequence.fetch_add(1);
    current_ = {};
    current_.id = std::to_string(nowNs()) + "-" + std::to_string(sequence);
    current_.label = label_;
    current_.alarmId = alarmId_;
    current_.codec = packet.codec;
    current_.startTimestampNs = packet.timestampNs;
    current_.endTimestampNs = packet.timestampNs;
    current_.path = state_->config.root /
                    (current_.id + "-" + safeLabel(label_) +
                     extension(packet.codec));
    output_.open(current_.path, std::ios::binary | std::ios::trunc);
    return output_ ? 0 : -EIO;
  }

  int finishSegmentLocked() {
    if (!output_.is_open())
      return 0;
    output_.flush();
    output_.close();
    if (!output_) {
      const std::lock_guard<std::mutex> stateLock(state_->mutex);
      ++state_->writeErrors;
      return -EIO;
    }
    {
      const std::lock_guard<std::mutex> stateLock(state_->mutex);
      state_->recordings.push_back(current_);
      if (!persistIndexLocked(*state_)) {
        ++state_->writeErrors;
        return -EIO;
      }
      const int result = cleanupLocked(*state_);
      if (result != 0)
        return result;
    }
    current_ = {};
    rotatePending_ = false;
    return 0;
  }

  std::shared_ptr<State> state_;
  std::string label_;
  std::string alarmId_;
  mutable std::mutex mutex_;
  std::ofstream output_;
  RecordingInfo current_;
  std::atomic<std::uint64_t> bytesWritten_{0};
  bool running_{false};
  bool rotatePending_{false};
};

class StorageManagerImpl final : public StorageManager {
public:
  explicit StorageManagerImpl(std::shared_ptr<State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<Recorder>
  createRecorder(const std::string &label, const std::string &alarmId,
                 std::string &error) override {
    if (label.empty()) {
      error = "recording label must not be empty";
      return nullptr;
    }
    auto recorder = std::make_shared<RecorderImpl>(state_, label, alarmId);
    error.clear();
    return recorder;
  }

  std::vector<RecordingInfo>
  query(const RecordingQuery &filter) const override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<RecordingInfo> output;
    for (auto iterator = state_->recordings.rbegin();
         iterator != state_->recordings.rend() && output.size() < filter.limit;
         ++iterator) {
      if (!filter.label.empty() && iterator->label != filter.label)
        continue;
      if (!filter.alarmId.empty() && iterator->alarmId != filter.alarmId)
        continue;
      if (filter.fromTimestampNs != 0 &&
          iterator->endTimestampNs < filter.fromTimestampNs)
        continue;
      if (filter.toTimestampNs != 0 &&
          iterator->startTimestampNs > filter.toTimestampNs)
        continue;
      output.push_back(*iterator);
    }
    return output;
  }

  int read(const std::string &id, std::uint64_t offset,
           std::size_t maximumBytes,
           std::vector<std::uint8_t> &output) const override {
    std::filesystem::path path;
    std::uint64_t size = 0;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      const auto entry = std::find_if(
          state_->recordings.begin(), state_->recordings.end(),
          [&](const RecordingInfo &value) { return value.id == id; });
      if (entry == state_->recordings.end())
        return -ENOENT;
      path = entry->path;
      size = entry->bytes;
    }
    if (offset > size)
      return -EINVAL;
    const std::uint64_t remaining = size - offset;
    const std::size_t count = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, maximumBytes));
    output.assign(count, 0);
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char *>(output.data()),
               static_cast<std::streamsize>(count));
    if (static_cast<std::size_t>(input.gcount()) != count) {
      output.clear();
      return -EIO;
    }
    return 0;
  }

  int remove(const std::string &id) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = std::find_if(
        state_->recordings.begin(), state_->recordings.end(),
        [&](const RecordingInfo &value) { return value.id == id; });
    if (entry == state_->recordings.end())
      return -ENOENT;
    std::error_code error;
    std::filesystem::remove(entry->path, error);
    if (error)
      return -error.value();
    state_->recordings.erase(entry);
    ++state_->deleted;
    return persistIndexLocked(*state_) ? 0 : -EIO;
  }

  int cleanup() override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return cleanupLocked(*state_);
  }

  StorageStats stats() const noexcept override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    StorageStats output;
    output.recordingCount = state_->recordings.size();
    for (const auto &value : state_->recordings)
      output.managedBytes += value.bytes;
    output.deletedRecordings = state_->deleted;
    output.writeErrors = state_->writeErrors;
    return output;
  }

private:
  std::shared_ptr<State> state_;
};

} // namespace

std::unique_ptr<StorageManager>
StorageManager::create(const StorageConfig &config, std::string &error) {
  if (config.root.empty()) {
    error = "storage root must not be empty";
    return nullptr;
  }
  std::error_code filesystemError;
  std::filesystem::create_directories(config.root, filesystemError);
  if (filesystemError) {
    error = "create storage root failed: " + filesystemError.message();
    return nullptr;
  }
  auto state = std::make_shared<State>(config);
  loadIndex(*state);
  auto manager = std::unique_ptr<StorageManager>(
      new (std::nothrow) StorageManagerImpl(std::move(state)));
  if (!manager) {
    error = "out of memory creating storage manager";
    return nullptr;
  }
  error.clear();
  return manager;
}

} // namespace darkos::storage
