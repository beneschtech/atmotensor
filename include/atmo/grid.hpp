// Grid definitions: everything that depends only on the *grid*, never on the
// field, so it can be computed once and shared by every field/forecast hour
// on that grid (GFS 0.25, RRFS, HRRR, ...).
//
//   GridIdentity  what makes two grids "the same": opaque canonical bytes
//                 (for GRIB2: the section 3 template octets) + ni, nj + the
//                 periodic-in-i flag.  Hashed to a 64-bit GridKey.
//   GridData      per-point latitude/longitude (degrees, double) and the
//                 rotation angle of the grid's +i axis (radians, float).
//   Grid          immutable view of one grid: arrays, nearest-point lookup,
//                 and locate() = lat/lon -> containing cell + fractions.
//
// Conventions
//   * Points are a regular (i, j) array, row-major, i fastest:
//     index = j*ni + i.  Ingest is responsible for normalising GRIB scan
//     modes so that this order is the same for data and coordinates.
//   * A "cell" (i, j) is the quad with corners (i,j) (i+1,j) (i+1,j+1)
//     (i,j+1); with periodic_i the last column's cell wraps to column 0.
//   * rotation alpha = angle of the grid +i axis, measured counter-clockwise
//     from local east.  Grid-relative winds (u, v) become earth-relative
//     with rotate_to_earth().
//   * Only structured grids are handled here.  Unstructured grids (reduced
//     Gaussian, cubed sphere) would get their own class.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "atmo/kdtree.hpp"

namespace atmo
{

using GridKey = std::uint64_t;

constexpr std::uint64_t kFnv1aOffset = 14695981039346656037ULL;

std::uint64_t
fnv1a64(const void* data, std::size_t n, std::uint64_t h = kFnv1aOffset);

// 16 lower-case hex digits.
std::string key_hex(GridKey k);

struct GridIdentity
{
  static constexpr std::size_t kMaxCanon = 2048;

  std::vector<std::uint8_t> canon;  // canonical definition bytes
  std::uint32_t ni = 0, nj = 0;
  bool periodic_i = false;  // longitude wraps (global lat/lon grids)

  GridKey key() const;
  bool operator==(const GridIdentity& o) const = default;
  void validate() const;
  std::size_t points() const;
};

struct GridData
{
  std::vector<double> lat_deg, lon_deg;
  std::vector<float> rot_rad;  // may be empty -> derived
};

// Rotation of the +i axis relative to east, from the coordinate arrays alone
// (works for any projection, no PROJ needed).  Uses a centred difference
// where both neighbours exist, one-sided at non-periodic edges.  Where the
// neighbours coincide (pole rows of a lat/lon grid) the angle is 0.
std::vector<float> derive_rotation(
  const std::vector<double>& lat_deg,
  const std::vector<double>& lon_deg,
  std::uint32_t ni,
  std::uint32_t nj,
  bool periodic_i);

// Grid-relative (u along +i, v along +j) -> earth-relative (east, north).
void rotate_to_earth(
  double alpha_rad,
  double u,
  double v,
  double& east,
  double& north);
void rotate_to_grid(
  double alpha_rad,
  double east,
  double north,
  double& u,
  double& v);

class Grid
{
 public:
  struct Cell
  {
    std::uint32_t i, j;  // lower-left corner of the cell
    double fx, fy;       // position inside the cell, each in [0, 1]
  };

  // Owning grid from in-memory data.  Derives rotation if `data.rot_rad` is
  // empty.
  static std::shared_ptr<const Grid> create(GridIdentity id, GridData data);

  // Grid over externally owned arrays (e.g. an mmap); `keepalive` owns them.
  static std::shared_ptr<const Grid> adopt(
    GridIdentity id,
    const double* lat_deg,
    const double* lon_deg,
    const float* rot_rad,
    std::shared_ptr<const void> keepalive);

  const GridIdentity& identity() const { return id_; }
  GridKey key() const { return key_; }
  std::uint32_t ni() const { return id_.ni; }
  std::uint32_t nj() const { return id_.nj; }
  bool periodic_i() const { return id_.periodic_i; }
  const double* lat_deg() const { return lat_; }
  const double* lon_deg() const { return lon_; }
  const float* rot_rad() const { return rot_; }

  std::size_t size() const;
  std::size_t index(std::uint32_t i, std::uint32_t j) const;

  // Flat index of the closest grid point (great-circle order).
  std::optional<std::uint32_t> nearest(double lat_deg, double lon_deg) const;

  // Containing cell and fractional position, or nullopt if outside the grid.
  // Fractions come from an inverse bilinear solve of the cell's four corners
  // in the local tangent plane of the query.  That treats each cell as a flat
  // quad, so against the true projection (or true lat/lon spacing) the
  // fractions are off by up to ~1e-3 of a cell for 1 degree cells at mid
  // latitudes, shrinking in proportion to the cell size (~1e-4 at 0.25
  // degrees, ~1e-5 at 3 km).  Regular lat/lon grids can use exact arithmetic
  // instead if that ever matters.
  std::optional<Cell> locate(double lat_deg, double lon_deg) const;

 private:
  static constexpr double kSnapKm2 = 1e-8;  // 10 cm
  static constexpr double kCellTol = 1e-7;

  Grid(
    GridIdentity id,
    const double* lat_deg,
    const double* lon_deg,
    const float* rot_rad,
    std::shared_ptr<const void> keep);

  // Spatial-only 4D tree over grid points (ids = flat index), built on first
  // use and shared by all callers.  ~37 bytes/point.
  const KdTree4<double>& tree() const;

  GridIdentity id_;
  GridKey key_;
  const double* lat_;
  const double* lon_;
  const float* rot_;
  std::shared_ptr<const void> keep_;
  mutable std::once_flag tree_once_;
  mutable std::unique_ptr<KdTree4<double>> tree_;
};

}  // namespace atmo
