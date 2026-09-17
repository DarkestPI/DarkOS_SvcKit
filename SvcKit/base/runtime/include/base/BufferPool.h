#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace darkos {

class BufferPool {
public:
  struct Buffer {
    std::size_t capacity{0};
    std::uint8_t *bytes() noexcept { return storage.get(); }
    const std::uint8_t *bytes() const noexcept { return storage.get(); }
    std::unique_ptr<std::uint8_t[]> storage;
  };

  using BufferRef = std::shared_ptr<Buffer>;

  static BufferPool *create(std::size_t bufferCapacity,
                            std::size_t bufferCount) noexcept;
  virtual ~BufferPool() = default;

  BufferPool(const BufferPool &) = delete;
  BufferPool &operator=(const BufferPool &) = delete;

  virtual BufferRef acquire() noexcept = 0;

protected:
  BufferPool() = default;
};

} // namespace darkos
