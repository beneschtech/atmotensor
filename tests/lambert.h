// Spherical Lambert conformal conic projection, northern hemisphere (n > 0).
// Test scaffolding only: lets us build a realistic HRRR-style grid without
// pulling in PROJ or eccodes.  In production the lat/lon arrays come from
// eccodes.
#pragma once

#include <utility>
#include <vector>

namespace atmo_test
{

struct Lambert
{
  static constexpr double kPi = 3.14159265358979323846;
  double R, n, F, rho0, lam0;

  // Angles in degrees, R in metres.  origin = (lat0, lon0) maps to x=y=0.
  Lambert(
    double lat1_deg,
    double lat2_deg,
    double lat0_deg,
    double lon0_deg,
    double radius_m);

  static double wrap_lon_deg(double d);
  void inverse(double x_m, double y_m, double& lat_deg, double& lon_deg) const;
  void forward(double lat_deg, double lon_deg, double& x_m, double& y_m) const;
};

// nx*ny grid of (lat_deg, lon_deg), row-major (i fastest), centred on the
// projection origin.
std::vector<std::pair<double, double>>
lambert_grid(const Lambert& L, int nx, int ny, double dx_m);

}  // namespace atmo_test
