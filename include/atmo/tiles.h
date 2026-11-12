// PNG map tiles (T10b; docs/api/query-api-v1.md /v1/tiles): Web Mercator
// XYZ tiles of 256 x 256 px, coloured with a request-supplied Colormap.
//
// Each pixel is sampled at its centre: Grid::locate -> bilinear value.
// Locating 65536 pixels one by one costs ~90 ms, so the grid coordinates
// (i + fx, j + fy) are located on a 17 x 17 lattice (every 16 px) and
// interpolated inside each 16 x 16 block; a block falls back to per-pixel
// locate when a corner is off the grid or straddles the periodic seam.
// Blocks entirely outside the grid's lat/lon bounding box are skipped.
// Winds are earth-relative: grid-relative u/v are rotated per pixel.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "atmo/colormap.h"
#include "atmo/query.h"

namespace atmo
{

constexpr std::uint32_t kTileSize = 256;

struct TileRequest
{
  std::string model;
  FieldSpec field;  // explicit level (Level::fixed)
  std::int64_t run_s = 0;
  std::uint32_t fhr = 0;
  std::uint32_t z = 0, x = 0, y = 0;
};

struct TileResult
{
  enum class Status
  {
    ok,         // at least one pixel on the grid
    empty,      // tile outside the grid: fully transparent PNG
    no_time,    // model has no data for (run, fhr)
    no_field,   // field not stored at that time
    ambiguous,  // several signatures of the parameter (catalog, Q19b)
  };
  Status status = Status::ok;
  std::string png;  // set for ok and empty
};

// Values at the 256 x 256 pixel centres, row-major from the top-left;
// NaN off the grid or where missing.  `status` reports no_time/no_field
// (values then empty) or empty (all NaN because off the grid).
// std::invalid_argument for z > 24, x or y >= 2^z, or a field without an
// explicit level.
std::vector<double> tile_values(
  const FieldStore& store,
  const TileRequest& r,
  TileResult::Status& status);

TileResult render_tile(
  const FieldStore& store,
  const TileRequest& r,
  const Colormap& cmap);

}  // namespace atmo
