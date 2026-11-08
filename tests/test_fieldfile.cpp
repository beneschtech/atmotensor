// Field files: acceptance tests 1-4 of docs/design/03-field-tiles.md, plus
// writer validation and snapshot integration.
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/fieldfile.h"
#include "atmo/grid.h"
#include "atmo/gridstore.h"
#include "atmo/manifest.h"
#include "atmo/snapshot.h"
#include "atmo/source.h"
#include "check.h"
#include "lambert.h"

using namespace atmo;
namespace fs = std::filesystem;

namespace
{

struct TempDir
{
  fs::path p;
  TempDir()
  {
    p = fs::temp_directory_path() /
        ("atmo_test_fieldfile_" + std::to_string(::getpid()) + "_" +
         std::to_string(std::random_device{}()));
    fs::create_directories(p);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

template <class E, class F>
bool throws(F&& f)
{
  try
  {
    f();
  }
  catch (const E&)
  {
    return true;
  }
  catch (...)
  {
    return false;
  }
  return false;
}

constexpr double kPi = 3.14159265358979323846;
constexpr std::int64_t kRun = 1762333200;  // 2025-11-05 09:00Z

double analytic(double lat_deg, double lon_deg)
{
  const double la = lat_deg * kPi / 180.0, lo = lon_deg * kPi / 180.0;
  return 10.0 * std::sin(2.0 * la) * std::cos(3.0 * lo) + 0.5 * la;
}

std::vector<std::uint8_t> bytes(const std::string& s)
{
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

// CONUS-ish Lambert grid, 3 km, 600 x 400 (3 x 2 tiles of 256).
struct LambertFixture
{
  atmo_test::Lambert L{38.5, 38.5, 38.5, -97.5, 6371229.0};
  int nx = 600, ny = 400;
  double dx = 3000.0;
  std::shared_ptr<const Grid> grid;
  LambertFixture()
  {
    GridIdentity id;
    id.canon = bytes("lambert fieldfile test");
    id.ni = static_cast<std::uint32_t>(nx);
    id.nj = static_cast<std::uint32_t>(ny);
    GridData d;
    for (auto& p : atmo_test::lambert_grid(L, nx, ny, dx))
    {
      d.lat_deg.push_back(p.first);
      d.lon_deg.push_back(p.second);
    }
    grid = Grid::create(id, d);
  }
  void latlon(double fi, double fj, double& lat, double& lon) const
  {
    L.inverse(
      -(nx - 1) * 0.5 * dx + fi * dx, -(ny - 1) * 0.5 * dx + fj * dx, lat, lon);
  }
};

// Global 1 degree lat/lon, periodic in i.
std::shared_ptr<const Grid> global_grid()
{
  GridIdentity id;
  id.canon = bytes("latlon 1deg fieldfile test");
  id.ni = 360;
  id.nj = 181;
  id.periodic_i = true;
  GridData d;
  for (int j = 0; j < 181; ++j)
  {
    for (int i = 0; i < 360; ++i)
    {
      d.lat_deg.push_back(-90.0 + j);
      d.lon_deg.push_back(static_cast<double>(i));
    }
  }
  return Grid::create(id, d);
}

std::vector<float> analytic_field(const Grid& g)
{
  std::vector<float> v(g.size());
  for (std::size_t k = 0; k < g.size(); ++k)
    v[k] = static_cast<float>(analytic(g.lat_deg()[k], g.lon_deg()[k]));
  return v;
}

FieldInfo info(std::uint16_t param, float level = 0.0f, std::uint8_t lt = 1)
{
  FieldInfo f;
  f.param_id = param;
  f.level_type = lt;
  f.level_value = level;
  return f;
}

// Expected tile, computed independently of the writer.
std::vector<float> expected_tile(
  const Grid& g,
  const std::vector<float>& v,
  std::uint32_t T,
  std::uint32_t H,
  std::uint32_t tx,
  std::uint32_t ty)
{
  const long w = static_cast<long>(T + 2 * H);
  const long ni = g.ni(), nj = g.nj();
  std::vector<float> out(
    static_cast<std::size_t>(w * w), std::numeric_limits<float>::quiet_NaN());
  for (long lj = 0; lj < w; ++lj)
  {
    for (long li = 0; li < w; ++li)
    {
      long i = static_cast<long>(tx * T) + li - static_cast<long>(H);
      const long j = static_cast<long>(ty * T) + lj - static_cast<long>(H);
      if (g.periodic_i())
        i = ((i % ni) + ni) % ni;
      if (i >= 0 && i < ni && j >= 0 && j < nj)
        out[static_cast<std::size_t>(lj * w + li)] =
          v[static_cast<std::size_t>(j * ni + i)];
    }
  }
  return out;
}

bool bit_equal(std::span<const float> a, const std::vector<float>& b)
{
  return a.size() == b.size() &&
         std::memcmp(a.data(), b.data(), b.size() * sizeof(float)) == 0;
}

void flip_byte(const fs::path& p, std::uint64_t off)
{
  std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
  f.seekg(static_cast<std::streamoff>(off));
  char c = 0;
  f.read(&c, 1);
  c = static_cast<char>(c ^ 0x5a);
  f.seekp(static_cast<std::streamoff>(off));
  f.write(&c, 1);
}

std::uint64_t slot_bytes(std::uint32_t T, std::uint32_t H)
{
  const std::uint64_t w = T + 2 * H;
  return (w * w * 4 + 4095) / 4096 * 4096;
}

}  // namespace

// ---------------------------------------------------------------------------

// Acceptance 1 (Lambert): sample() at random points matches the analytic
// function; tiles join continuously.
TEST(accept1_lambert_sampling)
{
  TempDir td;
  LambertFixture fx;
  const fs::path p = td.p / FieldFile::relative_path("hrrr", kRun, 3);
  CHECK(
    FieldFile::relative_path("hrrr", kRun, 3) ==
    "fields/hrrr/2025110509/f003.fld");
  {
    FieldFileWriter w(p, *fx.grid, "hrrr", kRun, 3);
    CHECK(w.add_field(info(1), analytic_field(*fx.grid)) == 0);
    w.seal();
  }
  auto ff = FieldFile::open(p);
  CHECK(ff->model() == "hrrr" && ff->run_s() == kRun && ff->fhr() == 3);
  CHECK(ff->tiles_x() == 3 && ff->tiles_y() == 2);
  CHECK(ff->grid_key() == fx.grid->key());
  ff->require_grid(*fx.grid);

  std::mt19937 rng(7);
  std::uniform_real_distribution<double> ui(0.0, fx.nx - 1.0),
    uj(0.0, fx.ny - 1.0);
  int located = 0;
  double worst = 0.0;
  for (int n = 0; n < 1000; ++n)
  {
    double lat, lon;
    fx.latlon(ui(rng), uj(rng), lat, lon);
    auto c = fx.grid->locate(lat, lon);
    if (!c)
      continue;
    ++located;
    worst = std::max(worst, std::fabs(ff->sample(0, *c) - analytic(lat, lon)));
  }
  CHECK(located > 990);
  CHECK(worst < 1e-4);

  // Across tile boundaries (i = 255 | 256, j = 255 | 256): the shared edge
  // gives bit-identical values from both tiles.
  for (double f : {0.0, 0.3, 1.0})
  {
    CHECK(
      ff->sample(0, {255, 100, 1.0, f}) == ff->sample(0, {256, 100, 0.0, f}));
    CHECK(
      ff->sample(0, {300, 255, f, 1.0}) == ff->sample(0, {300, 256, f, 0.0}));
  }
  // Corner values are the stored grid values.
  CHECK(ff->sample(0, {256, 256, 0.0, 0.0}) == ff->value(0, 256, 256));
  CHECK(
    ff->value(0, 599, 399) == static_cast<float>(analytic(
                                fx.grid->lat_deg()[399 * 600 + 599],
                                fx.grid->lon_deg()[399 * 600 + 599])));
}

// Acceptance 1 (global, periodic): accuracy and continuity across the seam.
TEST(accept1_global_periodic_seam)
{
  TempDir td;
  auto g = global_grid();
  const fs::path p = td.p / "f.fld";
  {
    FieldFileWriter w(p, *g, "gfs", kRun, 0);
    w.add_field(info(1), analytic_field(*g));
    w.seal();
  }
  auto ff = FieldFile::open(p);
  CHECK(ff->periodic_i());
  CHECK(ff->tiles_x() == 2 && ff->tiles_y() == 1);

  std::mt19937 rng(11);
  std::uniform_real_distribution<double> ulat(-88.0, 88.0), ulon(0.0, 360.0);
  double worst = 0.0;
  for (int n = 0; n < 1000; ++n)
  {
    const double lat = ulat(rng);
    // Every 4th point lands in the seam cell (359 .. 360 degrees).
    const double lon = n % 4 == 0 ? 359.0 + (ulon(rng) / 360.0) : ulon(rng);
    auto c = g->locate(lat, lon);
    CHECK(c.has_value());
    if (!c)
      continue;
    worst = std::max(worst, std::fabs(ff->sample(0, *c) - analytic(lat, lon)));
  }
  CHECK(worst < 1e-2);

  // The seam cell (i = 359) wraps to column 0 inside one tile.
  for (double f : {0.0, 0.5, 1.0})
    CHECK(ff->sample(0, {359, 120, 1.0, f}) == ff->sample(0, {0, 120, 0.0, f}));
  auto a = g->locate(30.0, 359.9999), b = g->locate(30.0, 0.0001);
  CHECK(a && b);
  if (a && b)
    CHECK(std::fabs(ff->sample(0, *a) - ff->sample(0, *b)) < 1e-3);
  // Halo of tile 0 holds column 359 on its left.
  CHECK(ff->tile(0, 0, 0)[1 * 258 + 0] == ff->value(0, 359, 0));
}

// Acceptance 2: bit-exact round trip of every tile; a CRC flip in one tile
// is caught on first touch of that tile only.
TEST(accept2_tiles_bit_exact_and_tile_crc)
{
  TempDir td;
  LambertFixture fx;
  const std::uint32_t T = 128, H = 2;
  std::vector<float> a = analytic_field(*fx.grid), b(a.size());
  for (std::size_t k = 0; k < b.size(); ++k)
    b[k] = k % 97 == 0 ? std::numeric_limits<float>::quiet_NaN()
                       : static_cast<float>(k) * 0.25f - 1000.0f;
  const fs::path p = td.p / "f.fld";
  {
    FieldFileWriter w(p, *fx.grid, "rrfs", kRun, 1, T, H);
    w.add_field(info(1, 85000.0f, 100), a);
    FieldInfo wind = info(2, 85000.0f, 100);
    wind.flags = kFieldGridRelative;
    w.add_field(wind, b);
    w.seal();
  }
  auto ff = FieldFile::open(p);
  CHECK(ff->size() == 2);
  CHECK(ff->tile_size() == T && ff->halo() == H);
  CHECK(ff->find(2, 100, 85000.0f) == std::optional<std::size_t>(1));
  CHECK(!ff->find(2, 100, 70000.0f));
  CHECK(ff->field(1).flags == kFieldGridRelative);
  CHECK(ff->field(0).level_type == 100 && ff->field(0).level_value == 85000.0f);
  // min/max over finite values.
  CHECK(ff->field(1).min == -1000.0f + 0.25f);
  CHECK(ff->field(1).max == static_cast<float>(b.size() - 1) * 0.25f - 1000.0f);
  for (std::size_t f = 0; f < 2; ++f)
  {
    const auto& src = f == 0 ? a : b;
    for (std::uint32_t ty = 0; ty < ff->tiles_y(); ++ty)
      for (std::uint32_t tx = 0; tx < ff->tiles_x(); ++tx)
        CHECK(bit_equal(
          ff->tile(f, tx, ty), expected_tile(*fx.grid, src, T, H, tx, ty)));
  }

  // Corrupt field 1, tile (2, 1).
  const std::uint64_t ntiles = ff->tiles_x() * ff->tiles_y();
  const std::uint64_t t = 1 * ff->tiles_x() + 2;
  flip_byte(p, 4096 + (1 * ntiles + t) * slot_bytes(T, H) + 1000);
  auto bad = FieldFile::open(p);  // opening does not read tiles
  CHECK(bit_equal(bad->tile(1, 1, 1), expected_tile(*fx.grid, b, T, H, 1, 1)));
  CHECK(bit_equal(bad->tile(0, 2, 1), expected_tile(*fx.grid, a, T, H, 2, 1)));
  CHECK(throws<std::runtime_error>([&] { bad->tile(1, 2, 1); }));
  CHECK(throws<std::runtime_error>([&] { bad->value(1, 2 * T + 5, T + 5); }));
  CHECK(throws<std::runtime_error>([&] { bad->tile(1, 2, 1); }));  // still
  CHECK(throws<std::out_of_range>([&] { bad->tile(2, 0, 0); }));
  CHECK(throws<std::out_of_range>([&] { bad->tile(0, 5, 0); }));
}

// Acceptance 3: edge tiles are NaN outside the grid; locate() outside the
// grid returns nullopt.
TEST(accept3_edge_tiles)
{
  TempDir td;
  LambertFixture fx;
  const fs::path p = td.p / "f.fld";
  {
    FieldFileWriter w(p, *fx.grid, "hrrr", kRun, 0);
    w.add_field(info(1), analytic_field(*fx.grid));
    w.seal();
  }
  auto ff = FieldFile::open(p);
  const std::size_t w = 258;
  // Tile (2, 1) covers i 512..767 and j 256..511 of a 600 x 400 grid.
  auto t = ff->tile(0, 2, 1);
  CHECK(std::isnan(t[(1 + 10) * w + (1 + 600 - 512)]));  // i = 600
  CHECK(std::isnan(t[(1 + 144) * w + (1 + 10)]));        // j = 400
  CHECK(!std::isnan(t[(1 + 143) * w + (1 + 87)]));       // (599, 399)
  CHECK(std::isnan(t[(1 + 143) * w + (1 + 88)]));        // (600, 399)
  // Left halo of the first tile (i = -1) and bottom halo (j = -1).
  auto t0 = ff->tile(0, 0, 0);
  CHECK(std::isnan(t0[5 * w + 0]));
  CHECK(std::isnan(t0[0 * w + 5]));
  CHECK(!std::isnan(t0[1 * w + 1]));
  // Outside the grid there is no cell to sample.
  double lat, lon;
  fx.latlon(-5.0, 100.0, lat, lon);
  CHECK(!fx.grid->locate(lat, lon));
  fx.latlon(100.0, 410.0, lat, lon);
  CHECK(!fx.grid->locate(lat, lon));
  // A cell that does not exist is rejected.
  CHECK(throws<std::out_of_range>([&] { ff->sample(0, {599, 10, 0.5, 0.5}); }));
  CHECK(throws<std::out_of_range>([&] { ff->sample(0, {10, 399, 0.5, 0.5}); }));
  CHECK(throws<std::out_of_range>([&] { ff->value(0, 600, 0); }));
}

// Acceptance 4: truncated file, wrong magic, damaged header or directory,
// wrong grid -> clear errors.
TEST(accept4_corruption_and_wrong_grid)
{
  TempDir td;
  LambertFixture fx;
  const fs::path p = td.p / "f.fld";
  {
    FieldFileWriter w(p, *fx.grid, "hrrr", kRun, 0, 64, 1);
    w.add_field(info(1), analytic_field(*fx.grid));
    w.add_field(info(2), analytic_field(*fx.grid));
    w.seal();
  }
  CHECK(FieldFile::open(p)->size() == 2);
  auto with = [&](auto damage)
  {
    const fs::path q = td.p / "copy.fld";
    fs::remove(q);
    fs::copy_file(p, q);
    damage(q);
    return throws<std::runtime_error>([&] { FieldFile::open(q); });
  };
  CHECK(with([](const fs::path& q) { flip_byte(q, 0); }));   // magic
  CHECK(with([](const fs::path& q) { flip_byte(q, 50); }));  // header
  CHECK(with([](const fs::path& q)
             { fs::resize_file(q, fs::file_size(q) - 1); }));  // truncated
  CHECK(with([](const fs::path& q) { fs::resize_file(q, 100); }));
  CHECK(with([](const fs::path& q)
             { fs::resize_file(q, fs::file_size(q) + 4096); }));  // grown
  CHECK(with([](const fs::path& q)
             { flip_byte(q, fs::file_size(q) - 3); }));  // tables
  // Directory: first byte after the slots.
  const auto ff = FieldFile::open(p);
  const std::uint64_t dir =
    4096 + 2ull * ff->tiles_x() * ff->tiles_y() * slot_bytes(64, 1);
  CHECK(with([&](const fs::path& q) { flip_byte(q, dir + 4); }));
  CHECK(
    throws<std::runtime_error>([&] { FieldFile::open(td.p / "missing.fld"); }));

  // Wrong grid (e.g. the GridStore holds another grid for that key).
  auto g = global_grid();
  CHECK(throws<std::runtime_error>([&] { ff->require_grid(*g); }));
  // Same dimensions, different definition (key).
  GridIdentity other = fx.grid->identity();
  other.canon.push_back('x');
  auto same_dims = Grid::adopt(
    other, fx.grid->lat_deg(), fx.grid->lon_deg(), fx.grid->rot_rad(), fx.grid);
  CHECK(throws<std::runtime_error>([&] { ff->require_grid(*same_dims); }));
  ff->require_grid(*fx.grid);
}

TEST(writer_validation)
{
  TempDir td;
  auto g = global_grid();
  const fs::path p = td.p / "f.fld";
  CHECK(
    throws<std::invalid_argument>(
      [&] { FieldFileWriter w(p, *g, "GFS", kRun, 0); }));
  CHECK(
    throws<std::invalid_argument>(
      [&] { FieldFileWriter w(p, *g, "gfs", kRun, 0, 1, 1); }));
  CHECK(
    throws<std::invalid_argument>(
      [&] { FieldFileWriter w(p, *g, "gfs", kRun, 0, 8, 0); }));
  CHECK(
    throws<std::invalid_argument>(
      [&] { FieldFileWriter w(p, *g, "gfs", kRun, 0, 8, 8); }));
  {
    FieldFileWriter w(p, *g, "gfs", kRun, 0);
    std::vector<float> v(g->size(), 1.0f);
    CHECK(
      throws<std::invalid_argument>(
        [&] { w.add_field(info(1), std::span<const float>(v).first(10)); }));
    w.add_field(info(1), v);
    CHECK(throws<std::invalid_argument>([&] { w.add_field(info(1), v); }));
    FieldInfo bad = info(3);
    bad.flags = 0x80;
    CHECK(throws<std::invalid_argument>([&] { w.add_field(bad, v); }));
    // Same parameter on another level is a different field.
    w.add_field(info(1, 2.0f, 103), v);
    CHECK(w.size() == 2);
    // All-missing field: min/max NaN.
    std::vector<float> nan(g->size(), std::numeric_limits<float>::quiet_NaN());
    w.add_field(info(4), nan);
    w.seal();
    CHECK(throws<std::logic_error>([&] { w.seal(); }));
  }
  auto ff = FieldFile::open(p);
  CHECK(ff->size() == 3);
  CHECK(std::isnan(ff->field(2).min) && std::isnan(ff->field(2).max));
  CHECK(ff->field(0).min == 1.0f && ff->field(0).max == 1.0f);
  // Published files are never replaced; an abandoned writer leaves nothing.
  {
    FieldFileWriter w(p, *g, "gfs", kRun, 0);
    CHECK(throws<std::runtime_error>([&] { w.seal(); }));
  }
  {
    FieldFileWriter w(td.p / "abandoned.fld", *g, "gfs", kRun, 0);
    w.add_field(info(1), std::vector<float>(g->size(), 2.0f));
  }
  std::size_t files = 0;
  for (const auto& e : fs::directory_iterator(td.p))
  {
    (void)e;
    ++files;
  }
  CHECK(files == 1);
}

TEST(snapshot_opens_field_files)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  LambertFixture fx;
  GridStore store(td.p / "grids");
  auto g = store.put(
    fx.grid->identity(),
    GridData{
      std::vector<double>(
        fx.grid->lat_deg(), fx.grid->lat_deg() + fx.grid->size()),
      std::vector<double>(
        fx.grid->lon_deg(), fx.grid->lon_deg() + fx.grid->size()),
      {}});
  Manifest m;
  m.sources = SourceRegistry::defaults();
  m.grids.push_back(
    GridEntry{
      g->key(), "grids/" + key_hex(g->key()) + ".grid",
      store.header_crc(g->key())});
  for (std::uint32_t fhr : {0u, 1u})
  {
    const std::string rel = FieldFile::relative_path("hrrr", kRun, fhr);
    FieldFileWriter w(td.p / rel, *g, "hrrr", kRun, fhr);
    std::vector<float> v = analytic_field(*g);
    for (auto& x : v)
      x += static_cast<float>(fhr);
    w.add_field(info(1), v);
    w.seal();
    m.fields.push_back(
      FieldEntry::from_file(td.p, *FieldFile::open(td.p / rel)));
  }
  CHECK(m.fields[1].fhr == 1 && m.fields[1].grid_key == g->key());
  m = pub.publish(m);

  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto s = l.current();
  CHECK(s->files_opened() == 3);
  auto f1 = s->field_file("hrrr", kRun, 1);
  CHECK(f1 != nullptr);
  CHECK(s->field_file("hrrr", kRun, 2) == nullptr);
  double lat, lon;
  fx.latlon(300.5, 200.25, lat, lon);
  auto c = s->grid(g->key())->locate(lat, lon);
  CHECK(c.has_value());
  if (c && f1)
    CHECK(std::fabs(f1->sample(0, *c) - (analytic(lat, lon) + 1.0)) < 1e-4);

  // Reused by the next snapshot; a wrong header CRC is refused.
  m.next_receipt_seq = 1;
  pub.publish(m);
  CHECK(l.refresh());
  CHECK(l.current()->files_opened() == 0);
  CHECK(l.current()->field_file("hrrr", kRun, 1) == f1);
  m.fields[0].header_crc ^= 1;
  pub.publish(m);
  CHECK(throws<std::runtime_error>([&] { l.refresh(); }));
}

int main()
{
  return tst::run_all();
}
