#include "atmo/geo.h"

namespace atmo
{

double great_circle_km(
  double lat1_deg,
  double lon1_deg,
  double lat2_deg,
  double lon2_deg,
  double radius_km)
{
  const double p1 = lat1_deg * kDegToRad;
  const double p2 = lat2_deg * kDegToRad;
  const double dp = p2 - p1;
  const double dl = (lon2_deg - lon1_deg) * kDegToRad;
  const double s1 = std::sin(dp / 2);
  const double s2 = std::sin(dl / 2);
  const double a = s1 * s1 + std::cos(p1) * std::cos(p2) * s2 * s2;
  return 2.0 * radius_km * std::asin(std::fmin(1.0, std::sqrt(a)));
}

}  // namespace atmo
