#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "atmo/crc32.hpp"
#include "atmo/geo.hpp"
#include "atmo/grid.hpp"
#include "atmo/gridstore.hpp"
#include "check.hpp"
#include "lambert.hpp"

using namespace atmo;
namespace fs = std::filesystem;

namespace
{

std::vector<std::uint8_t> bytes(const std::string& s)
{
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

// Lambert test grid (CONUS-ish, 3 km), centred on the projection origin.
struct LambertGrid
{
  atmo_test::Lambert L{38.5, 38.5, 38.5, -97.5, 6371229.0};
  int nx, ny;
  double dx;
  GridIdentity id;
  GridData data;
  LambertGrid(int nx_, int ny_, double dx_ = 3000.0) : nx(nx_), ny(ny_), dx(dx_)
  {
    id.canon = bytes("lambert 38.5/38.5/38.5/-97.5 test");
    id.ni = static_cast<std::uint32_t>(nx);
    id.nj = static_cast<std::uint32_t>(ny);
    for (auto& p : atmo_test::lambert_grid(L, nx, ny, dx))
    {
      data.lat_deg.push_back(p.first);
      data.lon_deg.push_back(p.second);
    }
  }
  double x_of(double fi) const { return -(nx - 1) * 0.5 * dx + fi * dx; }
  double y_of(double fj) const { return -(ny - 1) * 0.5 * dx + fj * dx; }
};

// Global regular lat/lon grid: lon 0..359 (periodic), lat -90..90.
struct GlobalLatLon
{
  GridIdentity id;
  GridData data;
  GlobalLatLon()
  {
    id.canon = bytes("latlon 1deg global test");
    id.ni = 360;
    id.nj = 181;
    id.periodic_i = true;
    for (int j = 0; j < 181; ++j)
    {
      for (int i = 0; i < 360; ++i)
      {
        data.lat_deg.push_back(-90.0 + j);
        data.lon_deg.push_back(static_cast<double>(i));
      }
    }
  }
};

struct TempDir
{
  fs::path path;
  TempDir()
  {
    std::string tmpl = (fs::temp_directory_path() / "atmo_gs_XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    path = ::mkdtemp(buf.data());
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

}  // namespace

TEST(crc32_known_vector)
{
  CHECK(crc32("123456789", 9) == 0xCBF43926u);
  CHECK(crc32("", 0) == 0u);
  // Piecewise == whole.
  const std::uint32_t part = crc32("12345", 5);
  CHECK(crc32("6789", 4, part) == 0xCBF43926u);
}

TEST(identity_key)
{
  GridIdentity a;
  a.canon = bytes("abc");
  a.ni = 10;
  a.nj = 20;
  GridIdentity b = a;
  CHECK(a.key() == b.key());
  CHECK(a == b);
  b.canon[1] ^= 1;
  CHECK(a.key() != b.key());
  b = a;
  b.ni = 11;
  CHECK(a.key() != b.key());
  b = a;
  b.nj = 21;
  CHECK(a.key() != b.key());
  b = a;
  b.periodic_i = true;
  CHECK(a.key() != b.key());
  // Moving a byte between canon and dimensions must not alias.
  GridIdentity c = a, d = a;
  c.canon = bytes("ab");
  d.canon = bytes("abc");
  CHECK(c.key() != d.key());
  CHECK(key_hex(0x0123456789abcdefULL) == "0123456789abcdef");
  CHECK(key_hex(1) == "0000000000000001");
}

TEST(identity_and_data_validation)
{
  GridIdentity id;
  id.canon = bytes("x");
  id.ni = 1;
  id.nj = 5;
  bool threw = false;
  try
  {
    id.validate();
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  CHECK(threw);
  id.ni = 3;
  id.nj = 3;
  GridData d;
  d.lat_deg.assign(9, 0.0);
  d.lon_deg.assign(8, 0.0);  // wrong size
  threw = false;
  try
  {
    Grid::create(id, d);
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  CHECK(threw);
  d.lon_deg.assign(9, 0.0);
  d.lat_deg[4] = 91.0;  // not a latitude
  threw = false;
  try
  {
    Grid::create(id, d);
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  CHECK(threw);
  d.lat_deg[4] = std::nan("");
  threw = false;
  try
  {
    Grid::create(id, d);
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  CHECK(threw);
  d.lat_deg[4] = 0.0;
  CHECK(Grid::create(id, d) != nullptr);
}

TEST(rotation_matches_lambert_convergence)
{
  LambertGrid g(300, 200);
  auto grid = Grid::create(g.id, g.data);
  const double n = g.L.n;
  double max_err = 0;
  for (int j = 0; j < g.ny; j += 7)
  {
    for (int i = 0; i < g.nx; i += 7)
    {
      const std::size_t k = grid->index(
        static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
      // Grid +x is the cone-rotated east: alpha = -n * (lon - lon0).
      const double dlon = (g.data.lon_deg[k] + 97.5) * kDegToRad;
      max_err = std::fmax(max_err, std::fabs(grid->rot_rad()[k] - (-n * dlon)));
    }
  }
  CHECK(max_err < 2e-3);
  // Zero at the central meridian, signs opposite on either side.
  const std::size_t west = grid->index(20, 100), east = grid->index(280, 100);
  CHECK(grid->rot_rad()[west] > 0 && grid->rot_rad()[east] < 0);
}

TEST(rotation_zero_on_latlon_grid_and_helpers)
{
  GlobalLatLon g;
  auto grid = Grid::create(g.id, g.data);
  for (int j = 1; j < 180; ++j)
  {
    for (int i = 0; i < 360; i += 13)
    {
      CHECK_NEAR(
        grid->rot_rad()[grid->index(
          static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j))],
        0.0, 1e-6);
    }
  }
  // Rotation helpers: magnitude preserved, round trip exact, 90 deg case.
  double e, nrt;
  rotate_to_earth(kPi / 2, 10.0, 0.0, e, nrt);  // +i points north
  CHECK_NEAR(e, 0.0, 1e-12);
  CHECK_NEAR(nrt, 10.0, 1e-12);
  rotate_to_earth(0.3, 3.0, -4.0, e, nrt);
  CHECK_NEAR(std::hypot(e, nrt), 5.0, 1e-12);
  double u, v;
  rotate_to_grid(0.3, e, nrt, u, v);
  CHECK_NEAR(u, 3.0, 1e-12);
  CHECK_NEAR(v, -4.0, 1e-12);
}

TEST(locate_recovers_lambert_cells)
{
  LambertGrid g(300, 200);
  auto grid = Grid::create(g.id, g.data);
  std::mt19937_64 rng(31337);
  std::uniform_int_distribution<int> ui(0, g.nx - 2), uj(0, g.ny - 2);
  std::uniform_real_distribution<double> uf(0.02, 0.98);
  double worst = 0;
  for (int t = 0; t < 1500; ++t)
  {
    const int ci = ui(rng), cj = uj(rng);
    const double fx = uf(rng), fy = uf(rng);
    double lat, lon;
    g.L.inverse(g.x_of(ci + fx), g.y_of(cj + fy), lat, lon);
    auto c = grid->locate(lat, lon);
    CHECK(c.has_value());
    if (!c)
      continue;
    CHECK(c->i == static_cast<std::uint32_t>(ci));
    CHECK(c->j == static_cast<std::uint32_t>(cj));
    worst =
      std::fmax(worst, std::fmax(std::fabs(c->fx - fx), std::fabs(c->fy - fy)));
  }
  CHECK(worst < 2e-3);
}

TEST(locate_edges_corners_and_outside)
{
  LambertGrid g(300, 200);
  auto grid = Grid::create(g.id, g.data);
  double lat, lon;
  // Exactly on interior grid points -> that point, fractions 0.
  for (auto ij : {std::pair<int, int>{10, 10}, {150, 100}, {298, 198}})
  {
    auto c = grid->locate(
      g.data.lat_deg[grid->index(ij.first, ij.second)],
      g.data.lon_deg[grid->index(ij.first, ij.second)]);
    CHECK(c.has_value());
    if (c)
    {
      CHECK(
        c->i == static_cast<std::uint32_t>(ij.first) &&
        c->j == static_cast<std::uint32_t>(ij.second));
      CHECK(c->fx == 0.0 && c->fy == 0.0);
    }
  }
  // Last column / row points report the last cell with fraction 1.
  {
    const std::size_t k = grid->index(299, 199);
    auto c = grid->locate(g.data.lat_deg[k], g.data.lon_deg[k]);
    CHECK(c.has_value());
    if (c)
      CHECK(c->i == 298 && c->j == 198 && c->fx == 1.0 && c->fy == 1.0);
  }
  // On an edge between points (not a grid point): just inside the boundary.
  g.L.inverse(g.x_of(299.0), g.y_of(100.5), lat, lon);
  auto e = grid->locate(lat, lon);
  CHECK(e.has_value());
  if (e)
    CHECK(
      e->i == 298 && e->j == 100 && e->fx > 0.999 &&
      std::fabs(e->fy - 0.5) < 2e-3);
  // Outside: one percent of a cell beyond each edge, far away, other
  // hemisphere.
  g.L.inverse(g.x_of(299.01), g.y_of(100.0), lat, lon);
  CHECK(!grid->locate(lat, lon).has_value());
  g.L.inverse(g.x_of(-0.01), g.y_of(100.5), lat, lon);
  CHECK(!grid->locate(lat, lon).has_value());
  g.L.inverse(g.x_of(100.5), g.y_of(199.01), lat, lon);
  CHECK(!grid->locate(lat, lon).has_value());
  g.L.inverse(g.x_of(100.5), g.y_of(-0.01), lat, lon);
  CHECK(!grid->locate(lat, lon).has_value());
  CHECK(!grid->locate(0.0, 0.0).has_value());
  CHECK(!grid->locate(-40.0, 120.0).has_value());
  CHECK(!grid->locate(std::nan(""), 10.0).has_value());
}

TEST(locate_global_periodic_latlon)
{
  GlobalLatLon g;
  auto grid = Grid::create(g.id, g.data);
  // Across the seam: lon 359.5 is half way between column 359 and column 0.
  for (double lon : {359.5, -0.5, 719.5})
  {
    auto c = grid->locate(10.25, lon);
    CHECK(c.has_value());
    if (c)
    {
      CHECK(c->i == 359 && c->j == 100);
      CHECK_NEAR(c->fx, 0.5, 2e-3);  // flat-cell approximation, see grid.hpp
      CHECK_NEAR(c->fy, 0.25, 2e-3);
    }
  }
  auto c0 = grid->locate(10.25, 0.25);
  CHECK(c0.has_value());
  if (c0)
    CHECK(c0->i == 0 && c0->j == 100 && std::fabs(c0->fx - 0.25) < 2e-3);
  // Exactly on a column between rows: either neighbour cell is acceptable.
  auto c180 = grid->locate(10.5, 180.0);
  CHECK(c180.has_value());
  if (c180)
  {
    const bool left = (c180->i == 179 && c180->fx > 1 - 1e-6);
    const bool right =
      (c180->i == 180 && c180->fx < 1e-6);  // on a column: exact
    CHECK(left || right);
    CHECK_NEAR(c180->fy, 0.5, 2e-3);
  }
  // Poles: on the pole row every longitude is the same point.
  auto np = grid->locate(90.0, 33.0);
  CHECK(np.has_value());
  if (np)
    CHECK(np->j == 179 && np->fy == 1.0);
  auto sp = grid->locate(-90.0, 200.0);
  CHECK(sp.has_value());
  if (sp)
    CHECK(sp->j == 0 && sp->fy == 0.0);
  // Near the pole the cells are wedges; fractions are still sensible.
  auto nr = grid->locate(89.5, 10.25);
  CHECK(nr.has_value());
  if (nr)
  {
    CHECK(nr->i == 10 && nr->j == 179);
    CHECK_NEAR(nr->fx, 0.25, 1e-2);
    CHECK_NEAR(nr->fy, 0.5, 1e-2);
  }
  // Random interior points: reconstruct lat/lon from (i, j, fx, fy).
  std::mt19937_64 rng(8);
  std::uniform_real_distribution<double> ulat(-88, 88), ulon(0, 360);
  for (int t = 0; t < 2000; ++t)
  {
    const double lat = ulat(rng), lon = ulon(rng);
    auto c = grid->locate(lat, lon);
    CHECK(c.has_value());
    if (!c)
      continue;
    CHECK_NEAR(-90.0 + c->j + c->fy, lat, 2e-3);
    double lo = c->i + c->fx;  // may exceed 360 only through wrap
    double d = std::fmod(lo - lon, 360.0);
    if (d > 180)
      d -= 360;
    if (d < -180)
      d += 360;
    CHECK(std::fabs(d) < 4e-3);
  }
}

TEST(nearest_matches_brute_force)
{
  LambertGrid g(120, 90);
  auto grid = Grid::create(g.id, g.data);
  std::mt19937_64 rng(4);
  std::uniform_real_distribution<double> ux(-1.7e5, 1.7e5), uy(-1.3e5, 1.3e5);
  for (int t = 0; t < 100; ++t)
  {
    double lat, lon;
    g.L.inverse(ux(rng), uy(rng), lat, lon);
    auto nn = grid->nearest(lat, lon);
    CHECK(nn.has_value());
    if (!nn)
      continue;
    double best = 1e30;
    for (std::size_t k = 0; k < grid->size(); ++k)
    {
      best = std::fmin(
        best, great_circle_km(lat, lon, g.data.lat_deg[k], g.data.lon_deg[k]));
    }
    CHECK_NEAR(
      great_circle_km(lat, lon, g.data.lat_deg[*nn], g.data.lon_deg[*nn]), best,
      1e-6);
  }
}

TEST(store_roundtrip_idempotent_and_shared)
{
  TempDir td;
  LambertGrid g(200, 150);
  GridStore store(td.path);
  CHECK(store.list().empty());
  CHECK(store.get(g.id.key()) == nullptr);
  CHECK(!store.contains(g.id.key()));

  auto a = store.put(g.id, g.data);
  CHECK(a != nullptr);
  CHECK(store.contains(g.id.key()));
  CHECK(store.list().size() == 1 && store.list()[0] == g.id.key());
  const auto path = store.path_for(g.id.key());
  CHECK(fs::exists(path));
  CHECK(fs::file_size(path) % 4096 == 0);
  const auto t1 = fs::last_write_time(path);

  // Same grid again: no rewrite, same shared object while alive.
  auto b = store.put(g.id, g.data);
  CHECK(a.get() == b.get());
  CHECK(store.get(g.id.key()).get() == a.get());
  CHECK(fs::last_write_time(path) == t1);

  // Contents identical, rotation derived and stored.
  const std::size_t n = g.id.points();
  CHECK(
    std::memcmp(a->lat_deg(), g.data.lat_deg.data(), n * sizeof(double)) == 0);
  CHECK(
    std::memcmp(a->lon_deg(), g.data.lon_deg.data(), n * sizeof(double)) == 0);
  auto mem = Grid::create(g.id, g.data);
  CHECK(std::memcmp(a->rot_rad(), mem->rot_rad(), n * sizeof(float)) == 0);
  CHECK(a->identity() == g.id);

  // A fresh store over the same directory (e.g. after a restart) reads it back
  // and behaves identically to the in-memory grid.
  GridStore store2(td.path);
  auto c = store2.get(g.id.key());
  CHECK(c != nullptr);
  if (c)
  {
    CHECK(c->identity() == g.id);
    for (int t = 0; t < 50; ++t)
    {
      double lat, lon;
      g.L.inverse(g.x_of(5.5 + 3.1 * t), g.y_of(7.25 + 2.3 * t), lat, lon);
      auto x = c->locate(lat, lon), y = mem->locate(lat, lon);
      CHECK(x.has_value() && y.has_value());
      if (x && y)
        CHECK(x->i == y->i && x->j == y->j && x->fx == y->fx && x->fy == y->fy);
    }
  }

  // No temp files left behind.
  for (auto& e : fs::directory_iterator(td.path))
  {
    CHECK(e.path().extension() == ".grid");
  }

  // A second, different grid coexists.
  GlobalLatLon gl;
  auto d = store.put(gl.id, gl.data);
  CHECK(d != nullptr);
  CHECK(store.list().size() == 2);
}

TEST(store_detects_corruption_and_truncation)
{
  LambertGrid g(100, 80);
  const GridKey key = g.id.key();
  auto flip = [](const fs::path& p, std::size_t off)
  {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    char ch;
    f.seekg(static_cast<std::streamoff>(off));
    f.get(ch);
    ch = static_cast<char>(ch ^ 0x40);
    f.seekp(static_cast<std::streamoff>(off));
    f.put(ch);
  };
  auto throws_on_get = [&](const fs::path& dir)
  {
    GridStore s(dir);
    try
    {
      s.get(key);
    }
    catch (const std::runtime_error&)
    {
      return true;
    }
    return false;
  };
  // Offsets: header 0.., canon 128.., lat 4096.., lon/rot after.
  const std::size_t n = g.id.points();
  const std::size_t lon_off = (4096 + n * 8 + 4095) / 4096 * 4096;
  const std::size_t rot_off = (lon_off + n * 8 + 4095) / 4096 * 4096;
  struct Case
  {
    const char* what;
    std::size_t off;
  };
  for (Case c :
       {Case{"header", 20}, Case{"canon", 130}, Case{"lat", 4096 + 100},
        Case{"lon", lon_off + 100}, Case{"rot", rot_off + 20}})
  {
    TempDir td;
    GridStore s(td.path);
    s.put(g.id, g.data);
    flip(s.path_for(key), c.off);
    const bool ok = throws_on_get(td.path);
    if (!ok)
      tst::fail(
        __FILE__, __LINE__,
        std::string("corruption in ") + c.what + " not detected");
  }
  {  // truncation
    TempDir td;
    GridStore s(td.path);
    s.put(g.id, g.data);
    fs::resize_file(s.path_for(key), 4096 + 10);
    CHECK(throws_on_get(td.path));
    fs::resize_file(s.path_for(key), 100);
    CHECK(throws_on_get(td.path));
  }
  {  // file renamed to the wrong key
    TempDir td;
    GridStore s(td.path);
    s.put(g.id, g.data);
    fs::rename(s.path_for(key), s.path_for(key ^ 1));
    GridStore s2(td.path);
    bool threw = false;
    try
    {
      s2.get(key ^ 1);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    CHECK(threw);
  }
  {  // put() over a mismatching existing file is refused, not overwritten
    TempDir td;
    GridStore s(td.path);
    s.put(g.id, g.data);
    GridData other = g.data;
    other.lat_deg[5] += 0.001;
    bool threw = false;
    try
    {
      GridStore(td.path).put(g.id, other);
    }
    catch (const std::runtime_error&)
    {
      threw = true;
    }
    CHECK(threw);
  }
}

int main()
{
  return tst::run_all();
}
