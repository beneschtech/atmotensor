// GRIB2 ingest (T11).  Test GRIB files are generated with eccodes itself
// (no binary fixtures): IEEE packing so values round-trip exactly.
#include <eccodes.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/fieldfile.h"
#include "atmo/grib.h"
#include "atmo/grib_api.h"
#include "atmo/gribstore.h"
#include "atmo/http_api.h"
#include "atmo/manifest.h"
#include "atmo/query.h"
#include "atmo/snapshot.h"
#include "atmo/source.h"
#include "atmo/variables.h"
#include "check.h"
#include "fake_catalog.h"

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
        ("atmo_test_grib_" + std::to_string(::getpid()) + "_" +
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

constexpr std::int64_t kRun = 1762333200;  // 2025-11-05 09:00Z

double analytic(double lat, double lon)
{
  return 280.0 + 0.5 * lat + 0.25 * std::fmod(lon + 360.0, 360.0);
}

// What one generated message looks like.
struct Msg
{
  int discipline = 0, category = 0, number = 0;  // air temperature
  int level_type = 1;
  bool level_missing = true;
  long level_scaled = 0;
  int level_scale = 0;
  int fhr = 3;
  int pdt = 0;
  int stat_start_h = 0;  // pdt 8: interval [start, fhr]
  long date = 20251105, time = 900;
  // Lat/lon box, or a Lambert grid.
  bool lambert = false;
  long ni = 6, nj = 4;
  double south = 30.0, west = 260.0, dlat = 1.0, dlon = 1.0;
  bool i_neg = false, j_pos = true, j_consecutive = false;
  bool uv_relative = false;
  std::function<double(double, double)> f = analytic;  // NaN = missing
  const char* sample = "regular_ll_sfc_grib2";
};

void ck(int err, const char* what)
{
  if (err != 0)
    throw std::runtime_error(
      std::string("eccodes test helper: ") + what + ": " +
      codes_get_error_message(err));
}

void set_str(codes_handle* h, const char* k, const char* v)
{
  std::size_t n = std::strlen(v);
  ck(codes_set_string(h, k, v, &n), k);
}

std::vector<unsigned char> make_message(const Msg& m)
{
  codes_handle* h = codes_grib_handle_new_from_samples(nullptr, m.sample);
  if (!h)
    throw std::runtime_error("no sample");
  if (
    std::string(m.sample).find("grib2") == std::string::npos ||
    std::string(m.sample).find("reduced") != std::string::npos)
  {
    // Foreign samples (GRIB1, reduced Gaussian) are written unchanged.
    const void* buf = nullptr;
    std::size_t n = 0;
    ck(codes_get_message(h, &buf, &n), "get_message");
    std::vector<unsigned char> out(
      static_cast<const unsigned char*>(buf),
      static_cast<const unsigned char*>(buf) + n);
    codes_handle_delete(h);
    return out;
  }
  ck(codes_set_long(h, "discipline", m.discipline), "discipline");
  ck(codes_set_long(h, "dataDate", m.date), "dataDate");
  ck(codes_set_long(h, "dataTime", m.time), "dataTime");
  if (m.lambert)
  {
    ck(codes_set_long(h, "gridDefinitionTemplateNumber", 30), "gdt");
    ck(codes_set_long(h, "shapeOfTheEarth", 6), "shape");
    ck(codes_set_long(h, "Nx", m.ni), "Nx");
    ck(codes_set_long(h, "Ny", m.nj), "Ny");
    ck(codes_set_double(h, "latitudeOfFirstGridPointInDegrees", 30.0), "la1");
    ck(codes_set_double(h, "longitudeOfFirstGridPointInDegrees", 260.0), "lo1");
    ck(codes_set_double(h, "LaDInDegrees", 38.5), "LaD");
    ck(codes_set_double(h, "LoVInDegrees", 262.5), "LoV");
    ck(codes_set_double(h, "Latin1InDegrees", 38.5), "Latin1");
    ck(codes_set_double(h, "Latin2InDegrees", 38.5), "Latin2");
    ck(codes_set_double(h, "DxInMetres", 3000.0), "Dx");
    ck(codes_set_double(h, "DyInMetres", 3000.0), "Dy");
    ck(codes_set_long(h, "jScansPositively", 1), "jpos");
  }
  else
  {
    const double north = m.south + static_cast<double>(m.nj - 1) * m.dlat;
    const double east = m.west + static_cast<double>(m.ni - 1) * m.dlon;
    ck(codes_set_long(h, "Ni", m.ni), "Ni");
    ck(codes_set_long(h, "Nj", m.nj), "Nj");
    ck(codes_set_long(h, "iScansNegatively", m.i_neg), "ineg");
    ck(codes_set_long(h, "jScansPositively", m.j_pos), "jpos");
    ck(codes_set_long(h, "jPointsAreConsecutive", m.j_consecutive), "jcons");
    ck(
      codes_set_double(
        h, "latitudeOfFirstGridPointInDegrees", m.j_pos ? m.south : north),
      "la1");
    ck(
      codes_set_double(
        h, "latitudeOfLastGridPointInDegrees", m.j_pos ? north : m.south),
      "la2");
    ck(
      codes_set_double(
        h, "longitudeOfFirstGridPointInDegrees", m.i_neg ? east : m.west),
      "lo1");
    ck(
      codes_set_double(
        h, "longitudeOfLastGridPointInDegrees", m.i_neg ? m.west : east),
      "lo2");
    ck(codes_set_double(h, "iDirectionIncrementInDegrees", m.dlon), "di");
    ck(codes_set_double(h, "jDirectionIncrementInDegrees", m.dlat), "dj");
  }
  ck(codes_set_long(h, "uvRelativeToGrid", m.uv_relative), "uvrel");
  if (m.pdt == 8)
  {
    ck(codes_set_long(h, "productDefinitionTemplateNumber", 8), "pdt");
    ck(codes_set_long(h, "typeOfStatisticalProcessing", 1), "stat");
    ck(codes_set_long(h, "indicatorOfUnitOfTimeRange", 1), "unit");
    ck(codes_set_long(h, "forecastTime", m.stat_start_h), "ft");
    ck(codes_set_long(h, "lengthOfTimeRange", m.fhr - m.stat_start_h), "len");
  }
  else
  {
    ck(codes_set_long(h, "indicatorOfUnitOfTimeRange", 1), "unit");
    ck(codes_set_long(h, "forecastTime", m.fhr), "ft");
  }
  ck(codes_set_long(h, "parameterCategory", m.category), "cat");
  ck(codes_set_long(h, "parameterNumber", m.number), "num");
  ck(codes_set_long(h, "typeOfFirstFixedSurface", m.level_type), "lt");
  if (m.level_missing)
  {
    codes_set_missing(h, "scaleFactorOfFirstFixedSurface");
    codes_set_missing(h, "scaledValueOfFirstFixedSurface");
  }
  else
  {
    ck(
      codes_set_long(h, "scaleFactorOfFirstFixedSurface", m.level_scale), "sf");
    ck(
      codes_set_long(h, "scaledValueOfFirstFixedSurface", m.level_scaled),
      "sv");
  }
  set_str(h, "packingType", "grid_ieee");

  // Values from the coordinates eccodes reports for each message point.
  const std::size_t n = static_cast<std::size_t>(m.ni * m.nj);
  std::vector<double> v(n, 0.0), lat(n), lon(n), tmp(n);
  ck(codes_set_double_array(h, "values", v.data(), n), "values0");
  ck(codes_grib_get_data(h, lat.data(), lon.data(), tmp.data()), "get_data");
  bool any_missing = false;
  for (std::size_t k = 0; k < n; ++k)
  {
    v[k] = m.f(lat[k], lon[k]);
    if (std::isnan(v[k]))
    {
      v[k] = 9999.0;
      any_missing = true;
    }
  }
  if (any_missing)
  {
    ck(codes_set_double(h, "missingValue", 9999.0), "mv");
    ck(codes_set_long(h, "bitmapPresent", 1), "bitmap");
  }
  ck(codes_set_double_array(h, "values", v.data(), n), "values");
  const void* buf = nullptr;
  std::size_t len = 0;
  ck(codes_get_message(h, &buf, &len), "get_message");
  std::vector<unsigned char> out(
    static_cast<const unsigned char*>(buf),
    static_cast<const unsigned char*>(buf) + len);
  codes_handle_delete(h);
  return out;
}

fs::path write_grib(const fs::path& p, const std::vector<Msg>& msgs)
{
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  for (const auto& m : msgs)
  {
    const auto b = make_message(m);
    out.write(
      reinterpret_cast<const char*>(b.data()),
      static_cast<std::streamsize>(b.size()));
  }
  return p;
}

Msg param(int d, int c, int n)
{
  Msg m;
  m.discipline = d;
  m.category = c;
  m.number = n;
  return m;
}

Msg at_level(Msg m, int type, long scaled, int scale = 0)
{
  m.level_type = type;
  m.level_missing = false;
  m.level_scaled = scaled;
  m.level_scale = scale;
  return m;
}

GribIngestOptions opts(const std::string& model)
{
  GribIngestOptions o;
  o.model = model;
  return o;
}

bool has_skip(const std::vector<GribSkip>& v, const std::string& what)
{
  for (const auto& s : v)
    if (s.reason.find(what) != std::string::npos)
      return true;
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(grib_scan_modes_are_normalised)
{
  TempDir td;
  Msg ref;  // i east, j north, rows consecutive
  GribReader r0(write_grib(td.p / "ref.grib2", {ref}));
  auto a = r0.next();
  CHECK(a.has_value());
  CHECK(!r0.next());
  if (!a)
    return;
  const auto& g = a->grid->data;
  CHECK(a->grid->id.ni == 6 && a->grid->id.nj == 4);
  CHECK(!a->grid->id.periodic_i);
  CHECK(a->grid->id.canon.size() > 20);
  // Normalised: +i east, +j north.
  CHECK(g.lat_deg[0] == 30.0 && g.lon_deg[0] == 260.0);
  CHECK(g.lon_deg[1] == 261.0 && g.lat_deg[6] == 31.0);
  for (std::size_t k = 0; k < a->values.size(); ++k)
    CHECK(
      a->values[k] == static_cast<float>(analytic(g.lat_deg[k], g.lon_deg[k])));
  CHECK(a->run_s == kRun && a->fhr == 3);
  CHECK(a->info.param_id == var::kAirTemperature);
  CHECK(a->info.level_type == 1 && a->info.level_value == 0.0f);

  int variant = 0;
  for (bool i_neg : {false, true})
    for (bool j_pos : {false, true})
      for (bool j_cons : {false, true})
      {
        Msg m;
        m.i_neg = i_neg;
        m.j_pos = j_pos;
        m.j_consecutive = j_cons;
        GribReader r(
          write_grib(td.p / ("v" + std::to_string(variant++) + ".grib2"), {m}));
        auto b = r.next();
        CHECK(b.has_value());
        if (!b)
          continue;
        CHECK(b->grid->data.lat_deg == g.lat_deg);
        CHECK(b->grid->data.lon_deg == g.lon_deg);
        CHECK(b->values == a->values);
        // Section 3 differs with the scanning mode, so the key may too;
        // the normalised grid is the same either way.
        CHECK(b->grid->id.ni == 6 && b->grid->id.nj == 4);
      }
}

TEST(grib_ingest_units_levels_and_skips)
{
  TempDir td;
  std::vector<Msg> msgs;
  msgs.push_back(at_level(Msg{}, 103, 2));  // T 2 m
  Msg rh = at_level(param(0, 1, 1), 100, 85000);
  rh.f = [](double lat, double) { return 50.0 + lat; };  // percent
  msgs.push_back(rh);
  Msg apcp = param(0, 1, 8);  // precipitation 0-3 h
  apcp.pdt = 8;
  apcp.f = [](double, double lon) { return lon - 255.0; };
  msgs.push_back(apcp);
  Msg apcp23 = apcp;  // 2-3 h: skipped
  apcp23.stat_start_h = 2;
  msgs.push_back(apcp23);
  msgs.push_back(param(0, 16, 196));        // local parameter: skipped
  msgs.push_back(at_level(Msg{}, 103, 2));  // duplicate: skipped
  Msg other_grid = at_level(Msg{}, 103, 10);
  other_grid.ni = 5;
  msgs.push_back(other_grid);                  // other grid: skipped
  Msg odd_level = at_level(Msg{}, 100, 5, 1);  // 0.5 Pa (scale factor 1)
  msgs.push_back(odd_level);
  Msg gr1;
  gr1.sample = "regular_ll_sfc_grib1";
  msgs.push_back(gr1);  // GRIB1: skipped
  Msg reduced;
  reduced.sample = "reduced_gg_pl_32_grib2";
  msgs.push_back(reduced);  // reduced grid: skipped
  const fs::path f = write_grib(td.p / "in.grib2", msgs);

  const GribIngestResult r = ingest_grib(td.p, {f}, opts("gfs"));
  CHECK(r.messages == msgs.size());
  CHECK(r.fields == 4);
  CHECK(r.skipped.size() == 6);
  CHECK(has_skip(r.skipped, "does not start at the run"));
  CHECK(has_skip(r.skipped, "not in the variable registry"));
  CHECK(has_skip(r.skipped, "already taken"));
  CHECK(has_skip(r.skipped, "grid differs"));
  CHECK(has_skip(r.skipped, "not GRIB edition 2"));
  CHECK(has_skip(r.skipped, "unsupported grid type"));
  CHECK(r.field.path == "fields/gfs/2025110509/f003.fld");
  CHECK(r.field.run_s == kRun && r.field.fhr == 3);

  auto ff = FieldFile::open(td.p / r.field.path);
  const auto t2 = ff->find(var::kAirTemperature, 103, 2.0f);
  const auto rh850 = ff->find(var::kRelativeHumidity, 100, 85000.0f);
  const auto precip = ff->find(var::kPrecipitationAmount, 1, 0.0f);
  const auto half_pa = ff->find(var::kAirTemperature, 100, 0.5f);
  CHECK(t2 && rh850 && precip && half_pa);
  if (t2 && rh850 && precip)
  {
    // (i, j) = (2, 1) is 31 N, 262 E.
    CHECK(ff->value(*t2, 2, 1) == static_cast<float>(analytic(31.0, 262.0)));
    CHECK(std::fabs(ff->value(*rh850, 2, 1) - 0.81f) < 1e-6);  // % -> 1
    CHECK(ff->value(*precip, 2, 1) == 7.0f);
    CHECK(ff->field(*rh850).max <= 0.84f);
  }

  // Only selected variables; another model name, so another file.
  GribIngestOptions only_t = opts("gfs_t");
  only_t.variables = {var::kAirTemperature};
  const GribIngestResult r2 = ingest_grib(td.p, {f}, only_t);
  CHECK(r2.fields == 2);  // 2 m and 0.5 Pa
  CHECK(has_skip(r2.skipped, "not selected"));
  CHECK(r2.grid.key == r.grid.key);  // same grid, stored once
}

TEST(grib_winds_lambert_and_missing_values)
{
  TempDir td;
  Msg u = at_level(param(0, 2, 2), 103, 10);
  u.lambert = true;
  u.ni = 20;
  u.nj = 15;
  u.uv_relative = true;
  u.f = [](double lat, double) { return lat - 30.0; };
  Msg v = u;
  v.number = 3;
  Msg t = at_level(Msg{}, 103, 2);
  t.lambert = true;
  t.ni = 20;
  t.nj = 15;
  t.uv_relative = true;  // only vector components get the flag
  // Missing points via the bitmap.
  t.f = [](double lat, double lon)
  {
    return lat > 30.2 ? std::numeric_limits<double>::quiet_NaN()
                      : analytic(lat, lon);
  };
  const fs::path f = write_grib(td.p / "hrrr.grib2", {u, v, t});
  const GribIngestResult r = ingest_grib(td.p, {f}, opts("hrrr"));
  CHECK(r.fields == 3 && r.skipped.empty());
  auto ff = FieldFile::open(td.p / r.field.path);
  const auto ku = ff->find(var::kWindU, 103, 10.0f);
  const auto kv = ff->find(var::kWindV, 103, 10.0f);
  const auto kt = ff->find(var::kAirTemperature, 103, 2.0f);
  CHECK(ku && kv && kt);
  if (!(ku && kv && kt))
    return;
  CHECK(ff->field(*ku).flags == kFieldGridRelative);
  CHECK(ff->field(*kv).flags == kFieldGridRelative);
  CHECK(ff->field(*kt).flags == 0);
  CHECK(!ff->periodic_i());

  // The stored grid: first point is the GRIB first point; +j is north-ish.
  SnapshotLoader l(td.p);
  ManifestPublisher pub(td.p);
  Manifest m;
  m.sources = SourceRegistry::defaults();
  add_to_manifest(m, r);
  CHECK(throws<std::invalid_argument>([&] { add_to_manifest(m, r); }));
  pub.publish(m);
  CHECK(l.refresh());
  auto s = l.current();
  auto g = s->grid(r.grid.key);
  CHECK(g != nullptr);
  if (!g)
    return;
  CHECK(std::fabs(g->lat_deg()[0] - 30.0) < 1e-9);
  CHECK(g->lat_deg()[20 * 14] > g->lat_deg()[0]);
  // Missing points are NaN, others are the analytic values.
  std::size_t nan = 0, finite = 0;
  for (std::uint32_t j = 0; j < 15; ++j)
    for (std::uint32_t i = 0; i < 20; ++i)
    {
      const float x = ff->value(*kt, i, j);
      const std::size_t k = j * 20 + i;
      if (g->lat_deg()[k] > 30.2)
      {
        nan += std::isnan(x) ? 1 : 0;
      }
      else
      {
        finite +=
          x == static_cast<float>(analytic(g->lat_deg()[k], g->lon_deg()[k]))
            ? 1
            : 0;
      }
    }
  CHECK(nan > 0 && finite > 0 && nan + finite == 300);
}

TEST(grib_global_periodic_and_end_to_end_sampling)
{
  TempDir td;
  Msg m;  // 2 degree global
  m.ni = 180;
  m.nj = 91;
  m.south = -90.0;
  m.west = 0.0;
  m.dlat = 2.0;
  m.dlon = 2.0;
  m.j_pos = false;  // north to south, like GFS
  m.f = [](double lat, double lon)
  {
    const double r = 3.14159265358979323846 / 180.0;
    return 280.0 + 10.0 * std::cos(lat * r) * std::cos(lon * r);
  };
  const fs::path f = write_grib(td.p / "gfs.grib2", {at_level(m, 103, 2)});
  const GribIngestResult r = ingest_grib(td.p, {f}, opts("gfs"));
  auto ff = FieldFile::open(td.p / r.field.path);
  CHECK(ff->periodic_i());

  ManifestPublisher pub(td.p);
  Manifest man;
  man.sources = SourceRegistry::defaults();
  add_to_manifest(man, r);
  pub.publish(man);
  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto s = l.current();
  auto file = s->field_file("gfs", kRun, 3);
  auto g = s->grid(r.grid.key);
  CHECK(file && g);
  if (!(file && g))
    return;
  // Across the seam and elsewhere: close to the analytic field.
  double worst = 0.0;
  for (double lon : {0.5, 120.3, 359.0, 359.9})
    for (double lat : {-60.0, 0.7, 45.2})
    {
      auto c = g->locate(lat, lon);
      CHECK(c.has_value());
      if (c)
        worst = std::max(worst, std::fabs(file->sample(0, *c) - m.f(lat, lon)));
    }
  CHECK(worst < 0.02);
}

TEST(grib_errors)
{
  TempDir td;
  const fs::path f = write_grib(td.p / "a.grib2", {Msg{}});
  CHECK(
    throws<std::invalid_argument>([&]
                                  { ingest_grib(td.p, {f}, opts("GFS")); }));
  GribIngestOptions bad_var = opts("gfs");
  bad_var.variables = {60000};
  CHECK(
    throws<std::invalid_argument>([&] { ingest_grib(td.p, {f}, bad_var); }));
  CHECK(
    throws<std::runtime_error>(
      [&] { ingest_grib(td.p, {td.p / "missing.grib2"}, opts("gfs")); }));

  ingest_grib(td.p, {f}, opts("gfs"));
  // Already ingested: the field file is immutable.
  CHECK(
    throws<std::runtime_error>([&] { ingest_grib(td.p, {f}, opts("gfs")); }));

  // Mixed forecast hours in one ingest.
  Msg f6;
  f6.fhr = 6;
  const fs::path mixed = write_grib(td.p / "mixed.grib2", {Msg{}, f6});
  CHECK(
    throws<std::runtime_error>([&]
                               { ingest_grib(td.p, {mixed}, opts("rap")); }));
  // A failed ingest leaves no field file or temp file behind (an empty
  // directory at most, which gc removes).
  std::size_t left = 0;
  if (fs::exists(td.p / "fields/rap"))
    for (const auto& e : fs::recursive_directory_iterator(td.p / "fields/rap"))
      left += e.is_regular_file() ? 1 : 0;
  CHECK(left == 0);

  // Nothing usable.
  const fs::path useless =
    write_grib(td.p / "useless.grib2", {param(0, 16, 196)});
  CHECK(
    throws<std::runtime_error>([&]
                               { ingest_grib(td.p, {useless}, opts("nam")); }));

  // Truncated message.
  const fs::path trunc = td.p / "trunc.grib2";
  fs::copy_file(f, trunc);
  fs::resize_file(trunc, fs::file_size(trunc) / 2);
  CHECK(
    throws<std::runtime_error>([&]
                               { ingest_grib(td.p, {trunc}, opts("ruc")); }));
}

// ---------------------------------------------------------------------------
// GRIB-native store (T10c)
// ---------------------------------------------------------------------------

TEST(grib_catalog_matches_tiles)
{
  TempDir td;
  // Several fields and two forecast hours in one file, like ECMWF steps.
  std::vector<Msg> msgs;
  for (int fhr : {0, 1})
  {
    Msg t2 = at_level(Msg{}, 103, 2);
    t2.ni = 40;
    t2.nj = 30;
    t2.fhr = fhr;
    t2.f = [fhr](double lat, double lon)
    { return analytic(lat, lon) + fhr + 0.01 * lat * lon; };
    Msg u = at_level(param(0, 2, 2), 103, 10);
    u.ni = 40;
    u.nj = 30;
    u.fhr = fhr;
    u.f = [](double lat, double) { return lat - 40.0; };
    msgs.push_back(t2);
    msgs.push_back(u);
  }
  msgs.push_back(param(0, 16, 196));  // skipped
  fs::create_directories(td.p / "ingest");
  const fs::path src = write_grib(td.p / "ingest/model.grib2", msgs);

  const CatalogFile r = catalog_grib(td.p, src, "nat");
  CHECK(r.messages.size() == 5);  // the unknown parameter is catalogued too
  CHECK(r.path == "grib/nat/2025110509/model.grib2");
  CHECK(r.grids.size() == 2);  // the extra message has its own grid
  // Kept as published: a hard link to the very same bytes.
  struct stat a, b;
  ::stat(src.c_str(), &a);
  ::stat((td.p / r.path).c_str(), &b);
  CHECK(a.st_ino == b.st_ino);
  CHECK(!fs::exists(td.p / (r.path + ".idx.json")));  // no sidecar (T18)

  // The same messages as T7 tiles, per hour, under another model name.
  for (int fhr : {0, 1})
  {
    std::vector<Msg> hour;
    for (const auto& m : msgs)
      if (
        m.fhr == fhr && m.sample == std::string("regular_ll_sfc_grib2") &&
        m.discipline == 0 && m.category != 16)
        hour.push_back(m);
    write_grib(td.p / ("h" + std::to_string(fhr) + ".grib2"), hour);
  }
  const auto t0 = ingest_grib(td.p, {td.p / "h0.grib2"}, opts("til"));
  const auto t1 = ingest_grib(td.p, {td.p / "h1.grib2"}, opts("til"));
  ManifestPublisher pub(td.p);
  Manifest m;
  m.sources = SourceRegistry::defaults();
  add_to_manifest(m, t0);
  add_to_manifest(m, t1);
  pub.publish(m);
  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto snap = l.current();
  SnapshotFieldStore tiles(snap);
  CHECK(tiles.models() == std::vector<std::string>({"til"}));

  // The catalog path: decoded in place through the cache.
  FakeCatalog cat(r);
  GribCache cache({}, grib_decoder());
  GribApi api(td.p, cat, &cache);
  const auto native = api.field_store();
  std::string model;
  for (const auto& mm : native->models())
  {
    model = native->times(mm).size() == 2 ? mm : model;
  }
  CHECK(!model.empty());
  // Listing fields (/v1/meta) decodes no more than one wind message per
  // grid to learn whether its winds are grid-relative (it was every one).
  for (const auto& tm : native->times(model))
  {
    CHECK(native->fields(model, tm).size() == 2);
  }
  CHECK(cache.stats().misses == 1);

  std::mt19937 rng(5);
  std::uniform_real_distribution<double> ula(30.5, 58.0), ulo(260.5, 298.0);
  double worst = 0.0;
  for (int n = 0; n < 200; ++n)
  {
    PointQuery q;
    q.lat_deg = ula(rng);
    q.lon_deg = ulo(rng);
    q.t_s = kRun + 1800;  // between the two hours
    q.fields = {
      FieldSpec::parse("air_temperature@2m"), FieldSpec::parse("wind_u@10m")};
    q.models = {"til"};
    const PointResult ft = point(tiles, nullptr, snap->sources(), q);
    q.models = {model};
    q.fields = {
      FieldSpec::parse("temperature@2m"),
      FieldSpec::parse("u-component_of_wind@10m")};
    const PointResult nt = point(*native, nullptr, snap->sources(), q);
    CHECK(ft.values.size() == 2 && nt.values.size() == 2);
    for (std::size_t k = 0; k < 2 && k < nt.values.size(); ++k)
    {
      CHECK(nt.values[k].status == PointValue::Status::ok);
      CHECK(ft.values[k].status == PointValue::Status::ok);
      worst =
        std::max(worst, std::fabs(nt.values[k].value - ft.values[k].value));
    }
  }
  // Native interpolates the doubles, tiles the stored floats.
  CHECK(worst < 1e-3);
  const auto st = cache.stats();
  CHECK(st.misses == 4);  // two fields x two hours, decoded once each
                          // (the u at f00 was the grid probe above)
  CHECK(st.hits > 700);

  // Bytes changed on disk: the hash check refuses the message.
  GribCache cold({}, grib_decoder());
  GribApi api2(td.p, cat, &cold);
  {
    std::fstream f(
      td.p / r.path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(r.messages[0].byte_offset + 200));
    f.put('\x55');
  }
  PointQuery q;
  q.lat_deg = 40.0;
  q.lon_deg = 270.0;
  q.t_s = kRun;
  q.fields = {FieldSpec::parse("temperature@2m")};
  q.models = {model};
  CHECK(
    throws<std::runtime_error>(
      [&] { point(*api2.field_store(), nullptr, snap->sources(), q); }));
}

int main()
{
  return tst::run_all();
}
