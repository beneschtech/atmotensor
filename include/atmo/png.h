// Minimal PNG writer (T10b): 8-bit RGBA, no interlacing.  Compressed with
// zlib when the build has it (ATMO_HAVE_ZLIB); otherwise the image data is
// written as stored (uncompressed) deflate blocks, which every PNG decoder
// reads, so the core needs no extra library.  Deterministic: equal pixels
// give equal bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace atmo
{

// `rgba` has w * h * 4 bytes, rows top to bottom.  std::invalid_argument
// for zero or oversized dimensions.
std::string
encode_png_rgba(std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba);

// True if encode_png_rgba compresses (built with zlib).
bool png_compressed();

}  // namespace atmo
