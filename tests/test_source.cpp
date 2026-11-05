#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "atmo/source.h"
#include "check.h"

using namespace atmo;

namespace
{

template <class F>
bool throws_invalid(F&& f)
{
  try
  {
    f();
  }
  catch (const std::invalid_argument&)
  {
    return true;
  }
  catch (...)
  {
    return false;
  }
  return false;
}

const SourceClass* find(const SourceRegistry& r, const std::string& name)
{
  return r.find(name);
}

}  // namespace

// Suggested starting values from docs/design/01-obs-store.md; the Symfony
// seed migration uses the same values.
TEST(defaults_match_spec)
{
  const SourceRegistry r = SourceRegistry::defaults();
  CHECK(r.size() == 5);
  struct Want
  {
    const char* name;
    double L, tau;
    std::int64_t bucket;
  } want[] = {
    {"metar", 100, 3600, 3600},
    {"synop", 150, 10800, 3600},
    {"sounding", 250, 21600, 43200},
    {"pirep", 100, 3600, 3600},
    {"aircraft", 100, 1800, 3600}};
  for (const auto& w : want)
  {
    const SourceClass* s = find(r, w.name);
    CHECK(s != nullptr);
    if (!s)
      continue;
    CHECK(s->L_km == w.L);
    CHECK(s->tau_seconds == w.tau);
    CHECK(s->bucket_seconds == w.bucket);
    CHECK(s->vertical_scale == 10.0);
    CHECK(s->retain_seconds == 259200);  // 72 h (D1)
    CHECK(s->lambda() == w.L / w.tau);
  }
  // The spec's example config line, verbatim.
  const SourceClass m = SourceClass::from_text(
    R"({ "name": "metar", "bucket_seconds": 3600, "L_km": 100, "tau_seconds": 3600,
           "vertical_scale": 10, "retain_seconds": 259200 })");
  CHECK(m == *find(r, "metar"));
  CHECK(find(r, "nope") == nullptr);
}

TEST(lambda_buckets_and_embedding)
{
  SourceClass s = *SourceRegistry::defaults().find("synop");
  CHECK_NEAR(s.lambda(), 150.0 / 10800.0, 0.0);
  // tau seconds of time costs L_km.
  const Embedding e = s.make_embedding(1700000000 - 1700000000 % 3600);
  CHECK(e.t0_s == static_cast<double>(1700000000 - 1700000000 % 3600));
  CHECK(e.km_per_second == s.lambda());
  CHECK(e.vertical_scale == s.vertical_scale);
  const auto a = embed(10, 20, 0, e.t0_s, e);
  const auto b = embed(10, 20, 0, e.t0_s + s.tau_seconds, e);
  CHECK_NEAR(b[3] - a[3], s.L_km, 1e-9);

  CHECK(s.bucket_start_s(3600) == 3600);
  CHECK(s.bucket_start_s(3599) == 0);
  CHECK(s.bucket_start_s(7199) == 3600);
  CHECK(s.bucket_start_s(-1) == -3600);  // floor, not truncation
  CHECK(s.bucket_start_s(-3600) == -3600);

  // Embedding must be refused for a time that is not a bucket start.
  CHECK(throws_invalid([&] { (void)s.make_embedding(1); }));
}

// A complete export as Symfony writes it (docs/design/05-reference-data.md).
nlohmann::json good_export()
{
  return nlohmann::json::parse(R"({
    "format": "atmo-sources", "version": 1,
    "exported_at": "2026-11-05T12:00:00Z",
    "sources": [
      {"name": "metar", "bucket_seconds": 3600, "L_km": 100,
       "tau_seconds": 1800, "vertical_scale": 10, "retain_seconds": 259200},
      {"name": "buoy", "bucket_seconds": 7200, "L_km": 200,
       "tau_seconds": 10800, "vertical_scale": 1, "retain_seconds": 86400}
    ]
  })");
}

TEST(export_loads)
{
  const SourceRegistry r = SourceRegistry::from_json(good_export());
  CHECK(r.size() == 2);  // only the exported classes, not the built-ins
  const SourceClass* m = r.find("metar");
  CHECK(m && m->tau_seconds == 1800 && m->L_km == 100);
  const SourceClass* b = r.find("buoy");
  CHECK(b && b->bucket_seconds == 7200 && b->vertical_scale == 1);
  // Order is preserved as exported.
  CHECK(r.all()[0].name == "metar" && r.all()[1].name == "buoy");
  // Round trip through to_json (which writes format and version too).
  const SourceRegistry r2 = SourceRegistry::from_json(r.to_json());
  CHECK(
    r2.size() == 2 && r2.all()[0] == r.all()[0] && r2.all()[1] == r.all()[1]);
  CHECK(r.to_json()["format"] == "atmo-sources");
  CHECK(r.to_json()["version"] == 1);
}

TEST(bad_config_rejected)
{
  // Each case changes one thing in an otherwise good export.
  auto bad = [](auto mutate)
  {
    nlohmann::json j = good_export();
    mutate(j);
    return throws_invalid([&] { (void)SourceRegistry::from_json(j); });
  };
  auto field = [&](const char* key, nlohmann::json v)
  { return bad([&](nlohmann::json& j) { j["sources"][0][key] = v; }); };
  CHECK(!bad([](nlohmann::json&) {}));  // baseline is accepted

  CHECK(field("tau_seconds", 0));
  CHECK(field("tau_seconds", -5));
  CHECK(field("L_km", 0));
  CHECK(field("L_km", "100"));
  CHECK(field("bucket_seconds", 0));
  CHECK(field("bucket_seconds", 3600.5));
  CHECK(field("retain_seconds", -1));
  CHECK(field("vertical_scale", -1));
  CHECK(!field("vertical_scale", 0));  // 0 = height ignored, allowed
  // Names are used as directory names: obs/<source>/...
  CHECK(field("name", ""));
  CHECK(field("name", "../etc"));
  CHECK(field("name", "Metar"));
  CHECK(field("name", "abcdefghijklmnopqrstuvwxyz0123456"));
  CHECK(field("name", 7));
  // Every field is required (rows from MySQL are always complete), even for
  // a class whose name is built in.
  for (const char* key :
       {"name", "bucket_seconds", "L_km", "tau_seconds", "vertical_scale",
        "retain_seconds"})
  {
    CHECK(bad([&](nlohmann::json& j) { j["sources"][0].erase(key); }));
  }
  // Unknown keys are typos, not silently ignored.
  CHECK(field("tau_secs", 3600));
  // Duplicate names.
  CHECK(bad([](nlohmann::json& j) { j["sources"][1]["name"] = "metar"; }));
  // Wrong or missing file marker: e.g. a stations export by mistake.
  CHECK(bad([](nlohmann::json& j) { j["format"] = "atmo-stations"; }));
  CHECK(bad([](nlohmann::json& j) { j.erase("format"); }));
  CHECK(bad([](nlohmann::json& j) { j["version"] = 2; }));
  CHECK(bad([](nlohmann::json& j) { j.erase("version"); }));
  // Structural problems.
  CHECK(
    bad([](nlohmann::json& j) { j["sources"] = nlohmann::json::object(); }));
  CHECK(bad([](nlohmann::json& j) { j["sources"] = nlohmann::json::array(); }));
  CHECK(bad([](nlohmann::json& j) { j = nlohmann::json::array({1, 2}); }));
  CHECK(throws_invalid(
    [] { (void)SourceRegistry::from_text(R"({"format":)"); }));  // malformed
}

TEST(load_from_file)
{
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
                       ("atmo_test_source_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  {
    std::ofstream(dir / "sources.json") << good_export().dump(2);
  }
  const SourceRegistry r = SourceRegistry::from_file(dir / "sources.json");
  CHECK(r.size() == 2 && r.at("buoy").bucket_seconds == 7200);
  CHECK(throws_invalid(
    [&] { (void)SourceRegistry::from_file(dir / "missing.json"); }));
  bool oor = false;
  try
  {
    (void)r.at("sounding");
  }
  catch (const std::out_of_range&)
  {
    oor = true;
  }
  CHECK(oor);
  fs::remove_all(dir);
}

int main()
{
  return tst::run_all();
}
