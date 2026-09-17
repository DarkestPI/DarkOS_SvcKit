#include <storage_manager.h>

#include <media_buffer.h>

#include <array>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {
int fail(const std::filesystem::path &root) {
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return 1;
}
} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("darkos-storage-test-" + std::to_string(getpid()));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  darkos::storage::StorageConfig config;
  config.root = root;
  config.segmentMaxBytes = 1024;
  std::string error;
  auto manager = darkos::storage::StorageManager::create(config, error);
  if (!manager)
    return fail(root);
  auto recorder = manager->createRecorder("main", "alarm-1", error);
  if (!recorder || recorder->start() != 0)
    return fail(root);

  const std::array<std::uint8_t, 8> bytes{0, 0, 0, 1, 0x65, 1, 2, 3};
  for (std::uint64_t index = 0; index < 3; ++index) {
    auto packet = std::make_shared<darkos::media::VideoPacket>();
    packet->buffer = darkos::media::copyMediaBuffer(bytes.data(), bytes.size());
    packet->timestampNs = 100 + index;
    packet->codec = darkos::media::VideoCodec::H264;
    packet->keyframe = index == 0;
    if (recorder->consume(packet) != 0)
      return fail(root);
  }
  if (recorder->stop() != 0)
    return fail(root);

  const auto values = manager->query();
  if (values.size() != 1 || values.front().bytes != bytes.size() * 3 ||
      values.front().packets != 3 || values.front().alarmId != "alarm-1")
    return fail(root);
  std::vector<std::uint8_t> output;
  if (manager->read(values.front().id, 4, 8, output) != 0 ||
      output.size() != 8 || output.front() != 0x65)
    return fail(root);

  manager.reset();
  manager = darkos::storage::StorageManager::create(config, error);
  if (!manager || manager->query().size() != 1 ||
      manager->remove(values.front().id) != 0 || !manager->query().empty())
    return fail(root);
  std::filesystem::remove_all(root, ignored);
  return 0;
}
