// Query engine (T9): point() on synthetic data through an in-memory
// FieldStore, plus field specs, JSON, and an integration run over real
// field files in a snapshot.
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/fieldfile.h"
#include "atmo/gridstore.h"
#include "atmo/manifest.h"
#include "atmo/obs_index.h"
#include "atmo/obs_segment.h"
#include "atmo/query.h"
#include "atmo/snapshot.h"
#include "atmo/variables.h"
#include "check.h"

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
        ("atmo_test_query_" + std::to_string(::getpid()) + "_" +
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

constexpr std::int64_t kRun0 = 1762300800;  // 2025-11-05 00Z
constexpr std::int64_t kH = 3600;

// Regular 1-degree lat/lon box, 30..40 N, 260..270 E (11 x 11).
std::shared_ptr<const Grid> box_grid(float rot_deg = 0.0f)
{
  GridIdentity id;
  const std::string c = "box rot " + std::to_string(rot_deg);
  id.canon.assign(c.begin(), c.end());
  id.ni = 11;
  id.nj = 11;
  GridData d;
  for (int j = 0; j < 11; ++j)
    for (int i = 0; i < 11; ++i)
    {
      d.lat_deg.push_back(30.0 + j);
      d.lon_deg.push_back(260.0 + i);
    }
  if (rot_deg != 0.0f)
    d.rot_rad.assign(121, rot_deg * 3.14159265358979f / 180.0f);
  return Grid::create(id, d);
}

using FieldFn = std::function<double(double lat, double lon)>;

// In-memory FieldStore: every field is an analytic function sampled at the
// grid points; sample() interpolates bilinearly like a real store.
class FakeStore : public FieldStore
{
 public:
  struct Slice
  {
    std::shared_ptr<const Grid> grid;
    std::vector<std::pair<FieldInfo, std::vector<float>>> fields;
  };

  void
  add(const std::string& model, ModelTime t, std::shared_ptr<const Grid> grid)
  {
    data_[model][{t.run_s, t.fhr}].grid = std::move(grid);
  }
  void
  field(const std::string& model, ModelTime t, FieldInfo info, const FieldFn& f)
  {
    Slice& s = data_[model][{t.run_s, t.fhr}];
    std::vector<float> v(s.grid->size());
    for (std::size_t k = 0; k < v.size(); ++k)
      v[k] = static_cast<float>(f(s.grid->lat_deg()[k], s.grid->lon_deg()[k]));
    s.fields.push_back({info, std::move(v)});
  }

  std::vector<std::string> models() const override
  {
    std::vector<std::string> m;
    for (const auto& [k, v] : data_)
      m.push_back(k);
    return m;
  }
  std::vector<ModelTime> times(std::string_view model) const override
  {
    std::vector<ModelTime> t;
    auto it = data_.find(std::string(model));
    if (it != data_.end())
      for (const auto& [k, v] : it->second)
        t.push_back({k.first, k.second});
    return t;
  }
  std::shared_ptr<const Grid> grid(std::string_view model, const ModelTime& t)
    const override
  {
    const Slice* s = find(model, t);
    return s ? s->grid : nullptr;
  }
  std::vector<FieldInfo> fields(std::string_view model, const ModelTime& t)
    const override
  {
    std::vector<FieldInfo> v;
    if (const Slice* s = find(model, t))
      for (const auto& f : s->fields)
        v.push_back(f.first);
    return v;
  }
  float sample(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& k,
    const Grid::Cell& c) const override
  {
    const Slice* s = find(model, t);
    if (!s)
      return NAN;
    for (const auto& [info, v] : s->fields)
    {
      if (
        info.param_id == k.param_id && info.level_type == k.level_type &&
        info.level_value == k.level_value)
      {
        const Grid& g = *s->grid;
        auto at = [&](std::uint32_t i, std::uint32_t j)
        { return static_cast<double>(v[g.index(i, j)]); };
        return static_cast<float>(
          (1 - c.fy) * ((1 - c.fx) * at(c.i, c.j) + c.fx * at(c.i + 1, c.j)) +
          c.fy * ((1 - c.fx) * at(c.i, c.j + 1) + c.fx * at(c.i + 1, c.j + 1)));
      }
    }
    return NAN;
  }

  // Bulk view over the stored values.
  class View : public FieldView
  {
   public:
    View(
      std::shared_ptr<const Grid> g,
      FieldInfo fi,
      const std::vector<float>* v)
        : FieldView(std::move(g), fi), v_(v)
    {
    }
    double at(std::uint32_t i, std::uint32_t j) const override
    {
      return (*v_)[grid().index(i, j)];
    }

   private:
    const std::vector<float>* v_;
  };
  std::shared_ptr<const FieldView> view(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& k) const override
  {
    const Slice* s = find(model, t);
    if (!s)
      return nullptr;
    for (const auto& [info, v] : s->fields)
      if (
        info.param_id == k.param_id && info.level_type == k.level_type &&
        info.level_value == k.level_value)
        return std::make_shared<View>(s->grid, info, &v);
    return nullptr;
  }

 private:
  const Slice* find(std::string_view model, const ModelTime& t) const
  {
    auto it = data_.find(std::string(model));
    if (it == data_.end())
      return nullptr;
    auto jt = it->second.find({t.run_s, t.fhr});
    return jt == it->second.end() ? nullptr : &jt->second;
  }

  std::map<std::string, std::map<std::pair<std::int64_t, std::uint32_t>, Slice>>
    data_;
};

FieldInfo
fi(std::uint16_t param, std::uint8_t lt, float lv, std::uint8_t flags = 0)
{
  FieldInfo f;
  f.param_id = param;
  f.level_type = lt;
  f.level_value = lv;
  f.flags = flags;
  return f;
}

PointQuery
query(double lat, double lon, std::int64_t t, std::vector<std::string> fields)
{
  PointQuery q;
  q.lat_deg = lat;
  q.lon_deg = lon;
  q.t_s = t;
  for (const auto& f : fields)
    q.fields.push_back(FieldSpec::parse(f));
  return q;
}

const SourceRegistry& registry()
{
  static const SourceRegistry r = SourceRegistry::defaults();
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(field_specs_parse_and_format)
{
  auto s = FieldSpec::parse("air_temperature@2m");
  CHECK(s.var->code == var::kAirTemperature);
  CHECK(s.level == FieldSpec::Level::fixed);
  CHECK(s.level_type == kLevelHeightAgl && s.level_value == 2.0f);
  CHECK(s.text == "air_temperature@2m");
  s = FieldSpec::parse("wind_u@850hPa");
  CHECK(s.level_type == kLevelIsobaric && s.level_value == 85000.0f);
  CHECK(s.text == "wind_u@850hPa");
  CHECK(FieldSpec::parse("pressure_msl@msl").level_type == kLevelMsl);
  CHECK(FieldSpec::parse("cape@sfc").level_type == kLevelSurface);
  s = FieldSpec::parse("relative_humidity@L104=0.44");
  CHECK(s.level_type == 104 && s.level_value == 0.44f);
  CHECK(s.text == "relative_humidity@L104=0.44");
  s = FieldSpec::parse("air_temperature");
  CHECK(s.level == FieldSpec::Level::query && s.text == "air_temperature");
  CHECK(
    FieldSpec::parse("air_temperature@925.5hPa").text ==
    "air_temperature@925.5hPa");

  for (const char* bad :
       {"nope", "air_temperature@", "air_temperature@abc",
        "air_temperature@-5m", "air_temperature@0hPa", "air_temperature@L300=1",
        "air_temperature@Lx=1", "air_temperature@2mm"})
    CHECK(throws<std::invalid_argument>([&] { FieldSpec::parse(bad); }));

  // format() is parseable and canonical.
  for (const FieldInfo& f :
       {fi(var::kAirTemperature, 103, 2), fi(var::kWindV, 100, 25000),
        fi(var::kPressureMsl, 101, 0), fi(var::kCape, 1, 0),
        fi(var::kRelativeHumidity, 104, 0.72f)})
    CHECK(FieldSpec::parse(FieldSpec::format(f)).text == FieldSpec::format(f));
}

TEST(time_selection_and_interpolation)
{
  FakeStore st;
  auto g = box_grid();
  // Run 00Z: f0..f6; run 06Z: f0..f3.  Value = run hour * 100 + valid hour.
  for (std::int64_t run : {kRun0, kRun0 + 6 * kH})
  {
    const int last = run == kRun0 ? 6 : 3;
    for (int f = 0; f <= last; ++f)
    {
      const ModelTime t{run, static_cast<std::uint32_t>(f)};
      st.add("gfs", t, g);
      const double v = static_cast<double>(
        (run - kRun0) / kH * 100 + (t.valid_s() - kRun0) / kH);
      st.field(
        "gfs", t, fi(var::kAirTemperature, 103, 2),
        [v](double, double) { return v; });
    }
  }
  auto at = [&](std::int64_t t)
  {
    return point(
             st, nullptr, registry(),
             query(35.5, 265.5, t, {"air_temperature@2m"}))
      .values.at(0);
  };

  // 07:30 -> run 06Z, between f1 (607) and f2 (608).
  PointValue v = at(kRun0 + 7 * kH + 1800);
  CHECK(v.status == PointValue::Status::ok);
  CHECK(v.run_s == kRun0 + 6 * kH);
  CHECK_NEAR(v.fhr, 1.5, 1e-12);
  CHECK_NEAR(v.value, 607.5, 1e-4);
  // 05:00 -> only run 00Z covers it.
  v = at(kRun0 + 5 * kH);
  CHECK(v.run_s == kRun0 && v.fhr == 5.0);
  CHECK_NEAR(v.value, 5.0, 1e-6);
  // Exactly 06:00: newest run, f0.
  v = at(kRun0 + 6 * kH);
  CHECK(v.run_s == kRun0 + 6 * kH && v.fhr == 0.0);
  CHECK_NEAR(v.value, 606.0, 1e-4);
  // After the newest forecast hour and before the first run: no data.
  CHECK(at(kRun0 + 10 * kH).status == PointValue::Status::no_time);
  CHECK(at(kRun0 - 1).status == PointValue::Status::no_time);
}

TEST(spatial_interpolation_and_grid_edges)
{
  FakeStore st;
  const ModelTime t{kRun0, 0};
  st.add("m", t, box_grid());
  // Linear in lat/lon: bilinear interpolation reproduces it.
  st.field(
    "m", t, fi(var::kPressureMsl, 101, 0),
    [](double lat, double lon) { return 100000 + 50 * lat - 20 * lon; });
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> ula(30.0, 40.0), ulo(260.0, 270.0);
  for (int n = 0; n < 200; ++n)
  {
    const double la = ula(rng), lo = ulo(rng);
    const auto r = point(
      st, nullptr, registry(), query(la, lo, kRun0, {"pressure_msl@msl"}));
    CHECK(r.values.at(0).status == PointValue::Status::ok);
    // locate() treats 1-degree cells as flat quads (~1e-3 cell).
    CHECK_NEAR(r.values[0].value, 100000 + 50 * la - 20 * lo, 0.2);
  }
  // Outside the grid.
  auto r = point(
    st, nullptr, registry(), query(45.0, 265.0, kRun0, {"pressure_msl@msl"}));
  CHECK(r.values.at(0).status == PointValue::Status::outside_grid);
  CHECK(std::isnan(r.values[0].value));
  // Unknown model, bad coordinates.
  PointQuery q = query(35.0, 265.0, kRun0, {"pressure_msl@msl"});
  q.models = {"nope"};
  CHECK(
    throws<std::invalid_argument>([&] { point(st, nullptr, registry(), q); }));
  q = query(95.0, 265.0, kRun0, {"pressure_msl@msl"});
  CHECK(
    throws<std::invalid_argument>([&] { point(st, nullptr, registry(), q); }));
}

TEST(vertical_isobaric_altitude_and_levels)
{
  FakeStore st;
  const ModelTime t{kRun0, 0};
  st.add("m", t, box_grid());
  // T = 200 + 10 ln(p): log-p interpolation is exact for it.
  const float levels[] = {100000, 85000, 70000, 50000};
  for (float p : levels)
  {
    st.field(
      "m", t, fi(var::kAirTemperature, 100, p),
      [p](double, double) { return 200 + 10 * std::log(p); });
    // Geopotential height per level (constant in space).
    const double gh = p == 100000  ? 100
                      : p == 85000 ? 1500
                      : p == 70000 ? 3000
                                   : 5500;
    st.field(
      "m", t, fi(var::kGeopotentialHeight, 100, p),
      [gh](double, double) { return gh; });
  }
  st.field(
    "m", t, fi(var::kAirTemperature, 103, 2),
    [](double, double) { return 290.0; });
  st.field(
    "m", t, fi(var::kPressureMsl, 101, 0),
    [](double, double) { return 101000.0; });

  auto one = [&](PointQuery q)
  { return point(st, nullptr, registry(), q).values.at(0); };

  PointQuery q = query(35.0, 265.0, kRun0, {"air_temperature"});
  q.level_hpa = 600.0;
  PointValue v = one(q);
  CHECK(v.status == PointValue::Status::ok);
  CHECK_NEAR(v.value, 200 + 10 * std::log(60000.0), 1e-3);
  q.level_hpa = 850.0;  // exact level
  CHECK_NEAR(one(q).value, 200 + 10 * std::log(85000.0), 1e-4);
  q.level_hpa = 300.0;  // above the top level
  CHECK(one(q).status == PointValue::Status::out_of_levels);
  // An explicit isobaric spec between levels interpolates as well.
  CHECK_NEAR(
    one(query(35, 265, kRun0, {"air_temperature@600hPa"})).value,
    200 + 10 * std::log(60000.0), 1e-3);
  // Explicit fixed level beats the query's coordinate.
  q = query(35.0, 265.0, kRun0, {"air_temperature@2m"});
  q.level_hpa = 600.0;
  CHECK_NEAR(one(q).value, 290.0, 1e-4);

  // Altitude: between 850 hPa (gh 1500) and 700 hPa (gh 3000), linear in
  // geometric height.
  const double R = 6371008.8;
  auto geo = [R](double gh) { return R * gh / (R - gh); };
  q = query(35.0, 265.0, kRun0, {"air_temperature"});
  q.alt_m = 2250.0;
  const double w = (2250.0 - geo(1500)) / (geo(3000) - geo(1500));
  const double t850 = 200 + 10 * std::log(85000.0f);
  const double t700 = 200 + 10 * std::log(70000.0f);
  CHECK_NEAR(one(q).value, t850 + w * (t700 - t850), 1e-3);
  q.alt_m = 50.0;  // below the 1000 hPa surface
  CHECK(one(q).status == PointValue::Status::out_of_levels);
  q.alt_m.reset();

  // No vertical coordinate: several levels -> level_required; one -> used.
  CHECK(
    one(query(35, 265, kRun0, {"air_temperature"})).status ==
    PointValue::Status::level_required);
  CHECK_NEAR(one(query(35, 265, kRun0, {"pressure_msl"})).value, 101000, 0.1);
  CHECK(
    one(query(35, 265, kRun0, {"cape"})).status ==
    PointValue::Status::no_field);
  CHECK(
    one(query(35, 265, kRun0, {"air_temperature@10m"})).status ==
    PointValue::Status::no_field);
  q = query(35, 265, kRun0, {"air_temperature"});
  q.level_hpa = 500;
  q.alt_m = 100;
  CHECK(
    throws<std::invalid_argument>([&] { point(st, nullptr, registry(), q); }));
}

TEST(grid_relative_winds_are_rotated)
{
  FakeStore st;
  const ModelTime t{kRun0, 0};
  // Grid +i axis 30 degrees counter-clockwise from east.
  st.add("lam", t, box_grid(30.0f));
  st.field(
    "lam", t, fi(var::kWindU, 103, 10, kFieldGridRelative),
    [](double, double) { return 10.0; });
  st.field(
    "lam", t, fi(var::kWindV, 103, 10, kFieldGridRelative),
    [](double, double) { return 0.0; });
  st.add("ll", t, box_grid());
  st.field(
    "ll", t, fi(var::kWindU, 103, 10), [](double, double) { return 10.0; });
  st.field(
    "ll", t, fi(var::kWindV, 103, 10), [](double, double) { return 0.0; });

  const auto r = point(
    st, nullptr, registry(),
    query(35.0, 265.0, kRun0, {"wind_u@10m", "wind_v@10m"}));
  CHECK(r.values.size() == 4);
  // Model order "lam", "ll"; fields in request order.
  CHECK(r.values[0].model == "lam" && r.values[2].model == "ll");
  CHECK_NEAR(r.values[0].value, 10.0 * std::cos(3.14159265358979 / 6), 1e-4);
  CHECK_NEAR(r.values[1].value, 10.0 * std::sin(3.14159265358979 / 6), 1e-4);
  CHECK_NEAR(r.values[2].value, 10.0, 1e-6);  // earth-relative: as stored
  CHECK_NEAR(r.values[3].value, 0.0, 1e-6);
}

TEST(observations_and_json)
{
  TempDir td;
  FakeStore st;
  const ModelTime t{kRun0, 0};
  st.add("m", t, box_grid());
  st.field(
    "m", t, fi(var::kAirTemperature, 103, 2),
    [](double, double) { return 290.94; });

  // Two METARs near the point, one far away.
  const SourceClass& metar = registry().at("metar");
  ObsSegmentWriter w(metar, kRun0);
  auto rep = [&](std::uint64_t key, float lat, float lon, const char* raw)
  {
    ObsReport r;
    r.report_key = key;
    r.receipt_seq = key;
    r.receipt_time_ms = (kRun0 + 120) * 1000;
    r.obs_time_s = kRun0 + 60;
    r.lat_deg = lat;
    r.lon_deg = lon;
    r.height_m = 300.0f;
    r.station_ref = static_cast<std::uint32_t>(key);
    r.values.push_back(ObsValue{var::kAirTemperature, 291.15f, 0});
    r.raw = raw;
    w.add_report(r);
  };
  rep(1, 35.05f, 265.0f, "KAAA 050001Z 18010KT 10SM CLR 18/10 A3000");
  rep(2, 35.2f, 265.1f, "KBBB 050001Z 20005KT 10SM CLR 17/09 A3001");
  rep(3, 39.0f, 268.0f, "KFAR 050001Z 00000KT 10SM CLR 15/05 A3002");
  w.seal(td.p / "obs.seg");
  ObsIndex idx(registry(), {ObsSegment::open(td.p / "obs.seg")});

  PointQuery q = query(35.0, 265.0, kRun0, {"air_temperature@2m"});
  q.obs = true;
  q.obs_radius_km = 40.0f;
  const PointResult r = point(st, &idx, registry(), q);
  CHECK(r.obs.size() == 2);
  CHECK(r.obs.size() == 2 && r.obs[0].second.report.report_key == 1);

  const nlohmann::json j = to_json(r);
  CHECK(j["values"][0]["value"].dump() == "290.94");
  CHECK(j["values"][0]["units"] == "K");
  CHECK(j["values"][0]["status"] == "ok");
  CHECK(j["values"][0]["run"] == "2025-11-05T00:00:00Z");
  CHECK(j["obs"].size() == 2);
  CHECK(j["obs"][0]["raw"] == "KAAA 050001Z 18010KT 10SM CLR 18/10 A3000");
  CHECK(j["obs"][0]["values"][0]["var"] == "air_temperature");
  CHECK(j["obs"][0]["values"][0]["value"].dump() == "291.15");
  CHECK(j["obs"][0]["obs_time"] == "2025-11-05T00:01:00Z");
  // Deterministic: same input, same bytes.
  CHECK(to_json(point(st, &idx, registry(), q)).dump() == j.dump());

  // Missing data is null, not a number.
  const auto none = to_json(point(
    st, nullptr, registry(),
    query(35, 265, kRun0 + 99 * kH, {"air_temperature@2m"})));
  CHECK(none["values"][0]["value"].is_null());
  CHECK(none["values"][0]["status"] == "no_time");
  CHECK(!none["values"][0].contains("run"));
}

TEST(iso8601_times)
{
  CHECK(iso8601(kRun0) == "2025-11-05T00:00:00Z");
  CHECK(parse_iso8601("2025-11-05T00:00:00Z") == kRun0);
  CHECK(parse_iso8601("2025-11-05T07:30:00") == kRun0 + 7 * kH + 1800);
  CHECK(parse_iso8601("2025-11-05T07:30") == kRun0 + 7 * kH + 1800);
  CHECK(parse_iso8601("2025-11-05T07:30Z") == kRun0 + 7 * kH + 1800);
  for (const char* bad :
       {"", "2025-11-05", "2025-13-05T00:00:00Z", "2025-11-05T25:00:00Z",
        "2025-11-05T00:00:00+02", "2025-11-05T00:00:00Zjunk", "yesterday"})
    CHECK(throws<std::invalid_argument>([&] { parse_iso8601(bad); }));
}

TEST(snapshot_field_store_and_meta)
{
  TempDir td;
  // Real field files for two forecast hours, published in a snapshot.
  auto g0 = box_grid();
  GridStore store(td.p / "grids");
  auto g = store.put(
    g0->identity(),
    GridData{
      std::vector<double>(g0->lat_deg(), g0->lat_deg() + g0->size()),
      std::vector<double>(g0->lon_deg(), g0->lon_deg() + g0->size()),
      {}});
  Manifest m;
  m.sources = registry();
  m.grids.push_back(
    GridEntry{
      g->key(), "grids/" + key_hex(g->key()) + ".grid",
      store.header_crc(g->key())});
  for (std::uint32_t fhr : {0u, 1u})
  {
    const std::string rel = FieldFile::relative_path("hrrr", kRun0, fhr);
    FieldFileWriter w(td.p / rel, *g, "hrrr", kRun0, fhr);
    std::vector<float> v(g->size(), 280.0f + static_cast<float>(fhr));
    w.add_field(fi(var::kAirTemperature, 103, 2), v);
    for (float p : {85000.0f, 50000.0f})
      w.add_field(
        fi(var::kAirTemperature, 100, p),
        std::vector<float>(g->size(), p / 1000.0f));
    w.seal();
    m.fields.push_back(
      FieldEntry::from_file(td.p, *FieldFile::open(td.p / rel)));
  }
  ManifestPublisher pub(td.p);
  pub.publish(m);
  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto snap = l.current();
  SnapshotFieldStore fs_(snap);
  CHECK(fs_.models() == std::vector<std::string>{"hrrr"});
  CHECK(fs_.times("hrrr").size() == 2);
  CHECK(fs_.times("gfs").empty());

  PointQuery q = query(35.5, 265.5, kRun0 + 1800, {"air_temperature@2m"});
  const PointResult r = point(fs_, &snap->obs(), snap->sources(), q);
  CHECK(r.values.size() == 1);
  CHECK_NEAR(r.values.at(0).value, 280.5, 1e-4);
  CHECK_NEAR(r.values[0].fhr, 0.5, 1e-12);

  const nlohmann::json meta = meta_json(*snap, fs_);
  CHECK(meta["snapshot"] == 1);
  CHECK(meta["models"].size() == 1);
  CHECK(meta["models"][0]["name"] == "hrrr");
  CHECK(meta["models"][0]["runs"][0]["run"] == "2025-11-05T00:00:00Z");
  CHECK(meta["models"][0]["runs"][0]["fhr"] == nlohmann::json({0, 1}));
  CHECK(
    meta["models"][0]["fields"] ==
    nlohmann::json(
      {"air_temperature@2m", "air_temperature@500hPa",
       "air_temperature@850hPa"}));
  CHECK(meta["obs_sources"].size() == registry().size());
  CHECK(meta["stations_generation"].is_null());
}

int main()
{
  return tst::run_all();
}
