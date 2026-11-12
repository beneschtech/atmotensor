// /v1/grib/* (T17) over an in-memory CatalogQuery built from a real
// catalog_grib() result: no database needed.  The MySQL CatalogReader is
// tested in test_catalog.
#include <eccodes.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/catalog.h"
#include "atmo/grib.h"
#include "atmo/grib_api.h"
#include "atmo/http_api.h"
#include "atmo/manifest.h"
#include "atmo/snapshot.h"
#include "check.h"
#include "fake_catalog.h"

using namespace atmo;
namespace fs = std::filesystem;
using nlohmann::json;

namespace
{

struct TempDir
{
  fs::path p;
  TempDir()
  {
    p = fs::temp_directory_path() /
        ("atmo_test_grib_api_" + std::to_string(::getpid()) + "_" +
         std::to_string(std::random_device{}()));
    fs::create_directories(p);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

using Bytes = std::vector<unsigned char>;
using Sets = std::vector<std::pair<const char*, long>>;

void ck(int err, const char* what)
{
  if (err != 0)
  {
    throw std::runtime_error(
      std::string("eccodes: ") + what + ": " + codes_get_error_message(err));
  }
}

// NCEP-style message with every value = `value`, exact (IEEE packing).
Bytes make(
  long pdt,
  const Sets& sets,
  double value,
  const char* sample = "regular_ll_sfc_grib2")
{
  codes_handle* h = codes_grib_handle_new_from_samples(nullptr, sample);
  if (!h)
  {
    throw std::runtime_error("no sample");
  }
  ck(codes_set_long(h, "centre", 7), "centre");
  ck(codes_set_long(h, "productDefinitionTemplateNumber", pdt), "pdt");
  ck(codes_set_long(h, "dataDate", 20261110), "dataDate");
  ck(codes_set_long(h, "dataTime", 0), "dataTime");
  ck(codes_set_long(h, "generatingProcessIdentifier", 96), "process");
  for (const auto& [k, v] : sets)
  {
    ck(codes_set_long(h, k, v), k);
  }
  std::size_t n = 0;
  ck(codes_get_size(h, "values", &n), "size");
  std::size_t len = 9;
  ck(codes_set_string(h, "packingType", "grid_ieee", &len), "packing");
  std::vector<double> v(n, value);
  ck(codes_set_double_array(h, "values", v.data(), n), "values");
  const void* buf = nullptr;
  std::size_t sz = 0;
  ck(codes_get_message(h, &buf, &sz), "get_message");
  Bytes out(
    static_cast<const unsigned char*>(buf),
    static_cast<const unsigned char*>(buf) + sz);
  codes_handle_delete(h);
  return out;
}

Bytes precip(long start, long len, double value)
{
  return make(
    8,
    {{"parameterCategory", 1},
     {"parameterNumber", 8},
     {"typeOfFirstFixedSurface", 1},
     {"typeOfStatisticalProcessing", 1},
     {"forecastTime", start},
     {"lengthOfTimeRange", len}},
    value);
}

const Sets kT850 = {
  {"parameterCategory", 0},
  {"parameterNumber", 0},
  {"typeOfFirstFixedSurface", 100},
  {"scaleFactorOfFirstFixedSurface", 0},
  {"scaledValueOfFirstFixedSurface", 85000}};

Sets at(Sets s, long ft)
{
  s.push_back({"forecastTime", ft});
  return s;
}

struct Fixture
{
  TempDir dir;
  fs::path root;
  CatalogFile file;
  std::unique_ptr<FakeCatalog> cat;
  GribCache cache{GribCacheLimits{}, grib_decoder()};
  std::unique_ptr<GribApi> api;

  Fixture()
  {
    root = dir.p / "store";
    fs::create_directories(root);
    const fs::path src = dir.p / "f.grib2";
    const std::vector<Bytes> msgs = {
      make(0, at(kT850, 6), 281.25),  // 100 t850 f06
      make(0, at(kT850, 3), 280.0),   // 101 t850 f03
      precip(0, 1, 1.0),              // 102 run total 0-1 (first bucket)
      precip(1, 1, 2.0),              // 103 1 h bucket 1-2
      precip(0, 2, 3.0),              // 104 run total 0-2
      make(0, at(kT850, 6), 5.0, "reduced_gg_pl_32_grib2"),  // 105
    };
    std::ofstream o(src, std::ios::binary);
    for (const auto& m : msgs)
    {
      o.write(
        reinterpret_cast<const char*>(m.data()),
        static_cast<std::streamsize>(m.size()));
    }
    o.close();
    file = catalog_grib(root, src, "test");
    cat = std::make_unique<FakeCatalog>(file);
    api = std::make_unique<GribApi>(root, *cat, &cache);
  }

  fcgi::Response get(
    const std::string& path,
    const std::string& qs = "",
    const std::string& inm = "") const
  {
    fcgi::Request r;
    r.params["DOCUMENT_URI"] = path;
    r.params["QUERY_STRING"] = qs;
    r.params["REQUEST_METHOD"] = "GET";
    if (!inm.empty())
    {
      r.params["HTTP_IF_NONE_MATCH"] = inm;
    }
    return api->handle(path, parse_query_string(qs), r);
  }
  json get_json(const std::string& path, const std::string& qs = "") const
  {
    return json::parse(get(path, qs).body);
  }
};

std::string header(const fcgi::Response& r, const std::string& name)
{
  for (const auto& [k, v] : r.headers)
  {
    if (k == name)
    {
      return v;
    }
  }
  return {};
}

}  // namespace

TEST(drill_down_lists)
{
  Fixture fx;
  const auto c = fx.get("/v1/grib/centers");
  CHECK(c.status == 200);
  CHECK(header(c, "X-Atmo-Generation") == "7");
  CHECK(header(c, "Cache-Control") == "no-store");
  const json cj = json::parse(c.body);
  CHECK(cj["generation"] == 7);
  CHECK(cj["centers"][0]["code"] == 7);
  CHECK(cj["centers"][0]["subcenters"][0]["name"] == "ops");

  const json m = fx.get_json("/v1/grib/models", "center=1");
  CHECK(m["models"].size() == 1 && m["models"][0]["slug"] == "7-0-96");
  CHECK(fx.get_json("/v1/grib/models", "center=2")["models"].empty());

  const json g = fx.get_json("/v1/grib/grids", "model=1");
  CHECK(g["grids"].size() == 2);  // the ll grid and the reduced placeholder
  bool saw_not_viewable = false;
  for (const auto& x : g["grids"])
  {
    saw_not_viewable = saw_not_viewable || x["grid"]["viewable"] == false;
    CHECK(x["api_name"].get<std::string>().rfind("gfs_", 0) == 0);
  }
  CHECK(saw_not_viewable);

  const json r = fx.get_json("/v1/grib/runs", "model_grid=1");
  CHECK(r["runs"].size() == 1);
  CHECK(r["runs"][0]["basetime"] == "2026-11-10T00:00:00Z");
  CHECK(r["runs"][0]["member"] == -1);
  CHECK(r["runs"][0]["member_type"].is_null());

  const json p = fx.get_json("/v1/grib/parameters", "run=1");
  CHECK(p["parameters"].size() == 3);  // t, tp 1hr, tp run total
  std::vector<std::string> names;
  for (const auto& x : p["parameters"])
  {
    names.push_back(x["name"]);
  }
  CHECK(std::is_sorted(names.begin(), names.end()));
  CHECK(
    std::find(names.begin(), names.end(), "Total precipitation 1hr") !=
    names.end());

  const std::string t_sig = key_hex(fx.file.messages[0].header.sig.hash);
  const json l = fx.get_json("/v1/grib/levels", "run=1&signature=" + t_sig);
  CHECK(l["levels"].size() == 1);
  CHECK(l["levels"][0]["level1"]["type"] == 100);
  CHECK(l["levels"][0]["level1"]["value"] == 85000.0);
  CHECK(l["levels"][0]["level2"].is_null());

  const json o = fx.get_json(
    "/v1/grib/offsets", "run=1&signature=" + t_sig + "&level=100:85000");
  CHECK(o["offsets"].size() == 2);
  CHECK(o["offsets"][0]["offset_end_s"] == 3 * 3600);  // sorted by end
  CHECK(o["offsets"][0]["message"] == 101);
  CHECK(o["offsets"][1]["valid_time"] == "2026-11-10T06:00:00Z");
  CHECK(fx.get_json(
            "/v1/grib/offsets",
            "run=1&signature=" + t_sig + "&level=100:50000")["offsets"]
          .empty());
}

TEST(first_bucket_repair)
{
  Fixture fx;
  const std::string one_h = key_hex(fx.file.messages[3].header.sig.hash);
  const std::string run = key_hex(fx.file.messages[2].header.sig.hash);
  CHECK(one_h != run);
  const json o =
    fx.get_json("/v1/grib/offsets", "run=1&signature=" + one_h + "&level=1:0");
  CHECK(o["offsets"].size() == 2);
  if (o["offsets"].size() == 2)
  {
    CHECK(o["offsets"][0]["message"] == 102);  // 0-1 run total
    CHECK(o["offsets"][0]["from_run_total"] == true);
    CHECK(o["offsets"][0]["offset_end_s"] == 3600);
    CHECK(o["offsets"][1]["message"] == 103);  // 1-2 bucket
    CHECK(o["offsets"][1]["from_run_total"] == false);
    CHECK(o["offsets"][1]["offset_start_s"] == 3600);
  }
  // The run total itself is not repaired with anything.
  const json r =
    fx.get_json("/v1/grib/offsets", "run=1&signature=" + run + "&level=1:0");
  CHECK(r["offsets"].size() == 2);
  for (const auto& x : r["offsets"])
  {
    CHECK(x["from_run_total"] == false);
  }
}

TEST(errors)
{
  Fixture fx;
  CHECK(fx.get("/v1/grib/runs").status == 400);
  // A replaced message id: 410 with its replacement, on every sub-path.
  fx.cat->replaced[42] = 100;
  for (const char* p :
       {"/v1/grib/messages/42", "/v1/grib/messages/42/raw",
        "/v1/grib/messages/42/point"})
  {
    const auto r = fx.get(p, "lat=1&lon=1");
    CHECK(r.status == 410);
    const json e = json::parse(r.body)["error"];
    CHECK(e["code"] == "replaced" && e["replaced_by"] == 100);
  }
  CHECK(fx.get("/v1/grib/messages/43").status == 404);
  CHECK(fx.get("/v1/grib/runs", "model_grid=x").status == 400);
  CHECK(fx.get("/v1/grib/runs", "model_grid=99").status == 404);
  CHECK(fx.get("/v1/grib/parameters", "run=99").status == 404);
  CHECK(fx.get("/v1/grib/levels", "run=1&signature=zz").status == 400);
  CHECK(
    fx.get("/v1/grib/levels", "run=1&signature=0000000000000000").status ==
    404);
  CHECK(
    fx.get(
        "/v1/grib/offsets",
        "run=1&signature=" + key_hex(fx.file.messages[0].header.sig.hash))
      .status == 400);  // no level
  CHECK(fx.get("/v1/grib/nope").status == 404);
  CHECK(fx.get("/v1/grib/messages/999").status == 404);
  CHECK(fx.get("/v1/grib/messages/abc").status == 400);
  const json e = fx.get_json("/v1/grib/messages/999");
  CHECK(e["error"]["code"] == "not_found");
}

TEST(message_details)
{
  Fixture fx;
  const json m = fx.get_json("/v1/grib/messages/100")["message"];
  CHECK(m["id"] == 100);
  CHECK(m["model"]["slug"] == "7-0-96");
  CHECK(m["process_id"] == 96);
  CHECK(m["grid"]["viewable"] == true);
  CHECK(m["grid"]["api_name"].get<std::string>().rfind("gfs_", 0) == 0);
  CHECK(m["center"]["code"] == 7);
  CHECK(m["parameter"]["name"] == "Temperature");  // not "2 metre ..."
  CHECK(m["parameter"]["short_name"] == "");
  CHECK(m["parameter"]["canonical"].get<std::string>().rfind("v2\n", 0) == 0);
  CHECK(m["level1"]["value"] == 85000.0);
  CHECK(m["offset_end_s"] == 6 * 3600);
  CHECK(m["file"]["path"] == fx.file.path);
  CHECK(m["sampleable"] == true);
  CHECK(m["flags"].empty());
  CHECK(m["content_hash"] == key_hex(fx.file.messages[0].content_hash));
  const json r = fx.get_json("/v1/grib/messages/105")["message"];
  CHECK(r["sampleable"] == false);
  CHECK(r["flags"].size() == 1 && r["flags"][0] == "grid_unsupported");
}

TEST(point_by_message)
{
  Fixture fx;
  const json p = fx.get_json("/v1/grib/messages/100/point", "lat=50&lon=10");
  CHECK(p["status"] == "ok");
  CHECK(p["value"] == 281.25);
  CHECK(p["units"] == "K");
  CHECK(p["uv_relative_to_grid"] == false);
  CHECK(
    fx.get_json("/v1/grib/messages/101/point", "lat=50&lon=10")["value"] ==
    280.0);
  // The parameter's scale/offset apply.
  auto& s = fx.cat->sig(fx.file.messages[0].header.sig.hash);
  s.si_scale = 2.0;
  s.si_offset = 1.0;
  CHECK(
    fx.get_json("/v1/grib/messages/100/point", "lat=50&lon=10")["value"] ==
    563.5);
  const json out = fx.get_json("/v1/grib/messages/100/point", "lat=-89&lon=10");
  CHECK(out["status"] == "outside_grid" && out["value"].is_null());
  CHECK(fx.get("/v1/grib/messages/100/point", "lat=50").status == 400);
  CHECK(fx.get("/v1/grib/messages/100/point", "lat=91&lon=0").status == 400);
  const auto ns = fx.get("/v1/grib/messages/105/point", "lat=50&lon=10");
  CHECK(ns.status == 409);
  CHECK(json::parse(ns.body)["error"]["code"] == "not_sampleable");
}

TEST(tile_by_message)
{
  Fixture fx;
  const std::string cmap = "cmap=270:0000ff,lin,290:ff0000";
  const auto t = fx.get("/v1/grib/messages/100/tiles/0/0/0.png", cmap);
  CHECK(t.status == 200);
  CHECK(t.content_type == "image/png");
  CHECK(t.body.size() > 8 && t.body.compare(1, 3, "PNG") == 0);
  CHECK(header(t, "Cache-Control") == "no-cache");
  const std::string etag = header(t, "ETag");
  CHECK(!etag.empty());
  CHECK(
    fx.get("/v1/grib/messages/100/tiles/0/0/0.png", cmap, etag).status == 304);
  const std::string v = "&v=" + key_hex(fx.file.messages[0].content_hash);
  const auto pinned = fx.get("/v1/grib/messages/100/tiles/0/0/0.png", cmap + v);
  CHECK(pinned.status == 200);
  CHECK(
    header(pinned, "Cache-Control") == "public, max-age=31536000, immutable");
  CHECK(
    fx.get(
        "/v1/grib/messages/100/tiles/0/0/0.png", cmap + "&v=0000000000000000")
      .status == 404);
  CHECK(fx.get("/v1/grib/messages/100/tiles/0/0/0.png").status == 400);
  CHECK(fx.get("/v1/grib/messages/100/tiles/1/2/0.png", cmap).status == 400);
  CHECK(fx.get("/v1/grib/messages/100/tiles/0/0/0", cmap).status == 404);
  CHECK(fx.get("/v1/grib/messages/105/tiles/0/0/0.png", cmap).status == 409);
  // Different data, different tile.
  const auto t2 = fx.get("/v1/grib/messages/101/tiles/0/0/0.png", cmap);
  CHECK(t2.status == 200 && t2.body != t.body);
}

TEST(query_api_routes_grib_paths)
{
  Fixture fx;
  SnapshotLoader loader(fx.root);
  QueryApi with(fx.root, loader, 4, fx.api.get());
  QueryApi without(fx.root, loader, 4);
  fcgi::Request r;
  r.params["DOCUMENT_URI"] = "/v1/grib/centers";
  r.params["REQUEST_METHOD"] = "GET";
  // Served even though no snapshot has been published.
  CHECK(with.handle(r).status == 200);
  CHECK(without.handle(r).status == 404);
  r.params["REQUEST_METHOD"] = "POST";
  CHECK(with.handle(r).status == 405);
}

// ---- /v1/point, /v1/tiles, /v1/meta through the catalog (T17c) -------------

TEST(catalog_store_models_times_and_names)
{
  Fixture fx;
  auto st = fx.api->field_store();
  const auto models = st->models();
  CHECK(models.size() == 1);  // the reduced Gaussian grid is not viewable
  if (models.size() != 1)
  {
    return;
  }
  const std::string m = models[0];
  CHECK(m.rfind("gfs_", 0) == 0);
  const std::int64_t base = fx.file.messages[0].header.base_time_s;
  const auto times = st->times(m);
  CHECK(times.size() == 4);  // t850 3 h, 6 h; precip ending 1 h, 2 h
  CHECK(times.size() == 4 && times[0].fhr == 1 && times[3].fhr == 6);
  CHECK(times.size() == 4 && times[0].run_s == base);
  CHECK(st->grid(m, times[0]) != nullptr);
  CHECK(st->grid(m, ModelTime{base + 3600, 0}) == nullptr);

  // Models: exact, the only one containing the text, or errors.
  CHECK(!st->resolve_model(m).guess);
  CHECK(!st->resolve_model("GFS_" + m.substr(4)).guess);  // case
  const ResolvedName g = st->resolve_model("gfs");
  CHECK(g.guess && g.name == m);
  CHECK(st->resolve_model("7-0").guess);  // via the model slug
  auto code = [](auto f)
  {
    try
    {
      f();
    }
    catch (const NameError& e)
    {
      return std::to_string(e.status) + " " + e.code + " " +
             std::to_string(e.candidates.size());
    }
    return std::string("none");
  };
  CHECK(code([&] { st->resolve_model("ecmwf"); }) == "404 not_found 0");

  // Fields: normalised parameter names; partial -> guess or ambiguous.
  const ResolvedName t = st->resolve_field("Temperature");
  CHECK(!t.guess && t.name == "temperature" && t.param != 0);
  CHECK(st->param(t.param).units == "K");
  const ResolvedName tg = st->resolve_field("temp");
  CHECK(tg.guess && tg.param == t.param);
  CHECK(!st->resolve_field("total_precipitation_1hr").guess);
  CHECK(code([&] { st->resolve_field("precip"); }) == "400 ambiguous_name 2");
  CHECK(code([&] { st->resolve_field("snow"); }) == "404 not_found 0");

  // A common name from the web app becomes the field's name; the
  // parameter name still works.
  {
    CatalogTables& e = fx.cat->edit();
    for (auto& sg : e.signatures)
    {
      sg.parameter_id = sg.id;  // one grib_parameter per signature here
    }
    e.common_names.push_back({"air_temperature", t.param});
  }
  // (t.param == the temperature signature's id: ids follow the same order)
  st = fx.api->field_store();
  const ResolvedName a = st->resolve_field("AIR_temperature");
  CHECK(!a.guess && a.name == "air_temperature");
  CHECK(st->resolve_field("temperature").name == "air_temperature");
  CHECK(st->param(a.param).name == "air_temperature");
  CHECK(st->resolve_field("air_temp").guess);

  // Two parameters with one normalised name: ambiguous_field.
  {
    CatalogTables& e = fx.cat->edit();
    e.common_names.clear();
    for (auto& sg : e.signatures)
    {
      if (sg.name.rfind("Total precipitation", 0) == 0)
      {
        sg.name = "Total precipitation";
      }
    }
  }
  st = fx.api->field_store();
  CHECK(
    code([&] { st->resolve_field("total_precipitation"); }) ==
    "400 ambiguous_field 0");

  // Roles come from the GRIB2 numbers of instantaneous signatures.
  CHECK(st->param(t.param).role == ParamRole::other);
  for (const auto& [cat, num, role] :
       {std::tuple{2u, 2u, ParamRole::wind_u},
        std::tuple{2u, 3u, ParamRole::wind_v},
        std::tuple{3u, 5u, ParamRole::geopotential_height}})
  {
    CatalogTables& e = fx.cat->edit();
    for (auto& sg : e.signatures)
    {
      if (sg.id == t.param)
      {
        sg.category = cat;
        sg.number = num;
      }
    }
    st = fx.api->field_store();
    CHECK(st->param(t.param).role == role);
  }
  // ... not from statistical ones (a maximum wind is not a component).
  std::uint16_t p1 = 0;  // parameter ids follow signature ids here
  for (const auto& sg : fx.cat->tables()->signatures)
  {
    p1 = sg.stat_interval_s ? static_cast<std::uint16_t>(sg.id) : p1;
  }
  CHECK(p1 != 0);
  {
    CatalogTables& e = fx.cat->edit();
    for (auto& sg : e.signatures)
    {
      sg.category = 2;
      sg.number = 2;
    }
  }
  st = fx.api->field_store();
  CHECK(st->param(p1).role == ParamRole::other);
}

TEST(catalog_store_point_values_and_ambiguity)
{
  Fixture fx;
  auto st = fx.api->field_store();
  const std::int64_t base = fx.file.messages[0].header.base_time_s;
  SourceRegistry none;
  PointQuery q;
  q.lat_deg = 30.0;
  q.lon_deg = 10.0;
  q.t_s = base + 6 * 3600;
  q.fields = {FieldSpec::parse("temperature@850hPa")};
  q.models = {"gfs"};
  PointResult r = point(*st, nullptr, none, q);
  CHECK(r.values.size() == 1);
  CHECK(r.warnings.size() == 1 && r.warnings[0].argument == "models");
  if (r.values.size() != 1)
  {
    return;
  }
  CHECK(r.values[0].status == PointValue::Status::ok);
  CHECK(r.values[0].value == 281.25);
  CHECK(r.values[0].units == "K");
  CHECK(r.values[0].field == "temperature@850hPa");
  CHECK(r.values[0].fhr == 6.0);

  // Between f03 (280) and f06 (281.25); a partial field name is a guess.
  q.t_s = base + 4 * 3600 + 1800;
  q.fields = {FieldSpec::parse("temp@850hPa")};
  q.models.clear();
  r = point(*st, nullptr, none, q);
  CHECK(r.values.size() == 1 && r.warnings.size() == 1);
  CHECK(r.warnings.size() == 1 && r.warnings[0].used == "temperature");
  CHECK(r.values.size() == 1 && r.values[0].value == 280.625);
  const auto j = to_json(r);
  CHECK(j["warnings"][0]["code"] == "best_guess");
  CHECK(j["values"][0]["field"] == "temperature@850hPa");

  // Two signatures of one parameter at the same level and time (the 1 h
  // bucket 1-2 h and the run total 0-2 h made one parameter).
  {
    CatalogTables& e = fx.cat->edit();
    for (auto& sg : e.signatures)
    {
      sg.parameter_id =
        sg.name.rfind("Total precipitation", 0) == 0 ? 999 : sg.id;
    }
  }
  st = fx.api->field_store();
  q.t_s = base + 2 * 3600;
  q.fields = {FieldSpec::parse("total_precipitation_run_total@sfc")};
  r = point(*st, nullptr, none, q);
  CHECK(
    r.values.size() == 1 &&
    r.values[0].status == PointValue::Status::ambiguous_field);
  q.t_s = base + 3600;  // only the run total ends at 1 h
  r = point(*st, nullptr, none, q);
  CHECK(r.values.size() == 1 && r.values[0].status == PointValue::Status::ok);
  CHECK(r.values.size() == 1 && r.values[0].value == 1.0);
}

TEST(catalog_store_first_bucket_repair)
{
  // At f01 GFS-style files have no "1 h" bucket: the 0-1 h interval is
  // labelled since run start.  /v1/point uses that run-total message for
  // the 1 h parameter (Q22c), as /v1/grib/offsets lists it.
  Fixture fx;
  auto st = fx.api->field_store();
  const std::int64_t base = fx.file.messages[0].header.base_time_s;
  SourceRegistry none;
  PointQuery q;
  q.lat_deg = 30.0;
  q.lon_deg = 10.0;
  q.fields = {
    FieldSpec::parse("total_precipitation_1hr@sfc"),
    FieldSpec::parse("total_precipitation_run_total@sfc")};
  q.t_s = base + 3600;
  PointResult r = point(*st, nullptr, none, q);
  CHECK(r.values.size() == 2);
  if (r.values.size() != 2)
  {
    return;
  }
  CHECK(r.values[0].status == PointValue::Status::ok);
  CHECK(r.values[0].value == 1.0);  // the run total 0-1 h
  CHECK(r.values[1].value == 1.0);
  // A real bucket wins where there is one: 1-2 h = 2, run total 0-2 h = 3.
  q.t_s = base + 2 * 3600;
  r = point(*st, nullptr, none, q);
  CHECK(r.values.size() == 2 && r.values[0].value == 2.0);
  CHECK(r.values.size() == 2 && r.values[1].value == 3.0);
  // Between f01 and f02 the 1 h bucket interpolates repaired -> real.
  q.t_s = base + 3600 + 1800;
  r = point(*st, nullptr, none, q);
  CHECK(r.values.size() == 2 && r.values[0].value == 1.5);
  // Tiles see it too.
  const std::string m = st->models().at(0);
  const ResolvedName p = st->resolve_field("total_precipitation_1hr");
  CHECK(st->view(m, ModelTime{base, 1}, FieldKey{p.param, 1, 0.0f}) != nullptr);
  bool listed = false;
  for (const auto& f : st->fields(m, ModelTime{base, 1}))
  {
    listed = listed || f.param_id == p.param;
  }
  CHECK(listed);
}

TEST(query_api_point_meta_tiles_through_the_catalog)
{
  Fixture fx;
  {
    ManifestPublisher pub(fx.root);
    Manifest m;
    m.sources = SourceRegistry::defaults();
    pub.publish(m);
  }
  SnapshotLoader loader(fx.root);
  loader.refresh();
  QueryApi api(fx.root, loader, 4, fx.api.get());
  const std::string m = fx.api->field_store()->models().at(0);
  auto get = [&](const std::string& path, const std::string& qs)
  {
    fcgi::Request r;
    r.params["DOCUMENT_URI"] = path;
    r.params["QUERY_STRING"] = qs;
    r.params["REQUEST_METHOD"] = "GET";
    return api.handle(r);
  };
  const auto meta = json::parse(get("/v1/meta", "").body);
  CHECK(meta["models"].size() == 1 && meta["models"][0]["name"] == m);
  CHECK(meta["models"][0]["fields"] == json::array({"temperature@850hPa"}));

  auto r = get(
    "/v1/point",
    "lat=30&lon=10&time=2026-11-10T06:00:00Z&fields=temperature@850hPa&"
    "models=" +
      m);
  CHECK(r.status == 200);
  json j = json::parse(r.body);
  CHECK(j["values"][0]["value"] == 281.25 && !j.contains("warnings"));
  r = get(
    "/v1/point",
    "lat=30&lon=10&time=2026-11-10T06:00:00Z&fields=temperature@850hPa&"
    "models=gfs");
  j = json::parse(r.body);
  CHECK(r.status == 200 && j["warnings"][0]["used"] == m);
  r = get(
    "/v1/point", "lat=30&lon=10&time=2026-11-10T06:00:00Z&fields=precip@sfc");
  j = json::parse(r.body);
  CHECK(r.status == 400 && j["error"]["code"] == "ambiguous_name");
  CHECK(j["error"]["candidates"].size() == 2);
  r =
    get("/v1/point", "lat=30&lon=10&time=2026-11-10T06:00:00Z&fields=snow@sfc");
  CHECK(r.status == 404);

  const std::string cmap = "cmap=270:000000,lin,290:ffffff";
  r =
    get("/v1/tiles/" + m + "/temperature@850hPa/2026111000/6/0/0/0.png", cmap);
  CHECK(r.status == 200 && r.content_type == "image/png");
  CHECK(header(r, "Cache-Control") == "no-cache");
  CHECK(header(r, "X-Atmo-Warning").empty());
  r = get("/v1/tiles/gfs/temp@850hPa/2026111000/6/0/0/0.png", cmap);
  CHECK(r.status == 200 && header(r, "Cache-Control") == "no-store");
  CHECK(header(r, "X-Atmo-Warning").find("best_guess") == 0);
  r = get("/v1/tiles/nope/temperature@850hPa/2026111000/6/0/0/0.png", cmap);
  CHECK(r.status == 404);
}

TEST(tree_grid_details_and_downloads)
{
  Fixture fx;
  json tr = fx.get_json("/v1/grib/tree");
  const json& models = tr["centers"][0]["subcenters"][0]["models"];
  CHECK(models.size() == 1 && models[0]["slug"] == "7-0-96");
  const json& grids = models[0]["grids"];
  CHECK(grids.size() == 2);
  std::uint64_t reduced_grid = 0;
  for (const auto& g : grids)
  {
    CHECK(g["api_name"].get<std::string>().rfind("gfs_", 0) == 0);
    if (g["viewable"] == false)
    {
      reduced_grid = g["grid_id"];
      continue;
    }
    CHECK(g["runs"].size() == 1);
    const json& r = g["runs"][0];
    CHECK(r["member"] == -1 && r["parameters"] == 3);
    CHECK(r["first_offset_s"] == 3600 && r["last_offset_s"] == 6 * 3600);
    CHECK(!r.contains("parameter_list"));
  }
  CHECK(reduced_grid != 0);
  tr = fx.get_json("/v1/grib/tree", "depth=models");
  CHECK(!tr["centers"][0]["subcenters"][0]["models"][0].contains("grids"));
  tr = fx.get_json("/v1/grib/tree", "parameters=1");
  bool saw_t = false;
  for (const auto& g : tr["centers"][0]["subcenters"][0]["models"][0]["grids"])
  {
    for (const auto& p : g["runs"][0]["parameter_list"])
    {
      saw_t =
        saw_t || (p["field"] == "temperature" && p["name"] == "Temperature");
    }
  }
  CHECK(saw_t);
  CHECK(fx.get("/v1/grib/tree", "depth=x").status == 400);

  // Grid details.
  const json g = fx.get_json("/v1/grib/grids/" + std::to_string(reduced_grid));
  CHECK(g["grid"]["viewable"] == false && g["grid"]["summary"].is_object());
  CHECK(fx.get("/v1/grib/grids/999").status == 404);

  // Raw message: exactly the stored bytes, any message.
  const CatalogMessage& red = fx.file.messages[5];
  auto r = fx.get("/v1/grib/messages/105/raw");
  CHECK(r.status == 200 && r.content_type == "application/x-grib2");
  CHECK(r.body.size() == red.byte_length && r.body.rfind("GRIB", 0) == 0);
  CHECK(fnv1a64(r.body.data(), r.body.size()) == red.content_hash);
  CHECK(
    header(r, "Content-Disposition").find("105.grib2") != std::string::npos);
  CHECK(header(r, "Cache-Control") == "no-cache");
  const std::string etag = header(r, "ETag");
  CHECK(fx.get("/v1/grib/messages/105/raw", "", etag).status == 304);
  CHECK(
    header(
      fx.get("/v1/grib/messages/105/raw", "v=" + key_hex(red.content_hash)),
      "Cache-Control") == "public, max-age=31536000, immutable");
  CHECK(
    fx.get("/v1/grib/messages/105/raw", "v=0000000000000000").status == 404);

  // Values: need a decoder; then any decodable message, file order.
  CHECK(fx.get("/v1/grib/messages/105/values").status == 503);
  GribApi with(fx.root, *fx.cat, &fx.cache, atmo::grib_values);
  fcgi::Request req;
  req.params["REQUEST_METHOD"] = "GET";
  r = with.handle("/v1/grib/messages/105/values", {}, req);
  CHECK(r.status == 200 && r.content_type == "application/octet-stream");
  const std::size_t n = std::stoul(header(r, "X-Atmo-Count"));
  CHECK(n > 0 && r.body.size() == 4 * n);
  float first = 0.0f;
  std::memcpy(&first, r.body.data(), 4);  // little-endian host
  CHECK(first == 5.0f);
  CHECK(header(r, "X-Atmo-Grid") == std::to_string(reduced_grid));
  CHECK(header(r, "X-Atmo-Units") == "K");
  r = with.handle("/v1/grib/messages/100/values", {}, req);
  const json d = fx.get_json("/v1/grib/grids/1");
  CHECK(
    r.status == 200 &&
    std::stoul(header(r, "X-Atmo-Count")) ==
      d["grid"]["ni"].get<std::size_t>() * d["grid"]["nj"].get<std::size_t>());
  std::memcpy(&first, r.body.data() + r.body.size() - 4, 4);
  CHECK(first == 281.25f);
}

int main()
{
  return tst::run_all();
}
