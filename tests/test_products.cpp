// Derived products core (T21, 06-derived-products.md): built-in processors,
// interface JSON, grib_derive() output messages, and the matcher over an
// in-memory catalog (runs, own outputs under centre 254, chains, steps).
// No database needed; the MySQL side is tested in test_productd.
#include <eccodes.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/grib.h"
#include "atmo/grib_api.h"
#include "atmo/products.h"
#include "check.h"

using namespace atmo;
namespace fs = std::filesystem;

namespace
{

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

template <class F>
bool throws(F&& f)
{
  try
  {
    f();
  }
  catch (const std::exception&)
  {
    return true;
  }
  return false;
}

// ---- an in-memory catalog
// -------------------------------------------------------

class MemCatalog : public CatalogQuery
{
 public:
  std::shared_ptr<CatalogTables> t = std::make_shared<CatalogTables>();
  std::vector<CatMessage> msgs;

  std::shared_ptr<const CatalogTables> tables() override { return t; }
  std::vector<std::uint64_t> run_signatures(
    const CatalogTables&,
    std::uint64_t run) override
  {
    std::set<std::uint64_t> s;
    for (const auto& m : msgs)
    {
      if (m.run_id == run)
      {
        s.insert(m.signature_id);
      }
    }
    return {s.begin(), s.end()};
  }
  std::vector<CatMessage>
  messages(const CatalogTables&, std::uint64_t run, std::uint64_t sig) override
  {
    std::vector<CatMessage> out;
    for (const auto& m : msgs)
    {
      if (m.run_id == run && m.signature_id == sig)
      {
        out.push_back(m);
      }
    }
    return out;
  }
  std::optional<CatMessage> message(const CatalogTables&, std::uint64_t id)
    override
  {
    for (const auto& m : msgs)
    {
      if (m.id == id)
      {
        return m;
      }
    }
    return std::nullopt;
  }
  std::optional<std::uint64_t> replaced_by(const CatalogTables&, std::uint64_t)
    override
  {
    return std::nullopt;
  }
  std::vector<std::int64_t> run_offsets(const CatalogTables&, std::uint64_t run)
    override
  {
    std::set<std::int64_t> s;
    for (const auto& m : msgs)
    {
      if (m.run_id == run)
      {
        s.insert(m.offset_end_s);
      }
    }
    return {s.begin(), s.end()};
  }
  std::vector<CatMessage> messages_at(
    const CatalogTables&,
    std::uint64_t run,
    std::int64_t off) override
  {
    std::vector<CatMessage> out;
    for (const auto& m : msgs)
    {
      if (m.run_id == run && m.offset_end_s == off)
      {
        out.push_back(m);
      }
    }
    return out;
  }

  std::uint64_t add(
    std::uint64_t run,
    std::uint64_t sig,
    std::int64_t off,
    std::uint32_t lt = 103,
    double lv = 10.0)
  {
    CatMessage m;
    m.id = msgs.size() + 100;
    m.run_id = run;
    m.signature_id = sig;
    m.grid_id = 1;
    m.level1 = {lt, lv};
    m.level2 = {255, 0.0};
    m.offset_start_s = off;
    m.offset_end_s = off;
    msgs.push_back(m);
    return m.id;
  }
  void drop(std::uint64_t id)
  {
    std::erase_if(msgs, [&](const CatMessage& m) { return m.id == id; });
  }
};

constexpr std::int64_t H = 3600;
constexpr std::int64_t T0 = 1795300000 / 21600 * 21600;  // a 6-hourly base

// GFS u/v (centre 7, process 96) with two runs; product 1's outputs
// (centre 254, process 1) with one run.  Signatures: 1 u, 2 v, 3 speed.
struct World
{
  MemCatalog cat;
  std::unique_ptr<GribApi> api;
  std::shared_ptr<const CatalogFieldStore> names;
  std::uint64_t r1 = 1, r2 = 2, r3 = 3;

  World()
  {
    CatalogTables& t = *cat.t;
    t.generation = 7;
    t.centers = {
      {1, 7, "NCEP", {{1, 0, ""}}},
      {2, 254, "AtmoTensor", {{2, 0, "Derived"}}}};
    CatModel gfs;
    gfs.id = 1;
    gfs.center_id = 1;
    gfs.subcenter_id = 1;
    gfs.center_code = 7;
    gfs.process_id = 96;
    gfs.slug = "7-0-96";
    gfs.name = "GFS";
    CatModel ours = gfs;
    ours.id = 2;
    ours.center_id = 2;
    ours.subcenter_id = 2;
    ours.center_code = 254;
    ours.process_id = 1;
    ours.slug = "254-0-1";
    ours.name = "";
    t.models = {gfs, ours};
    CatGrid g;
    g.id = 1;
    g.key = 0x1234;
    g.ni = 4;
    g.nj = 3;
    t.grids = {g};
    t.model_grids = {
      {1, 1, 1, "0.25DEG", "gfs_0.25p"},
      {2, 2, 1, "0.25DEG", "254-0-1_0.25deg"}};
    t.runs = {
      {1, 1, T0, -1, std::nullopt},
      {2, 1, T0 + 6 * H, -1, std::nullopt},
      {3, 2, T0, -1, std::nullopt}};
    auto sig = [](std::uint64_t id, std::uint32_t n, std::string name)
    {
      CatSignature s;
      s.id = id;
      s.hash = 0x100 + id;
      s.base_hash = s.hash;
      s.discipline = 0;
      s.category = 2;
      s.number = n;
      s.parameter_id = id;
      s.name = std::move(name);
      s.status = "identified";
      return s;
    };
    t.signatures = {
      sig(1, 2, "U component of wind"), sig(2, 3, "V component of wind"),
      sig(3, 1, "Wind speed")};
    t.common_names = {{"wind_u", 1}, {"wind_v", 2}, {"wind_speed", 3}};
    t.sort();
    for (auto off : {0, 3, 6})
    {
      cat.add(r1, 1, off * H);
      cat.add(r1, 2, off * H);
    }
    for (auto off : {0, 3})
    {
      cat.add(r2, 1, off * H);
      cat.add(r2, 2, off * H);
    }
    cat.add(r1, 1, 0, 100, 85000.0);  // u at 850 hPa, to be ignored
    api = std::make_unique<GribApi>(fs::temp_directory_path(), cat, nullptr);
    names = api->field_store();
  }

  ProductMatcher matcher(std::vector<Product> ps)
  {
    return ProductMatcher(cat.t, *names, cat, std::move(ps), {});
  }
};

ProductInput same(std::string role, std::string model, std::string field)
{
  ProductInput i;
  i.role = std::move(role);
  i.model = std::move(model);
  i.field = std::move(field);
  return i;
}

Product product(std::uint64_t id, std::string slug, std::uint32_t pid)
{
  Product p;
  p.id = id;
  p.slug = std::move(slug);
  p.name = p.slug;
  p.process_id = pid;
  ProductOutput o;
  o.role = "speed";
  o.category = 2;
  o.number = 1;
  o.level1 = {103, 10.0};
  o.level2 = {255, 0.0};
  p.outputs = {o};
  return p;
}

Product wind_speed_product()
{
  Product p = product(1, "ws", 1);
  p.kind = ProcessorKind::builtin;
  p.processor = "wind_speed";
  p.inputs = {
    same("u", "gfs_0.25p", "wind_u@10m"), same("v", "gfs_0.25p", "wind_v@10m")};
  return p;
}

}  // namespace

// ---- processors
// --------------------------------------------------------------------

TEST(builtin_wind_speed_and_direction)
{
  const auto* ws = find_builtin("wind_speed");
  const auto* wd = find_builtin("wind_direction");
  CHECK(ws && wd && !find_builtin("nope"));
  CHECK(ws->iface.inputs.size() == 2 && ws->iface.outputs[0].role == "speed");
  CHECK(wd->iface.needs_alpha() && !ws->iface.needs_alpha());

  std::vector<float> u = {3, 0, -4, kNaN, 0.1f, 0};
  std::vector<float> v = {4, 5, 0, 1, 0.1f, -2};
  std::vector<float> out(u.size());
  ProcessorArgs a;
  a.ni = 3;
  a.nj = 2;
  a.inputs = {u.data(), v.data()};
  a.outputs = {out.data()};
  ws->fn(a);
  CHECK_NEAR(out[0], 5.0, 1e-6);
  CHECK_NEAR(out[1], 5.0, 1e-6);
  CHECK_NEAR(out[2], 4.0, 1e-6);
  CHECK(std::isnan(out[3]));

  a.params = param_values(wd->iface, {});
  CHECK(a.params.size() == 1 && a.params[0] == 0.5);
  wd->fn(a);
  // u=0, v=5: wind from the south -> 180; u=-4: from the east -> 90;
  // u=0, v=-2: from the north -> 360; 0.14 m/s: calm -> 0.
  CHECK_NEAR(out[1], 180.0, 1e-4);
  CHECK_NEAR(out[2], 90.0, 1e-4);
  CHECK_NEAR(out[5], 360.0, 1e-4);
  CHECK(out[4] == 0.0f);
  CHECK(std::isnan(out[3]));
  CHECK_NEAR(out[0], std::atan2(-3.0, -4.0) * 180.0 / M_PI + 360.0, 1e-3);

  // Grid-relative winds: a grid turned 90 degrees CCW (its +i points
  // north): u along +i = wind toward the north = from the south.
  std::vector<float> alpha(6, static_cast<float>(M_PI / 2));
  a.alpha = alpha.data();
  std::vector<float> u1 = {5, 0, 0, 0, 0, 0}, v1 = {0, 0, 0, 0, 0, 0};
  a.inputs = {u1.data(), v1.data()};
  wd->fn(a);
  CHECK_NEAR(out[0], 180.0, 1e-3);
}

TEST(interface_json_and_params)
{
  const auto f = ProductInterface::from_json(
    R"([{"role":"u","units":"m/s","needs_alpha":true},{"role":"v"}])",
    R"([{"role":"dir","units":"deg","discipline":0}])",
    R"([{"name":"calm","type":"float","default":0.5,"min":0},{"name":"n","type":"int"}])");
  CHECK(
    f.inputs.size() == 2 && f.inputs[0].needs_alpha &&
    !f.inputs[1].needs_alpha);
  CHECK(f.params.size() == 2 && f.params[1].is_int && !f.params[1].def);
  CHECK(throws([&] { param_values(f, {}); }));  // n has no default
  const auto p = param_values(f, {{"n", 2.6}});
  CHECK(p.size() == 2 && p[0] == 0.5 && p[1] == 3.0);
  std::string a, b, c;
  f.to_json(a, b, c);
  const auto g = ProductInterface::from_json(a, b, c);
  CHECK(g.inputs[0].needs_alpha && g.params[0].min && *g.params[0].min == 0.0);
  CHECK(throws([] { ProductInterface::from_json("{}", "[]", "[]"); }));
  CHECK(throws([] { ProductInterface::from_json("[{\"x\":1}]", "[]", "[]"); }));
  CHECK(offset_rule_from("own_previous") == OffsetRule::own_previous);
  CHECK(!offset_rule_from("later"));
}

// ---- grib_derive
// -------------------------------------------------------------------

TEST(grib_derive_writes_our_message_in_the_templates_scan_order)
{
  // Template: u at 10 m on a 4x3 lat/lon grid scanning north to south.
  codes_handle* h =
    codes_grib_handle_new_from_samples(nullptr, "regular_ll_sfc_grib2");
  CHECK(h != nullptr);
  codes_set_long(h, "Ni", 4);
  codes_set_long(h, "Nj", 3);
  codes_set_long(h, "jScansPositively", 0);
  codes_set_double(h, "latitudeOfFirstGridPointInDegrees", 40.0);
  codes_set_double(h, "latitudeOfLastGridPointInDegrees", 38.0);
  codes_set_double(h, "longitudeOfFirstGridPointInDegrees", 250.0);
  codes_set_double(h, "longitudeOfLastGridPointInDegrees", 253.0);
  codes_set_double(h, "iDirectionIncrementInDegrees", 1.0);
  codes_set_double(h, "jDirectionIncrementInDegrees", 1.0);
  codes_set_long(h, "dataDate", 20261121);
  codes_set_long(h, "dataTime", 600);
  codes_set_long(h, "parameterCategory", 2);
  codes_set_long(h, "parameterNumber", 2);
  codes_set_long(h, "typeOfFirstFixedSurface", 103);
  codes_set_long(h, "scaleFactorOfFirstFixedSurface", 0);
  codes_set_long(h, "scaledValueOfFirstFixedSurface", 10);
  codes_set_long(h, "forecastTime", 3);
  std::vector<double> z(12, 1.0);
  codes_set_double_array(h, "values", z.data(), z.size());
  const void* buf = nullptr;
  std::size_t len = 0;
  codes_get_message(h, &buf, &len);
  std::vector<unsigned char> tmpl(
    static_cast<const unsigned char*>(buf),
    static_cast<const unsigned char*>(buf) + len);
  codes_handle_delete(h);

  // Normalised order: j = 0 is the SOUTH row (38N).
  std::vector<float> vals(12);
  for (std::size_t k = 0; k < 12; ++k)
  {
    vals[k] = static_cast<float>(k) * 0.5f;
  }
  vals[5] = kNaN;
  GribDeriveSpec spec;
  spec.process_id = 7;
  spec.discipline = 0;
  spec.category = 2;
  spec.number = 1;
  spec.level1 = {103, 10.0};
  spec.level2 = {255, 0.0};
  spec.offset_end_s = 9 * 3600;
  const auto out = grib_derive(tmpl.data(), tmpl.size(), spec, vals);

  codes_handle* o =
    codes_handle_new_from_message(nullptr, out.data(), out.size());
  CHECK(o != nullptr);
  long v = 0;
  auto get = [&](const char* k)
  {
    codes_get_long(o, k, &v);
    return v;
  };
  CHECK(get("centre") == 254 && get("subCentre") == 0);
  CHECK(get("generatingProcessIdentifier") == 7);
  CHECK(
    get("discipline") == 0 && get("parameterCategory") == 2 &&
    get("parameterNumber") == 1);
  CHECK(
    get("typeOfFirstFixedSurface") == 103 &&
    get("scaledValueOfFirstFixedSurface") == 10);
  CHECK(get("productDefinitionTemplateNumber") == 0);
  CHECK(get("forecastTime") == 9 && get("indicatorOfUnitOfTimeRange") == 1);
  CHECK(get("dataDate") == 20261121 && get("dataTime") == 600);
  CHECK(get("jScansPositively") == 0 && get("Ni") == 4);
  codes_handle_delete(o);

  // Values in file order: the first file row is the north row (j = 2).
  const auto file = atmo::grib_values(out.data(), out.size());
  CHECK(file.size() == 12);
  CHECK_NEAR(file[0], vals[8], 1e-3);   // north-west corner
  CHECK_NEAR(file[11], vals[3], 1e-3);  // south-east corner
  // Every cell: file row r is normalised row j = 2 - r.  vals[5] (j = 1,
  // i = 1) is file index 5 (the middle row stays) and missing.
  bool all = true;
  for (std::size_t r = 0; r < 3; ++r)
  {
    for (std::size_t i = 0; i < 4; ++i)
    {
      const float want = vals[(2 - r) * 4 + i];
      const double got = file[r * 4 + i];
      all = all &&
            (std::isnan(want) ? std::isnan(got) : std::fabs(got - want) < 1e-3);
    }
  }
  CHECK(all);
  CHECK(std::isnan(file[5]));

  // Sub-hourly forecast time in minutes; bad specs.
  spec.offset_end_s = 90 * 60;
  const auto out2 = grib_derive(tmpl.data(), tmpl.size(), spec, vals);
  o = codes_handle_new_from_message(nullptr, out2.data(), out2.size());
  CHECK(get("indicatorOfUnitOfTimeRange") == 0 && get("forecastTime") == 90);
  codes_handle_delete(o);
  CHECK(throws(
    [&]
    { grib_derive(tmpl.data(), tmpl.size(), spec, std::vector<float>(11)); }));
  spec.bits = 40;
  CHECK(throws([&] { grib_derive(tmpl.data(), tmpl.size(), spec, vals); }));
}

// ---- matcher
// -----------------------------------------------------------------------

TEST(matcher_same_offset_inputs)
{
  World w;
  auto m = w.matcher({wind_speed_product()});
  CHECK(m.problems().empty());
  auto p = m.ready(1, w.r1, 3 * H);
  CHECK(p && p->inputs.size() == 2 && !p->chain_start);
  CHECK(
    p && p->inputs.at("u") == w.cat.messages_at(*w.cat.t, w.r1, 3 * H)[0].id);
  std::string why;
  CHECK(!m.ready(1, w.r1, 9 * H, &why) && why == "input u not there yet");

  // Arrivals: the v at r2 f003 makes (r2, 3h) ready; an 850 hPa u does not.
  ArrivedMessage a;
  a.run_id = w.r2;
  a.signature_id = 2;
  a.level1 = {103, 10.0};
  a.offset_end_s = 3 * H;
  ArrivedMessage b = a;
  b.run_id = w.r1;
  b.signature_id = 1;
  b.level1 = {100, 85000.0};
  b.offset_end_s = 0;
  const auto plans = m.plan({a, b});
  CHECK(
    plans.size() == 1 && plans[0].run_id == w.r2 &&
    plans[0].offset_end_s == 3 * H);

  // Every retained run: r1 0/3/6, r2 0/3.
  CHECK(m.plan_all(1).size() == 5);
  // Disabled products are not matched.
  auto off = wind_speed_product();
  off.enabled = false;
  CHECK(w.matcher({off}).plan_all(1).empty());
}

TEST(matcher_names_are_exact)
{
  World w;
  auto p = wind_speed_product();
  p.inputs[0].field = "speed@10m";  // only a guess (wind_speed)
  p.inputs[1].model = "gfs";        // not an api_name
  auto m = w.matcher({p});
  const auto pr = m.problems();
  CHECK(pr.count(1) && pr.at(1).size() == 2);
  CHECK(pr.at(1)[0].find("not an exact name") != std::string::npos);
  CHECK(pr.at(1)[1].find("not in the catalog") != std::string::npos);
  CHECK(m.plan_all(1).empty());
  p.inputs[0].field = "wind_u";  // no level
  CHECK(w.matcher({p}).problems().at(1)[0].find("@level") != std::string::npos);
}

TEST(matcher_previous_step_and_levels)
{
  World w;
  Product p = product(5, "tend", 5);
  p.inputs = {
    same("now", "gfs_0.25p", "wind_u@10m"),
    same("before", "gfs_0.25p", "wind_u@10m")};
  p.inputs[1].rule = OffsetRule::previous_step;
  p.inputs[1].step_s = 3 * H;
  auto m = w.matcher({p});
  CHECK(!m.ready(5, w.r1, 0));  // nothing 3 h before f000
  auto j = m.ready(5, w.r1, 6 * H);
  CHECK(
    j &&
    j->inputs.at("before") == w.cat.messages_at(*w.cat.t, w.r1, 3 * H)[0].id);
  // Optional input missing: still ready, without the role.
  p.inputs[1].required = false;
  auto j0 = w.matcher({p}).ready(5, w.r1, 0);
  CHECK(j0 && j0->inputs.size() == 1);
  // 850 hPa level: the u at 850 hPa is found, not the 10 m one.
  Product q = product(6, "u850", 6);
  q.inputs = {same("u", "gfs_0.25p", "wind_u@850hPa")};
  auto m2 = w.matcher({q});
  auto j850 = m2.ready(6, w.r1, 0);
  CHECK(
    j850 && w.cat.message(*w.cat.t, j850->inputs.at("u"))->level1.type == 100);
  CHECK(!m2.ready(6, w.r1, 3 * H));
}

TEST(matcher_own_previous_chain)
{
  World w;
  // Product 1 (process 1) feeds back on its own speed output, 3-hourly.
  Product p = product(1, "acc", 1);
  ProductInput prev;
  prev.role = "prev";
  prev.rule = OffsetRule::own_previous;
  prev.own_output = "speed";
  prev.step_s = 3 * H;
  p.inputs = {same("u", "gfs_0.25p", "wind_u@10m"), prev};
  // Its outputs so far: r3 (base T0) at f000 and f003.
  const auto o0 = w.cat.add(w.r3, 3, 0);
  const auto o3 = w.cat.add(w.r3, 3, 3 * H);
  auto m = w.matcher({p});
  CHECK(m.output_grids(1) == std::set<std::uint64_t>{2});

  auto j0 = m.ready(1, w.r1, 0);  // first step, no earlier run: chain start
  CHECK(j0 && j0->chain_start && !j0->inputs.count("prev"));
  auto j3 = m.ready(1, w.r1, 3 * H);
  CHECK(j3 && !j3->chain_start && j3->inputs.at("prev") == o0);
  auto j6 = m.ready(1, w.r1, 6 * H);
  CHECK(j6 && j6->inputs.at("prev") == o3);
  // The next run's f000 starts from the previous run's output valid 3 h
  // before (T0 + 3 h = r3 f003).
  auto n0 = m.ready(1, w.r2, 0);
  CHECK(n0 && !n0->chain_start && n0->inputs.at("prev") == o3);

  // Without f003 output: f006 waits (the primary exists at f003), and r2's
  // first step falls back to nothing -> chain start.
  w.cat.drop(o3);
  auto m2 = w.matcher({p});
  std::string why;
  CHECK(
    !m2.ready(1, w.r1, 6 * H, &why) &&
    why.find("previous step") != std::string::npos);
  auto n0b = m2.ready(1, w.r2, 0);
  CHECK(n0b && n0b->chain_start);

  // An output arriving triggers the next step and later runs' first step.
  const auto o3b = w.cat.add(w.r3, 3, 3 * H);
  auto m3 = w.matcher({p});
  ArrivedMessage a;
  a.id = o3b;
  a.run_id = w.r3;
  a.signature_id = 3;
  a.level1 = {103, 10.0};
  a.offset_end_s = 3 * H;
  const auto plans = m3.plan({a});
  std::set<std::pair<std::uint64_t, std::int64_t>> got;
  for (const auto& pl : plans)
  {
    got.emplace(pl.run_id, pl.offset_end_s);
  }
  CHECK(got.count({w.r1, 6 * H}) && got.count({w.r2, 0}));
}

TEST(matcher_chained_products_by_slug)
{
  World w;
  Product a = wind_speed_product();  // slug "ws", process 1: outputs in mg 2
  Product b = product(2, "ws_copy", 2);
  b.inputs = {same("s", "WS", "wind_speed@10m")};  // slug, any case
  const auto o0 = w.cat.add(w.r3, 3, 0);
  auto m = w.matcher({a, b});
  CHECK(!m.problems().count(2));
  auto j = m.ready(2, w.r3, 0);
  CHECK(j && j->inputs.at("s") == o0);
  // A disabled upstream product still names its outputs.
  a.enabled = false;
  CHECK(w.matcher({a, b}).ready(2, w.r3, 0).has_value());
  // plan(): the upstream output arriving triggers the chained product.
  ArrivedMessage ar;
  ar.id = o0;
  ar.run_id = w.r3;
  ar.signature_id = 3;
  ar.level1 = {103, 10.0};
  auto plans = w.matcher({a, b}).plan({ar});
  CHECK(plans.size() == 1 && plans[0].product_id == 2);
}

TEST(matcher_versions_and_ambiguity)
{
  World w;
  ProductMatcher m(
    w.cat.t, *w.names, w.cat, {wind_speed_product()}, {{1, {3, 0}}});
  auto j = m.ready(1, w.r1, 0);
  CHECK(j && j->processor_version == 3 && j->kernel_revision == 0);
  JobPlan k = *j;
  CHECK(k.same_job(*j));
  k.kernel_revision = 1;
  CHECK(!k.same_job(*j));
  // Two u messages at one level and time: ambiguous, not ready.
  w.cat.add(w.r1, 1, 0);
  std::string why;
  CHECK(!w.matcher({wind_speed_product()}).ready(1, w.r1, 0, &why));
  CHECK(why.find("ambiguous") != std::string::npos);
}

int main()
{
  return tst::run_all();
}
