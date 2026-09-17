#include "base/Hash.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace darkos {
namespace {

constexpr std::array<std::uint32_t, 64> kShift = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr std::array<std::uint32_t, 64> kConstant = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf,
    0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
    0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e,
    0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
    0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
    0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039,
    0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
    0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

std::uint32_t rotateLeft(std::uint32_t value, std::uint32_t count) {
  return (value << count) | (value >> (32 - count));
}

} // namespace

std::string md5Hex(std::string_view input) {
  const std::uint64_t bitLength = static_cast<std::uint64_t>(input.size()) * 8;
  std::vector<std::uint8_t> bytes(input.begin(), input.end());
  bytes.push_back(0x80);
  while ((bytes.size() % 64) != 56)
    bytes.push_back(0);
  for (unsigned index = 0; index < 8; ++index)
    bytes.push_back(static_cast<std::uint8_t>(bitLength >> (index * 8)));

  std::uint32_t a0 = 0x67452301;
  std::uint32_t b0 = 0xefcdab89;
  std::uint32_t c0 = 0x98badcfe;
  std::uint32_t d0 = 0x10325476;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
    std::uint32_t words[16]{};
    for (unsigned index = 0; index < 16; ++index) {
      const std::size_t base = offset + index * 4;
      words[index] = static_cast<std::uint32_t>(bytes[base]) |
                     (static_cast<std::uint32_t>(bytes[base + 1]) << 8) |
                     (static_cast<std::uint32_t>(bytes[base + 2]) << 16) |
                     (static_cast<std::uint32_t>(bytes[base + 3]) << 24);
    }
    std::uint32_t a = a0, b = b0, c = c0, d = d0;
    for (std::uint32_t index = 0; index < 64; ++index) {
      std::uint32_t f = 0;
      std::uint32_t word = 0;
      if (index < 16) {
        f = (b & c) | ((~b) & d);
        word = index;
      } else if (index < 32) {
        f = (d & b) | ((~d) & c);
        word = (5 * index + 1) % 16;
      } else if (index < 48) {
        f = b ^ c ^ d;
        word = (3 * index + 5) % 16;
      } else {
        f = c ^ (b | (~d));
        word = (7 * index) % 16;
      }
      const std::uint32_t nextD = d;
      d = c;
      c = b;
      b += rotateLeft(a + f + kConstant[index] + words[word], kShift[index]);
      a = nextD;
    }
    a0 += a;
    b0 += b;
    c0 += c;
    d0 += d;
  }

  const std::uint32_t digest[4] = {a0, b0, c0, d0};
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::uint32_t word : digest) {
    for (unsigned index = 0; index < 4; ++index)
      output << std::setw(2) << ((word >> (index * 8)) & 0xff);
  }
  return output.str();
}

} // namespace darkos
