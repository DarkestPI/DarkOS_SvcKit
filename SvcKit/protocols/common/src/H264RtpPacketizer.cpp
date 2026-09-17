#include <H264RtpPacketizer.h>

#include <algorithm>
#include <vector>

namespace darkos::protocols::common {
namespace {

struct NalUnit {
  const std::uint8_t *data{nullptr};
  std::size_t size{0};
};

std::size_t startCodeSize(const std::uint8_t *data, std::size_t size,
                          std::size_t offset) {
  if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
      data[offset + 2] == 1)
    return 3;
  if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
      data[offset + 2] == 0 && data[offset + 3] == 1)
    return 4;
  return 0;
}

std::vector<NalUnit> splitAnnexB(const std::uint8_t *data, std::size_t size) {
  std::vector<NalUnit> output;
  std::size_t offset = 0;
  while (offset < size) {
    std::size_t code = 0;
    while (offset < size && (code = startCodeSize(data, size, offset)) == 0)
      ++offset;
    if (offset == size)
      break;
    const std::size_t begin = offset + code;
    offset = begin;
    while (offset < size && startCodeSize(data, size, offset) == 0)
      ++offset;
    std::size_t end = offset;
    while (end > begin && data[end - 1] == 0)
      --end;
    if (end > begin)
      output.push_back({data + begin, end - begin});
  }
  if (output.empty() && data != nullptr && size != 0)
    output.push_back({data, size});
  return output;
}

} // namespace

H264RtpPacketizer::H264RtpPacketizer(std::size_t maximumPayloadBytes)
    : maximumPayloadBytes_(std::max<std::size_t>(maximumPayloadBytes, 3)) {}

bool H264RtpPacketizer::packetize(const std::uint8_t *annexB, std::size_t size,
                                  const PayloadCallback &callback) const {
  if (annexB == nullptr || size == 0 || !callback)
    return false;
  const auto units = splitAnnexB(annexB, size);
  if (units.empty())
    return false;
  std::vector<std::uint8_t> fragment;
  fragment.resize(maximumPayloadBytes_);
  for (std::size_t index = 0; index < units.size(); ++index) {
    const NalUnit &unit = units[index];
    const bool finalUnit = index + 1 == units.size();
    if (unit.size <= maximumPayloadBytes_) {
      if (!callback(unit.data, unit.size, finalUnit))
        return false;
      continue;
    }
    if (unit.size < 2)
      return false;
    const std::uint8_t indicator =
        static_cast<std::uint8_t>((unit.data[0] & 0xe0U) | 28U);
    const std::uint8_t type = unit.data[0] & 0x1fU;
    const std::size_t capacity = maximumPayloadBytes_ - 2;
    std::size_t offset = 1;
    bool first = true;
    while (offset < unit.size) {
      const std::size_t count = std::min(capacity, unit.size - offset);
      const bool last = offset + count == unit.size;
      fragment[0] = indicator;
      fragment[1] = static_cast<std::uint8_t>(type |
                    (first ? 0x80U : 0U) | (last ? 0x40U : 0U));
      std::copy_n(unit.data + offset, count, fragment.data() + 2);
      if (!callback(fragment.data(), count + 2, finalUnit && last))
        return false;
      offset += count;
      first = false;
    }
  }
  return true;
}

std::uint32_t videoTimestamp90k(std::uint64_t timestampNs) noexcept {
  return static_cast<std::uint32_t>((timestampNs / 1000ULL) * 90ULL / 1000ULL);
}

} // namespace darkos::protocols::common
