// Embedding of (lat, lon, height, time) into the 4D Euclidean space that
// atmo::KdTree4 searches.
//
//   (x, y, z) = (R + vertical_scale * height_km) * unit_vector(lat, lon)
//   w         = km_per_second * (t - t0)
//
// * Using the 3D unit vector instead of (lat, lon) removes the +-180 degree
//   wrap and the pole singularities; chord length is monotonic in
//   great-circle distance.
// * vertical_scale > 1 makes 1 km of altitude "cost" more than 1 km sideways.
// * km_per_second is the per-source time weight (lambda).  0 = pure spatial.
// * Time is stored relative to t0 (computed in double, then narrowed), so
//   float trees keep sub-minute resolution even for epoch times near 1.7e9.
#pragma once

#include <array>
#include <cmath>

namespace atmo
{

constexpr double kEarthRadiusKm = 6371.0088;  // IUGG mean radius
constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

struct Embedding
{
  double t0_s = 0.0;            // epoch seconds that maps to w = 0
  double km_per_second = 0.0;   // lambda
  double vertical_scale = 1.0;  // multiplier on height
  double radius_km = kEarthRadiusKm;
};

// Template: the code stays in the header (docs/CONVENTIONS.md).
template <typename T = double>
std::array<T, 4> embed(
  double lat_deg,
  double lon_deg,
  double height_m,
  double t_s,
  const Embedding& e)
{
  const double lat = lat_deg * kDegToRad;
  const double lon = lon_deg * kDegToRad;
  const double r = e.radius_km + e.vertical_scale * height_m * 1e-3;
  const double cl = std::cos(lat);
  return {
    static_cast<T>(r * cl * std::cos(lon)),
    static_cast<T>(r * cl * std::sin(lon)), static_cast<T>(r * std::sin(lat)),
    static_cast<T>((t_s - e.t0_s) * e.km_per_second)};
}

// Haversine distance on the sphere, km.  Handy for checking the embedding.
double great_circle_km(
  double lat1_deg,
  double lon1_deg,
  double lat2_deg,
  double lon2_deg,
  double radius_km = kEarthRadiusKm);

}  // namespace atmo
