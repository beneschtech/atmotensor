#include "atmo/crc32.h"

#include <array>

namespace atmo
{
namespace
{

const std::array<std::uint32_t, 256>& crc32_table()
{
  static const std::array<std::uint32_t, 256> t = []
  {
    std::array<std::uint32_t, 256> tab{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k)
      {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      tab[i] = c;
    }
    return tab;
  }();
  return t;
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t seed)
{
  const auto& t = crc32_table();
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t c = ~seed;
  for (std::size_t i = 0; i < n; ++i)
  {
    c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace atmo
