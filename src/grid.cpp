#include "atmo/grid.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "atmo/geo.hpp"

namespace atmo
{
namespace
{

struct V2
{
  double x, y;
};

double cross(V2 a, V2 b)
{
  return a.x * b.y - a.y * b.x;
}

// Solve p = a + e*u + f*v + g*u*v for p = (0,0), where the quad is
// a(0,0) b(1,0) c(1,1) d(0,1).  Accepts (u,v) within [-tol, 1+tol].
bool inv_bilinear(V2 a, V2 b, V2 c, V2 d, double tol, double& u, double& v)
{
  const V2 e{b.x - a.x, b.y - a.y};
  const V2 f{d.x - a.x, d.y - a.y};
  const V2 g{a.x - b.x + c.x - d.x, a.y - b.y + c.y - d.y};
  const V2 h{-a.x, -a.y};
  const double k2 = cross(g, f);
  const double k1 = cross(e, f) + cross(h, g);
  const double k0 = cross(h, e);
  const double scale = std::hypot(e.x, e.y) * std::hypot(f.x, f.y);
  if (!(scale > 0))
  {
    return false;
  }

  double vs[2];
  int nv = 0;
  if (std::fabs(k2) < 1e-9 * scale)
  {
    if (k1 == 0)
    {
      return false;
    }
    vs[nv++] = -k0 / k1;
  }
  else
  {
    double disc = k1 * k1 - 4 * k2 * k0;
    if (disc < 0)
    {
      if (disc < -1e-9 * k1 * k1 - 1e-18)
      {
        return false;
      }
      disc = 0;
    }
    const double sq = std::sqrt(disc);
    const double q = -0.5 * (k1 + (k1 >= 0 ? sq : -sq));  // stable form
    vs[nv++] = q / k2;
    if (q != 0)
    {
      vs[nv++] = k0 / q;
    }
  }
  for (int r = 0; r < nv; ++r)
  {
    const double vv = vs[r];
    if (!(vv >= -tol && vv <= 1 + tol))
    {
      continue;
    }
    const double dx = e.x + g.x * vv, dy = e.y + g.y * vv;
    double uu;
    if (std::fabs(dx) >= std::fabs(dy))
    {
      if (dx == 0)
      {
        continue;
      }
      uu = (h.x - f.x * vv) / dx;
    }
    else
    {
      uu = (h.y - f.y * vv) / dy;
    }
    if (!std::isfinite(uu) || !(uu >= -tol && uu <= 1 + tol))
    {
      continue;
    }
    // Verify: a degenerate quad (two coincident corners, e.g. at a pole) has
    // a spurious quadratic root at the shared corner; it fails this check.
    const double rx = a.x + e.x * uu + f.x * vv + g.x * uu * vv;
    const double ry = a.y + e.y * uu + f.y * vv + g.y * uu * vv;
    const double emax = std::fmax(std::hypot(e.x, e.y), std::hypot(f.x, f.y));
    if (!(std::hypot(rx, ry) <= 1e-6 * emax))
    {
      continue;
    }
    u = std::fmin(1.0, std::fmax(0.0, uu));
    v = std::fmin(1.0, std::fmax(0.0, vv));
    return true;
  }
  return false;
}

}  // namespace

std::uint64_t fnv1a64(const void* data, std::size_t n, std::uint64_t h)
{
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i)
  {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

std::string key_hex(GridKey k)
{
  static const char* d = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i)
  {
    s[static_cast<std::size_t>(i)] = d[k & 0xF];
    k >>= 4;
  }
  return s;
}

GridKey GridIdentity::key() const
{
  std::uint64_t h = fnv1a64("ATGRID1", 7);
  auto feed32 = [&h](std::uint32_t v)
  {
    const std::uint8_t b[4] = {
      static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
      static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    h = fnv1a64(b, 4, h);
  };
  feed32(ni);
  feed32(nj);
  feed32(periodic_i ? 1u : 0u);
  feed32(static_cast<std::uint32_t>(canon.size()));
  return fnv1a64(canon.data(), canon.size(), h);
}

void GridIdentity::validate() const
{
  if (ni < 2 || nj < 2)
  {
    throw std::invalid_argument("grid needs ni, nj >= 2");
  }
  if (canon.size() > kMaxCanon)
  {
    throw std::invalid_argument("grid canon too long");
  }
  if (static_cast<std::uint64_t>(ni) * nj > 0xFFFFFFFFull)
  {
    throw std::invalid_argument("grid too large");
  }
}

std::size_t GridIdentity::points() const
{
  return static_cast<std::size_t>(ni) * nj;
}

std::vector<float> derive_rotation(
  const std::vector<double>& lat_deg,
  const std::vector<double>& lon_deg,
  std::uint32_t ni,
  std::uint32_t nj,
  bool periodic_i)
{
  std::vector<float> rot(static_cast<std::size_t>(ni) * nj, 0.0f);
  auto unit = [&](std::size_t idx)
  {
    const double la = lat_deg[idx] * kDegToRad;
    const double lo = lon_deg[idx] * kDegToRad;
    return std::array<double, 3>{
      std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
  };
  const auto ni64 = static_cast<std::int64_t>(ni);
  for (std::uint32_t j = 0; j < nj; ++j)
  {
    const std::size_t row = static_cast<std::size_t>(j) * ni;
    for (std::uint32_t i = 0; i < ni; ++i)
    {
      const std::size_t idx = row + i;
      std::int64_t ia = static_cast<std::int64_t>(i) - 1;
      std::int64_t ib = static_cast<std::int64_t>(i) + 1;
      if (periodic_i)
      {
        if (ia < 0)
        {
          ia += ni64;
        }
        if (ib >= ni64)
        {
          ib -= ni64;
        }
      }
      else
      {
        if (ia < 0)
        {
          ia = i;  // one-sided
        }
        if (ib >= ni64)
        {
          ib = i;
        }
      }
      const auto pa = unit(row + static_cast<std::size_t>(ia));
      const auto pb = unit(row + static_cast<std::size_t>(ib));
      const std::array<double, 3> t = {
        pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
      const double la = lat_deg[idx] * kDegToRad;
      const double lo = lon_deg[idx] * kDegToRad;
      const double e[3] = {-std::sin(lo), std::cos(lo), 0.0};
      const double n[3] = {
        -std::sin(la) * std::cos(lo), -std::sin(la) * std::sin(lo),
        std::cos(la)};
      const double te = t[0] * e[0] + t[1] * e[1] + t[2] * e[2];
      const double tn = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
      if (std::hypot(te, tn) < 1e-12)
      {
        continue;  // degenerate: leave 0
      }
      rot[idx] = static_cast<float>(std::atan2(tn, te));
    }
  }
  return rot;
}

void rotate_to_earth(
  double alpha_rad,
  double u,
  double v,
  double& east,
  double& north)
{
  const double c = std::cos(alpha_rad), s = std::sin(alpha_rad);
  east = u * c - v * s;
  north = u * s + v * c;
}

void rotate_to_grid(
  double alpha_rad,
  double east,
  double north,
  double& u,
  double& v)
{
  const double c = std::cos(alpha_rad), s = std::sin(alpha_rad);
  u = east * c + north * s;
  v = -east * s + north * c;
}

std::shared_ptr<const Grid> Grid::create(GridIdentity id, GridData data)
{
  id.validate();
  const std::size_t n = id.points();
  if (data.lat_deg.size() != n || data.lon_deg.size() != n)
  {
    throw std::invalid_argument("grid arrays do not match ni*nj");
  }
  for (std::size_t k = 0; k < n; ++k)
  {
    if (
      !std::isfinite(data.lat_deg[k]) || !std::isfinite(data.lon_deg[k]) ||
      std::fabs(data.lat_deg[k]) > 90.0 + 1e-9)
    {
      throw std::invalid_argument("grid has invalid lat/lon");
    }
  }
  if (data.rot_rad.empty())
  {
    data.rot_rad =
      derive_rotation(data.lat_deg, data.lon_deg, id.ni, id.nj, id.periodic_i);
  }
  else if (data.rot_rad.size() != n)
  {
    throw std::invalid_argument("rotation array does not match ni*nj");
  }
  auto owned = std::make_shared<GridData>(std::move(data));
  const double* la = owned->lat_deg.data();
  const double* lo = owned->lon_deg.data();
  const float* ro = owned->rot_rad.data();
  return adopt(std::move(id), la, lo, ro, std::move(owned));
}

std::shared_ptr<const Grid> Grid::adopt(
  GridIdentity id,
  const double* lat_deg,
  const double* lon_deg,
  const float* rot_rad,
  std::shared_ptr<const void> keepalive)
{
  id.validate();
  return std::shared_ptr<const Grid>(
    new Grid(std::move(id), lat_deg, lon_deg, rot_rad, std::move(keepalive)));
}

Grid::Grid(
  GridIdentity id,
  const double* lat_deg,
  const double* lon_deg,
  const float* rot_rad,
  std::shared_ptr<const void> keep)
    : id_(std::move(id)),
      key_(id_.key()),
      lat_(lat_deg),
      lon_(lon_deg),
      rot_(rot_rad),
      keep_(std::move(keep))
{
}

std::size_t Grid::size() const
{
  return id_.points();
}

std::size_t Grid::index(std::uint32_t i, std::uint32_t j) const
{
  return static_cast<std::size_t>(j) * id_.ni + i;
}

std::optional<std::uint32_t> Grid::nearest(double lat_deg, double lon_deg) const
{
  if (!std::isfinite(lat_deg) || !std::isfinite(lon_deg))
  {
    return std::nullopt;
  }
  auto nn = tree().nearest(embed<double>(lat_deg, lon_deg, 0, 0, Embedding{}));
  if (!nn)
  {
    return std::nullopt;
  }
  return nn->id;
}

std::optional<Grid::Cell> Grid::locate(double lat_deg, double lon_deg) const
{
  if (!std::isfinite(lat_deg) || !std::isfinite(lon_deg))
  {
    return std::nullopt;
  }
  const Embedding emb;
  const auto q = embed<double>(lat_deg, lon_deg, 0, 0, emb);
  const auto nn = tree().nearest(q);
  if (!nn)
  {
    return std::nullopt;
  }
  const std::int64_t i0 = nn->id % id_.ni;
  const std::int64_t j0 = nn->id / id_.ni;

  // Query sits on a grid point (also covers degenerate pole cells).
  if (nn->d2 <= kSnapKm2)
  {
    Cell c{
      static_cast<std::uint32_t>(i0), static_cast<std::uint32_t>(j0), 0, 0};
    if (!id_.periodic_i && i0 == static_cast<std::int64_t>(id_.ni) - 1)
    {
      c.i = id_.ni - 2;
      c.fx = 1;
    }
    if (j0 == static_cast<std::int64_t>(id_.nj) - 1)
    {
      c.j = id_.nj - 2;
      c.fy = 1;
    }
    return c;
  }

  const double la = lat_deg * kDegToRad, lo = lon_deg * kDegToRad;
  const double east[3] = {-std::sin(lo), std::cos(lo), 0.0};
  const double north[3] = {
    -std::sin(la) * std::cos(lo), -std::sin(la) * std::sin(lo), std::cos(la)};
  auto corner = [&](std::size_t idx)
  {
    const auto p = embed<double>(lat_[idx], lon_[idx], 0, 0, emb);
    const double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return V2{
      dx * east[0] + dy * east[1] + dz * east[2],
      dx * north[0] + dy * north[1] + dz * north[2]};
  };
  auto try_cell = [&](std::int64_t ci, std::int64_t cj) -> std::optional<Cell>
  {
    const std::int64_t ni = id_.ni;
    if (id_.periodic_i)
    {
      ci = ((ci % ni) + ni) % ni;
    }
    else if (ci < 0 || ci > ni - 2)
    {
      return std::nullopt;
    }
    if (cj < 0 || cj > static_cast<std::int64_t>(id_.nj) - 2)
    {
      return std::nullopt;
    }
    const std::int64_t i1 = (ci + 1) % ni;  // only wraps when periodic
    const std::size_t row0 = static_cast<std::size_t>(cj) * id_.ni;
    const std::size_t row1 = row0 + id_.ni;
    double u, v;
    if (!inv_bilinear(
          corner(row0 + static_cast<std::size_t>(ci)),
          corner(row0 + static_cast<std::size_t>(i1)),
          corner(row1 + static_cast<std::size_t>(i1)),
          corner(row1 + static_cast<std::size_t>(ci)), kCellTol, u, v))
    {
      return std::nullopt;
    }
    return Cell{
      static_cast<std::uint32_t>(ci), static_cast<std::uint32_t>(cj), u, v};
  };

  // The four cells touching the nearest point first, then the ring around
  // them (covers strongly skewed cells).  Fixed order => deterministic when
  // the query lies exactly on a shared edge.
  for (int pass = 0; pass < 2; ++pass)
  {
    for (int dj = -2; dj <= 1; ++dj)
    {
      for (int di = -2; di <= 1; ++di)
      {
        const bool inner = (di >= -1 && di <= 0 && dj >= -1 && dj <= 0);
        if (inner != (pass == 0))
        {
          continue;
        }
        if (auto c = try_cell(i0 + di, j0 + dj))
        {
          return c;
        }
      }
    }
  }
  return std::nullopt;
}

const KdTree4<double>& Grid::tree() const
{
  std::call_once(
    tree_once_,
    [this]
    {
      std::vector<KdTree4<double>::Point> pts;
      pts.reserve(size());
      const Embedding emb;
      for (std::size_t k = 0; k < size(); ++k)
      {
        pts.push_back(embed<double>(lat_[k], lon_[k], 0, 0, emb));
      }
      tree_ = std::make_unique<KdTree4<double>>(pts);
    });
  return *tree_;
}

}  // namespace atmo
