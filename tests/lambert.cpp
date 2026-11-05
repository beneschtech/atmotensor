#include "lambert.hpp"

#include <cmath>
#include <cstddef>

namespace atmo_test
{

Lambert::Lambert(
  double lat1_deg,
  double lat2_deg,
  double lat0_deg,
  double lon0_deg,
  double radius_m)
    : R(radius_m), lam0(lon0_deg * kPi / 180)
{
  const double p1 = lat1_deg * kPi / 180, p2 = lat2_deg * kPi / 180,
               p0 = lat0_deg * kPi / 180;
  if (p1 == p2)
  {
    n = std::sin(p1);
  }
  else
  {
    n = std::log(std::cos(p1) / std::cos(p2)) /
        std::log(std::tan(kPi / 4 + p2 / 2) / std::tan(kPi / 4 + p1 / 2));
  }
  F = std::cos(p1) * std::pow(std::tan(kPi / 4 + p1 / 2), n) / n;
  rho0 = R * F / std::pow(std::tan(kPi / 4 + p0 / 2), n);
}

double Lambert::wrap_lon_deg(double d)
{
  while (d > 180)
  {
    d -= 360;
  }
  while (d < -180)
  {
    d += 360;
  }
  return d;
}

void Lambert::inverse(double x_m, double y_m, double& lat_deg, double& lon_deg)
  const
{
  const double dy = rho0 - y_m;
  const double rho = std::sqrt(x_m * x_m + dy * dy);
  const double theta = std::atan2(x_m, dy);
  lat_deg =
    (2 * std::atan(std::pow(R * F / rho, 1.0 / n)) - kPi / 2) * 180 / kPi;
  lon_deg = wrap_lon_deg((lam0 + theta / n) * 180 / kPi);
}

void Lambert::forward(double lat_deg, double lon_deg, double& x_m, double& y_m)
  const
{
  const double rho =
    R * F / std::pow(std::tan(kPi / 4 + lat_deg * kPi / 360), n);
  const double theta = n * wrap_lon_deg(lon_deg - lam0 * 180 / kPi) * kPi / 180;
  x_m = rho * std::sin(theta);
  y_m = rho0 - rho * std::cos(theta);
}

std::vector<std::pair<double, double>>
lambert_grid(const Lambert& L, int nx, int ny, double dx_m)
{
  std::vector<std::pair<double, double>> out;
  out.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
  const double x0 = -(nx - 1) * 0.5 * dx_m;
  const double y0 = -(ny - 1) * 0.5 * dx_m;
  for (int j = 0; j < ny; ++j)
  {
    for (int i = 0; i < nx; ++i)
    {
      double lat, lon;
      L.inverse(x0 + i * dx_m, y0 + j * dx_m, lat, lon);
      out.emplace_back(lat, lon);
    }
  }
  return out;
}

}  // namespace atmo_test
