#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "atmo/source.hpp"
#include "check.hpp"

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

// Suggested starting values from docs/design/01-obs-store.md.
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

TEST(bad_config_rejected)
{
  auto bad = [](const char* js)
  { return throws_invalid([&] { (void)SourceRegistry::from_text(js); }); };
  // Baseline is accepted.
  CHECK(!bad(R"({"sources":[{"name":"x","bucket_seconds":60,"L_km":1,
      "tau_seconds":1,"vertical_scale":0,"retain_seconds":60}]})"));
  // Non-positive / non-finite tau.
  CHECK(bad(R"({"sources":[{"name":"metar","tau_seconds":0}]})"));
  CHECK(bad(R"({"sources":[{"name":"metar","tau_seconds":-5}]})"));
  // Duplicate names.
  CHECK(bad(R"({"sources":[{"name":"metar"},{"name":"metar","L_km":50}]})"));
  // Other invalid values.
  CHECK(bad(R"({"sources":[{"name":"metar","L_km":0}]})"));
  CHECK(bad(R"({"sources":[{"name":"metar","bucket_seconds":0}]})"));
  CHECK(bad(R"({"sources":[{"name":"metar","bucket_seconds":3600.5}]})"));
  CHECK(bad(R"({"sources":[{"name":"metar","retain_seconds":-1}]})"));
  CHECK(bad(R"({"sources":[{"name":"metar","vertical_scale":-1}]})"));
  CHECK(bad(R"({"sources":[{"name":"metar","L_km":"100"}]})"));
  // Names are used as directory names: obs/<source>/...
  CHECK(bad(R"({"sources":[{"name":""}]})"));
  CHECK(bad(R"({"sources":[{"name":"../etc"}]})"));
  CHECK(bad(R"({"sources":[{"name":"Metar"}]})"));
  CHECK(bad(R"({"sources":[{"name":"abcdefghijklmnopqrstuvwxyz0123456"}]})"));
  // Unknown keys are typos, not silently ignored.
  CHECK(bad(R"({"sources":[{"name":"metar","tau_secs":3600}]})"));
  // A class that is not a built-in needs every field.
  CHECK(bad(R"({"sources":[{"name":"buoy","L_km":100}]})"));
  // Structural problems.
  CHECK(bad(R"({"sources":{}})"));
  CHECK(bad(R"({"sources":[]})"));
  CHECK(bad(R"([1,2])"));
  CHECK(bad(R"({"sources":[{"name":"metar"})"));  // malformed JSON
}

TEST(partial_config_fills_from_builtin_defaults)
{
  const SourceRegistry r = SourceRegistry::from_text(R"({
    "sources": [
      {"name": "metar", "tau_seconds": 1800},
      {"name": "buoy", "bucket_seconds": 7200, "L_km": 200, "tau_seconds": 10800,
       "vertical_scale": 1, "retain_seconds": 86400}
    ],
    "something_else": {"ignored": "by the source loader"}
  })");
  CHECK(r.size() == 2);  // only the configured classes, not all built-ins
  const SourceClass* m = r.find("metar");
  CHECK(
    m && m->tau_seconds == 1800 && m->L_km == 100 && m->bucket_seconds == 3600);
  const SourceClass* b = r.find("buoy");
  CHECK(b && b->bucket_seconds == 7200 && b->vertical_scale == 1);
  // Order is preserved as configured.
  CHECK(r.all()[0].name == "metar" && r.all()[1].name == "buoy");
  // A bare array is accepted too.
  CHECK(SourceRegistry::from_text(R"([{"name":"pirep"}])").size() == 1);

  // Round trip through to_json.
  const SourceRegistry r2 = SourceRegistry::from_json(r.to_json());
  CHECK(
    r2.size() == 2 && r2.all()[0] == r.all()[0] && r2.all()[1] == r.all()[1]);
}

TEST(load_from_file)
{
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
                       ("atmo_test_source_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  {
    std::ofstream(dir / "atmo.json") << R"({"sources":[{"name":"sounding"}]})";
  }
  const SourceRegistry r = SourceRegistry::from_file(dir / "atmo.json");
  CHECK(r.size() == 1 && r.at("sounding").bucket_seconds == 43200);
  CHECK(throws_invalid(
    [&] { (void)SourceRegistry::from_file(dir / "missing.json"); }));
  bool oor = false;
  try
  {
    (void)r.at("metar");
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
