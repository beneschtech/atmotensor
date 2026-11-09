#include "atmo/png.h"

#include <stdexcept>
#include <vector>

#ifdef ATMO_HAVE_ZLIB
#include <zlib.h>
#endif

#include "atmo/crc32.h"

namespace atmo
{
namespace
{

void put32(std::string& s, std::uint32_t v)
{
  s.push_back(static_cast<char>(v >> 24));
  s.push_back(static_cast<char>((v >> 16) & 0xff));
  s.push_back(static_cast<char>((v >> 8) & 0xff));
  s.push_back(static_cast<char>(v & 0xff));
}

void chunk(std::string& out, const char type[4], const std::string& data)
{
  put32(out, static_cast<std::uint32_t>(data.size()));
  std::string body(type, 4);
  body += data;
  out += body;
  put32(out, crc32(body.data(), body.size()));
}

std::uint32_t adler32(const std::uint8_t* p, std::size_t n)
{
  std::uint32_t a = 1, b = 0;
  for (std::size_t k = 0; k < n; ++k)
  {
    a = (a + p[k]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

// A zlib stream of stored deflate blocks.
std::string zlib_stored(const std::vector<std::uint8_t>& raw)
{
  std::string z;
  z.push_back(0x78);
  z.push_back(0x01);
  std::size_t p = 0;
  do
  {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - p);
    const bool last = p + n == raw.size();
    z.push_back(static_cast<char>(last ? 1 : 0));
    z.push_back(static_cast<char>(n & 0xff));
    z.push_back(static_cast<char>(n >> 8));
    z.push_back(static_cast<char>(~n & 0xff));
    z.push_back(static_cast<char>((~n >> 8) & 0xff));
    z.append(reinterpret_cast<const char*>(raw.data() + p), n);
    p += n;
  } while (p < raw.size());
  put32(z, adler32(raw.data(), raw.size()));
  return z;
}

}  // namespace

bool png_compressed()
{
#ifdef ATMO_HAVE_ZLIB
  return true;
#else
  return false;
#endif
}

std::string
encode_png_rgba(std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba)
{
  if (w == 0 || h == 0 || w > 16384 || h > 16384)
  {
    throw std::invalid_argument("png: bad dimensions");
  }
  // Filter byte 0 (None) before each row.
  const std::size_t row = static_cast<std::size_t>(w) * 4;
  std::vector<std::uint8_t> raw((row + 1) * h);
  for (std::uint32_t y = 0; y < h; ++y)
  {
    raw[y * (row + 1)] = 0;
    std::copy(
      rgba + y * row, rgba + (y + 1) * row, raw.begin() + y * (row + 1) + 1);
  }

  std::string idat;
#ifdef ATMO_HAVE_ZLIB
  uLongf len = compressBound(static_cast<uLong>(raw.size()));
  idat.resize(len);
  if (
    compress2(
      reinterpret_cast<Bytef*>(idat.data()), &len, raw.data(),
      static_cast<uLong>(raw.size()), 6) != Z_OK)
  {
    throw std::runtime_error("png: compression failed");
  }
  idat.resize(len);
#else
  idat = zlib_stored(raw);
#endif
  (void)&zlib_stored;

  std::string out("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8 bit, RGBA, std
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", idat);
  chunk(out, "IEND", "");
  return out;
}

}  // namespace atmo
