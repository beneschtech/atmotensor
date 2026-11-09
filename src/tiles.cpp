#include "atmo/tiles.h"

#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "atmo/png.h"
#include "atmo/variables.h"

namespace atmo
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::uint32_t kBlock = 16;
constexpr std::uint32_t kNodes = kTileSize / kBlock + 1;  // 17

struct LatLon
{
  double lat, lon;
};

// Pixel-space position (u, v in [0, 256], pixel centres at k + 0.5) ->
// lat/lon.
LatLon to_latlon(const TileRequest& r, double u, double v)
{
  const double n = std::ldexp(1.0, static_cast<int>(r.z));
  const double X = (r.x + u / kTileSize) / n;
  const double Y = (r.y + v / kTileSize) / n;
  return LatLon{
    std::atan(std::sinh(kPi * (1.0 - 2.0 * Y))) * 180.0 / kPi,
    X * 360.0 - 180.0};
}

// Continuous grid coordinates of a point, or nullopt off the grid.
struct GridPos
{
  double fi, fj;
};

std::optional<GridPos> locate(const Grid& g, LatLon p)
{
  auto c = g.locate(p.lat, p.lon);
  if (!c)
  {
    return std::nullopt;
  }
  return GridPos{c->i + c->fx, c->j + c->fy};
}

Grid::Cell to_cell(const Grid& g, GridPos p)
{
  const double imax = g.periodic_i() ? g.ni() : g.ni() - 1.0;
  double fi = p.fi, fj = p.fj;
  if (g.periodic_i())
  {
    fi = std::fmod(fi, static_cast<double>(g.ni()));
    if (fi < 0)
      fi += g.ni();
  }
  fi = std::fmin(std::fmax(fi, 0.0), imax);
  fj = std::fmin(std::fmax(fj, 0.0), g.nj() - 1.0);
  auto i = static_cast<std::uint32_t>(fi);
  auto j = static_cast<std::uint32_t>(fj);
  if (!g.periodic_i() && i + 1 >= g.ni())
    i = g.ni() - 2;
  if (g.periodic_i() && i >= g.ni())
    i = g.ni() - 1;
  if (j + 1 >= g.nj())
    j = g.nj() - 2;
  return Grid::Cell{i, j, fi - i, fj - j};
}

struct Box
{
  double lat0, lat1, lon0, lon1;
  bool all_lon;
};

// Lat/lon bounding box of a grid's points (cached per grid key).
Box grid_box(const Grid& g)
{
  static std::mutex mu;
  static std::map<GridKey, Box> cache;
  {
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(g.key());
    if (it != cache.end())
      return it->second;
  }
  Box b{90, -90, 180, -180, g.periodic_i()};
  for (std::size_t k = 0; k < g.size(); ++k)
  {
    double lon = std::fmod(g.lon_deg()[k] + 540.0, 360.0) - 180.0;
    b.lat0 = std::fmin(b.lat0, g.lat_deg()[k]);
    b.lat1 = std::fmax(b.lat1, g.lat_deg()[k]);
    b.lon0 = std::fmin(b.lon0, lon);
    b.lon1 = std::fmax(b.lon1, lon);
  }
  // A grid straddling the antimeridian shows up as nearly all longitudes;
  // treat it as unbounded in longitude (conservative).
  if (b.lon1 - b.lon0 > 300.0)
    b.all_lon = true;
  const double margin = 0.25;
  b.lat0 -= margin;
  b.lat1 += margin;
  b.lon0 -= margin;
  b.lon1 += margin;
  std::lock_guard<std::mutex> lk(mu);
  cache[g.key()] = b;
  return b;
}

bool intersects(const Box& g, LatLon top_left, LatLon bottom_right)
{
  if (bottom_right.lat > g.lat1 || top_left.lat < g.lat0)
    return false;
  if (g.all_lon)
    return true;
  return !(top_left.lon > g.lon1 || bottom_right.lon < g.lon0);
}

}  // namespace

namespace
{

// tile_values plus, if `on_grid` is given, which pixels lie on the grid.
std::vector<double> compute(
  const FieldStore& store,
  const TileRequest& r,
  TileResult::Status& status,
  std::vector<std::uint8_t>* on_grid)
{
  if (r.z > 24 || r.x >= (1u << r.z) || r.y >= (1u << r.z))
  {
    throw std::invalid_argument("tile: bad z/x/y");
  }
  if (r.field.level != FieldSpec::Level::fixed || !r.field.var)
  {
    throw std::invalid_argument("tile: the field needs an explicit level");
  }
  const ModelTime t{r.run_s, r.fhr};
  if (!store.grid(r.model, t))
  {
    status = TileResult::Status::no_time;
    return {};
  }
  const std::uint16_t code = r.field.var->code;
  const FieldKey key{code, r.field.level_type, r.field.level_value};
  auto view = store.view(r.model, t, key);
  if (!view)
  {
    status = TileResult::Status::no_field;
    return {};
  }
  // Earth-relative winds: rotate grid-relative u/v per pixel.
  std::shared_ptr<const FieldView> other;
  const bool wind = code == var::kWindU || code == var::kWindV;
  if (wind && (view->info().flags & kFieldGridRelative))
  {
    const FieldKey ok{
      code == var::kWindU ? var::kWindV : var::kWindU, key.level_type,
      key.level_value};
    other = store.view(r.model, t, ok);
    if (!other)
    {
      status = TileResult::Status::no_field;
      return {};
    }
  }
  const Grid& g = view->grid();
  auto value_at = [&](const Grid::Cell& c) -> double
  {
    const double v = view->sample(c);
    if (!other)
      return v;
    const double o = other->sample(c);
    const double u = code == var::kWindU ? v : o;
    const double vv = code == var::kWindU ? o : v;
    double east = 0.0, north = 0.0;
    rotate_to_earth(grid_alpha_at(g, c), u, vv, east, north);
    return code == var::kWindU ? east : north;
  };

  // Lattice of located nodes every 16 px.
  std::vector<std::optional<GridPos>> node(kNodes * kNodes);
  std::vector<LatLon> node_ll(kNodes * kNodes);
  for (std::uint32_t nv = 0; nv < kNodes; ++nv)
  {
    for (std::uint32_t nu = 0; nu < kNodes; ++nu)
    {
      const LatLon p = to_latlon(r, nu * kBlock, nv * kBlock);
      node_ll[nv * kNodes + nu] = p;
      node[nv * kNodes + nu] = locate(g, p);
    }
  }
  const Box box = grid_box(g);
  std::vector<double> out(kTileSize * kTileSize, kNaN);
  bool any = false;
  for (std::uint32_t bv = 0; bv < kTileSize / kBlock; ++bv)
  {
    for (std::uint32_t bu = 0; bu < kTileSize / kBlock; ++bu)
    {
      const auto& n00 = node[bv * kNodes + bu];
      const auto& n10 = node[bv * kNodes + bu + 1];
      const auto& n01 = node[(bv + 1) * kNodes + bu];
      const auto& n11 = node[(bv + 1) * kNodes + bu + 1];
      bool fast = n00 && n10 && n01 && n11;
      if (fast && g.periodic_i())
      {
        const double lo =
          std::fmin(std::fmin(n00->fi, n10->fi), std::fmin(n01->fi, n11->fi));
        const double hi =
          std::fmax(std::fmax(n00->fi, n10->fi), std::fmax(n01->fi, n11->fi));
        fast = hi - lo < g.ni() / 2.0;  // not across the seam
      }
      if (
        !fast && !(n00 || n10 || n01 || n11) &&
        !intersects(
          box, node_ll[bv * kNodes + bu], node_ll[(bv + 1) * kNodes + bu + 1]))
      {
        continue;  // nothing of the grid in this block
      }
      for (std::uint32_t py = 0; py < kBlock; ++py)
      {
        for (std::uint32_t px = 0; px < kBlock; ++px)
        {
          const std::uint32_t u = bu * kBlock + px, v = bv * kBlock + py;
          std::optional<GridPos> pos;
          if (fast)
          {
            const double a = (px + 0.5) / kBlock, b = (py + 0.5) / kBlock;
            pos = GridPos{
              (1 - b) * ((1 - a) * n00->fi + a * n10->fi) +
                b * ((1 - a) * n01->fi + a * n11->fi),
              (1 - b) * ((1 - a) * n00->fj + a * n10->fj) +
                b * ((1 - a) * n01->fj + a * n11->fj)};
          }
          else
          {
            pos = locate(g, to_latlon(r, u + 0.5, v + 0.5));
          }
          if (pos)
          {
            out[v * kTileSize + u] = value_at(to_cell(g, *pos));
            any = true;
            if (on_grid)
              (*on_grid)[v * kTileSize + u] = 1;
          }
        }
      }
    }
  }
  status = any ? TileResult::Status::ok : TileResult::Status::empty;
  return out;
}

}  // namespace

std::vector<double> tile_values(
  const FieldStore& store,
  const TileRequest& r,
  TileResult::Status& status)
{
  return compute(store, r, status, nullptr);
}

TileResult
render_tile(const FieldStore& store, const TileRequest& r, const Colormap& cmap)
{
  TileResult res;
  std::vector<std::uint8_t> on_grid(kTileSize * kTileSize, 0);
  const std::vector<double> v = compute(store, r, res.status, &on_grid);
  if (
    res.status == TileResult::Status::no_time ||
    res.status == TileResult::Status::no_field)
  {
    return res;
  }
  std::vector<std::uint8_t> rgba(kTileSize * kTileSize * 4, 0);
  if (res.status == TileResult::Status::ok)
  {
    for (std::size_t k = 0; k < v.size(); ++k)
    {
      // Off the grid: always transparent; on it, NaN -> cmap's nodata.
      if (on_grid[k])
      {
        const Rgba c = cmap.color(v[k]);
        std::copy(c.begin(), c.end(), rgba.begin() + 4 * k);
      }
    }
  }
  res.png = encode_png_rgba(kTileSize, kTileSize, rgba.data());
  return res;
}

}  // namespace atmo
