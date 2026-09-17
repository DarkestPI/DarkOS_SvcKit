#include "base/BufferPool.h"

#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace darkos {
namespace {

struct PoolState {
  std::mutex mutex;
  std::vector<std::unique_ptr<BufferPool::Buffer>> available;
};

class FixedBufferPool final : public BufferPool {
public:
  FixedBufferPool(std::size_t capacity, std::size_t count)
      : state_(std::make_shared<PoolState>()) {
    state_->available.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      auto buffer = std::make_unique<Buffer>();
      buffer->capacity = capacity;
      buffer->storage = std::make_unique<std::uint8_t[]>(capacity);
      state_->available.push_back(std::move(buffer));
    }
  }

  BufferRef acquire() noexcept override {
    try {
      std::unique_ptr<Buffer> buffer;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->available.empty())
          return {};
        buffer = std::move(state_->available.back());
        state_->available.pop_back();
      }
      const std::shared_ptr<PoolState> state = state_;
      return BufferRef(buffer.release(), [state](Buffer *released) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->available.emplace_back(released);
      });
    } catch (...) {
      return {};
    }
  }

private:
  std::shared_ptr<PoolState> state_;
};

} // namespace

BufferPool *BufferPool::create(std::size_t bufferCapacity,
                               std::size_t bufferCount) noexcept {
  if (bufferCapacity == 0 || bufferCount == 0)
    return nullptr;
  try {
    return new FixedBufferPool(bufferCapacity, bufferCount);
  } catch (...) {
    return nullptr;
  }
}

} // namespace darkos
