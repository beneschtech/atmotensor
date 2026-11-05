// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -- same as zlib's crc32().
#pragma once

#include <cstddef>
#include <cstdint>

namespace atmo
{

// Pass the previous result as `seed` to checksum data in pieces.
std::uint32_t crc32(const void* data, std::size_t n, std::uint32_t seed = 0);

}  // namespace atmo
