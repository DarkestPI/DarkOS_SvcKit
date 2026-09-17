#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace darkos::protocols::common {

/** RFC 6184 Annex-B H.264 payload packetizer. */
class H264RtpPacketizer final {
public:
  using PayloadCallback =
      std::function<bool(const std::uint8_t *, std::size_t, bool marker)>;

  explicit H264RtpPacketizer(std::size_t maximumPayloadBytes = 1200);
  bool packetize(const std::uint8_t *annexB, std::size_t size,
                 const PayloadCallback &callback) const;

private:
  std::size_t maximumPayloadBytes_;
};

std::uint32_t videoTimestamp90k(std::uint64_t timestampNs) noexcept;

} // namespace darkos::protocols::common
