// Query daemon HTTP API (T10): routing, parameters, errors, snapshot
// pinning, and the CURRENT watcher.
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atmo/fieldfile.h"
#include "atmo/gridstore.h"
#include "atmo/http_api.h"
#include "atmo/manifest.h"
#include "atmo/obs_segment.h"
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
        ("atmo_test_api_" + std::to_string(::getpid()) + "_" +
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

constexpr std::int64_t kRun = 1762300800;  // 2025-11-05 00Z

fcgi::Request get(
  const std::string& path,
  const std::string& qs = "",
  const std::string& method = "GET")
{
  fcgi::Request r;
  r.params["DOCUMENT_URI"] = path;
  r.params["QUERY_STRING"] = qs;
  r.params["REQUEST_METHOD"] = method;
  return r;
}

std::string header(const fcgi::Response& r, const std::string& name)
{
  for (const auto& [k, v] : r.headers)
    if (k == name)
      return v;
  return "";
}

nlohmann::json body(const fcgi::Response& r)
{
  return nlohmann::json::parse(r.body);
}

// A store with an HRRR-like model (two forecast hours, value 280 + fhr
// everywhere) and one METAR; returns the field file paths.
struct Store
{
  TempDir td;
  ManifestPublisher pub{td.p};
  Manifest m;
  std::vector<fs::path> files;

  Store()
  {
    GridIdentity id;
    const std::string c = "api test box";
    id.canon.assign(c.begin(), c.end());
    id.ni = 11;
    id.nj = 11;
    GridData d;
    for (int j = 0; j < 11; ++j)
      for (int i = 0; i < 11; ++i)
      {
        d.lat_deg.push_back(30.0 + j);
        d.lon_deg.push_back(-100.0 + i);
      }
    GridStore gs(td.p / "grids");
    auto g = gs.put(id, d);
    m.sources = SourceRegistry::defaults();
    m.grids.push_back(
      GridEntry{
        g->key(), "grids/" + key_hex(g->key()) + ".grid",
        gs.header_crc(g->key())});
    for (std::uint32_t fhr : {0u, 1u})
    {
      const std::string rel = FieldFile::relative_path("hrrr", kRun, fhr);
      FieldFileWriter w(td.p / rel, *g, "hrrr", kRun, fhr);
      FieldInfo f;
      f.param_id = var::kAirTemperature;
      f.level_type = 103;
      f.level_value = 2.0f;
      w.add_field(
        f, std::vector<float>(g->size(), 280.0f + static_cast<float>(fhr)));
      w.seal();
      files.push_back(td.p / rel);
      m.fields.push_back(
        FieldEntry::from_file(td.p, *FieldFile::open(td.p / rel)));
    }
    ObsSegmentWriter sw(m.sources.at("metar"), kRun);
    ObsReport rep;
    rep.report_key = 1;
    rep.receipt_seq = 1;
    rep.receipt_time_ms = (kRun + 120) * 1000;
    rep.obs_time_s = kRun + 60;
    rep.lat_deg = 35.4f;
    rep.lon_deg = -97.6f;
    rep.height_m = 392.0f;
    rep.values.push_back(ObsValue{var::kAirTemperature, 281.0f, 0});
    rep.raw = "KOKC 050001Z 18010KT 10SM CLR 08/02 A3000";
    sw.add_report(rep);
    const fs::path seg = td.p / "obs/metar" / std::to_string(kRun) / "a.seg";
    sw.seal(seg);
    m.segments.push_back(
      SegmentEntry::from_segment(td.p, *ObsSegment::open(seg)));
    m.next_receipt_seq = 2;
    m = pub.publish(m);
  }
};

const std::string kPointQs =
  "lat=35.4&lon=-97.6&time=2025-11-05T00:30:00Z&fields=air_temperature@2m";

}  // namespace

TEST(api_query_string)
{
  auto q = parse_query_string("a=1&b=x%20y+z&c=&d&a=2&e=%2C%40");
  CHECK(q["a"] == "2");
  CHECK(q["b"] == "x y z");
  CHECK(q.count("c") && q["c"].empty());
  CHECK(q.count("d") && q["d"].empty());
  CHECK(q["e"] == ",@");
  CHECK(parse_query_string("").empty());
  CHECK(parse_query_string("&&").empty());
  for (const char* bad : {"a=%", "a=%2", "a=%zz", "%G1=x"})
    CHECK(throws<std::invalid_argument>([&] { parse_query_string(bad); }));
}

TEST(api_without_snapshot)
{
  TempDir td;
  SnapshotLoader l(td.p);
  QueryApi api(td.p, l);
  auto r = api.handle(get("/healthz"));
  CHECK(r.status == 503 && r.content_type == "text/plain");
  r = api.handle(get("/v1/meta"));
  CHECK(r.status == 503);
  CHECK(body(r)["error"]["code"] == "no_snapshot");
  CHECK(api.handle(get("/v1/nope")).status == 404);
  CHECK(api.handle(get("/v1/meta", "", "POST")).status == 405);
  CHECK(api.handle(get("/v1/meta", "x=%zz")).status == 400);
}

TEST(api_meta_point_and_errors)
{
  Store s;
  SnapshotLoader l(s.td.p);
  CHECK(l.refresh());
  QueryApi api(s.td.p, l);

  auto r = api.handle(get("/healthz"));
  CHECK(r.status == 200 && r.body == "ok snapshot 1\n");

  r = api.handle(get("/v1/meta"));
  CHECK(r.status == 200);
  CHECK(header(r, "X-Atmo-Snapshot") == "1");
  CHECK(header(r, "Cache-Control") == "no-store");
  CHECK(body(r)["models"][0]["name"] == "hrrr");

  r = api.handle(get("/v1/point", kPointQs + "&obs=on&obs_radius_km=60"));
  CHECK(r.status == 200);
  const auto j = body(r);
  CHECK(j["snapshot"] == 1);
  CHECK(j["point"]["time"] == "2025-11-05T00:30:00Z");
  CHECK(j["values"][0]["value"].get<double>() == 280.5);
  CHECK(j["values"][0]["fhr"].get<double>() == 0.5);
  // 29 minutes away counts ~48 km in the METAR metric (Q11e).
  CHECK(j["obs"].size() == 1);
  if (j["obs"].size() == 1)
    CHECK(j["obs"][0]["raw"] == "KOKC 050001Z 18010KT 10SM CLR 08/02 A3000");
  // Deterministic.
  CHECK(
    api.handle(get("/v1/point", kPointQs + "&obs=on&obs_radius_km=60")).body ==
    r.body);
  // SCRIPT_NAME + PATH_INFO when DOCUMENT_URI is absent; HEAD.
  fcgi::Request alt;
  alt.params["SCRIPT_NAME"] = "/v1";
  alt.params["PATH_INFO"] = "/point";
  alt.params["QUERY_STRING"] = kPointQs;
  alt.params["REQUEST_METHOD"] = "HEAD";
  r = api.handle(alt);
  CHECK(r.status == 200 && r.body.empty());

  // Caller mistakes -> 400 with a message.
  for (const std::string& qs :
       {std::string("lon=-97&time=2025-11-05T00:00Z&fields=air_temperature"),
        std::string("lat=35&lon=-97&fields=air_temperature@2m"),
        std::string("lat=35&lon=-97&time=yesterday&fields=air_temperature"),
        std::string("lat=35&lon=-97&time=2025-11-05T00:00Z"),
        std::string("lat=35&lon=-97&time=2025-11-05T00:00Z&fields=nope"),
        std::string("lat=abc&lon=-97&time=2025-11-05T00:00Z&fields=cape"),
        kPointQs + "&level_hpa=500&alt_m=100", kPointQs + "&models=gfs",
        kPointQs + "&obs=maybe", kPointQs + "&obs_radius_km=0",
        kPointQs + "&as_of=-1", kPointQs + "&snapshot=0"})
  {
    r = api.handle(get("/v1/point", qs));
    CHECK(r.status == 400);
    CHECK(body(r)["error"]["code"] == "bad_request");
    CHECK(!body(r)["error"]["message"].get<std::string>().empty());
  }
}

TEST(api_snapshot_pinning)
{
  Store s;
  SnapshotLoader l(s.td.p);
  CHECK(l.refresh());
  QueryApi api(s.td.p, l);

  // Publish seq 2 without the f001 file; seq 1 stays retained.
  Manifest m2 = s.m;
  m2.fields.pop_back();
  s.pub.publish(m2);
  CHECK(l.refresh());
  auto r = api.handle(get("/v1/point", kPointQs));
  CHECK(header(r, "X-Atmo-Snapshot") == "2");
  CHECK(body(r)["values"][0]["status"] == "no_time");  // only f000 left

  r = api.handle(get("/v1/point", kPointQs + "&snapshot=1"));
  CHECK(r.status == 200);
  CHECK(header(r, "X-Atmo-Snapshot") == "1");
  CHECK(header(r, "Cache-Control") == "public, max-age=86400");
  CHECK(body(r)["values"][0]["value"].get<double>() == 280.5);
  // Pinning the current one is allowed too.
  CHECK(
    header(api.handle(get("/v1/meta", "snapshot=2")), "X-Atmo-Snapshot") ==
    "2");
  // Not retained.
  r = api.handle(get("/v1/meta", "snapshot=999"));
  CHECK(r.status == 404 && body(r)["error"]["code"] == "unknown_snapshot");
  // Files of seq 1 gone (expired): 410 for an API that has not cached it.
  fs::remove(s.files[1]);
  QueryApi fresh(s.td.p, l);
  r = fresh.handle(get("/v1/meta", "snapshot=1"));
  CHECK(r.status == 410 && body(r)["error"]["code"] == "snapshot_gone");
}

TEST(api_watcher_follows_current)
{
  TempDir td;
  SnapshotLoader l(td.p);
  std::vector<std::string> errors;
  // Long poll interval: picking up publishes quickly proves inotify works
  // (after the manifest directory exists).
  SnapshotWatcher w(
    td.p, l, [&](const std::string& e) { errors.push_back(e); },
    std::chrono::milliseconds(3000));
  w.start();
  ManifestPublisher pub(td.p);  // creates manifest/
  Manifest m;
  m.sources = SourceRegistry::defaults();
  pub.publish(m);
  auto wait_for = [&](std::uint64_t seq, int ms)
  {
    for (int k = 0; k < ms / 10; ++k)
    {
      auto s = l.current();
      if (s && s->seq() >= seq)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
  };
  CHECK(wait_for(1, 4000));  // may need one poll to start watching
  const auto t0 = std::chrono::steady_clock::now();
  pub.publish(m);
  CHECK(wait_for(2, 2000));
  CHECK(
    std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
  w.stop();
  CHECK(errors.empty());
}

TEST(api_png_tiles)
{
  Store s;
  SnapshotLoader l(s.td.p);
  CHECK(l.refresh());
  QueryApi api(s.td.p, l);
  const std::string base = "/v1/tiles/hrrr/air_temperature@2m/2025110500/0/";
  const std::string cmap = "cmap=270:000000,lin,290:ffffff";

  auto r = api.handle(get(base + "6/15/25.png", cmap));
  CHECK(r.status == 200);
  CHECK(r.content_type == "image/png");
  CHECK(r.body.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0);
  CHECK(header(r, "Cache-Control") == "public, max-age=31536000, immutable");
  CHECK(header(r, "X-Atmo-Snapshot") == "1");
  const std::string etag = header(r, "ETag");
  CHECK(etag.size() == 18 && etag.front() == '"');
  // Same request, same bytes; If-None-Match -> 304 without a body.
  CHECK(api.handle(get(base + "6/15/25.png", cmap)).body == r.body);
  fcgi::Request cond = get(base + "6/15/25.png", cmap);
  cond.params["HTTP_IF_NONE_MATCH"] = etag;
  auto r304 = api.handle(cond);
  CHECK(
    r304.status == 304 && r304.body.empty() && header(r304, "ETag") == etag);
  // Another colormap is another image.
  auto other =
    api.handle(get(base + "6/15/25.png", "cmap=270:ff0000,lin,290:00ff00"));
  CHECK(other.status == 200 && header(other, "ETag") != etag);
  // HEAD: headers only.
  CHECK(api.handle(get(base + "6/15/25.png", cmap, "HEAD")).body.empty());
  // Outside the grid: 200 with a transparent tile, not an error.
  CHECK(api.handle(get(base + "6/40/20.png", cmap)).status == 200);

  auto code = [&](const std::string& path, const std::string& qs)
  {
    auto x = api.handle(get(path, qs));
    return std::to_string(x.status) + " " +
           (x.status >= 400 ? body(x)["error"]["code"].get<std::string>() : "");
  };
  CHECK(code(base + "6/15/25.png", "") == "400 bad_request");  // no cmap
  CHECK(code(base + "6/15/25.png", "cmap=1:000000") == "400 bad_request");
  CHECK(code(base + "6/15/25.png", cmap + "&nodata=xyz") == "400 bad_request");
  CHECK(
    code("/v1/tiles/hrrr/air_temperature/2025110500/0/6/15/25.png", cmap) ==
    "400 bad_request");  // no @level
  CHECK(
    code("/v1/tiles/hrrr/air_temperature@2m/20251105/0/6/15/25.png", cmap) ==
    "400 bad_request");                                          // run format
  CHECK(code(base + "6/64/25.png", cmap) == "400 bad_request");  // x >= 2^z
  CHECK(code(base + "6/15/x.png", cmap) == "400 bad_request");
  CHECK(
    code("/v1/tiles/gfs/air_temperature@2m/2025110500/0/6/15/25.png", cmap) ==
    "404 not_found");
  CHECK(
    code("/v1/tiles/hrrr/air_temperature@2m/2025110500/5/6/15/25.png", cmap) ==
    "404 no_time");
  CHECK(
    code("/v1/tiles/hrrr/wind_u@10m/2025110500/0/6/15/25.png", cmap) ==
    "404 no_field");
  CHECK(code(base + "6/15/25.jpg", cmap) == "404 not_found");
  CHECK(code("/v1/tiles/hrrr", cmap) == "404 not_found");
}

int main()
{
  return tst::run_all();
}
