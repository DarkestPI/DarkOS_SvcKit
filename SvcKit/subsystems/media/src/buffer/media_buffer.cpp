#include "media_buffer.h"

#include <cstring>
#include <utility>

namespace darkos::media {

MediaBuffer::MediaBuffer(std::vector<std::uint8_t> bytes)
    : bytes_(std::move(bytes)) {}

const std::uint8_t *MediaBuffer::data() const noexcept { return bytes_.data(); }

std::size_t MediaBuffer::size() const noexcept { return bytes_.size(); }

MediaBufferPtr copyMediaBuffer(const void *data, std::size_t size) noexcept {
  if (data == nullptr || size == 0)
    return nullptr;
  try {
    std::vector<std::uint8_t> bytes(size);
    std::memcpy(bytes.data(), data, size);
    return std::make_shared<const MediaBuffer>(std::move(bytes));
  } catch (...) {
    return nullptr;
  }
}

} // namespace darkos::media
