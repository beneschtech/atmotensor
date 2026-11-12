// GRIB-native store without eccodes (T10c): index format, the decoded
// message cache (LRU purge, in-use protection, coalescing, RSS trigger),
// and GRIB entries in the manifest, retention and gc.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atmo/crc32.h"
#include "atmo/gribstore.h"
#include "atmo/manifest.h"
#include "atmo/retention.h"
#include "atmo/source.h"
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
        ("atmo_test_gribstore_" + std::to_string(::getpid()) + "_" +
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

GribIndexEntry entry(
  std::uint64_t hash,
  std::uint32_t fhr = 0,
  std::uint16_t param = var::kAirTemperature,
  float level = 2.0f)
{
  GribIndexEntry e;
  e.offset = hash * 100;
  e.length = 100;
  e.hash = hash;
  e.info.param_id = param;
  e.info.level_type = 103;
  e.info.level_value = level;
  e.fhr = fhr;
  e.grid = 0xabc;
  return e;
}

// Decoder producing `n` doubles per message, counting calls.
struct FakeDecoder
{
  std::size_t n = 1000;
  std::atomic<int> calls{0};
  std::chrono::milliseconds delay{0};
  std::uint64_t fail_hash = 0;

  GribDecoder fn()
  {
    return
      [this](
        const fs::path&, const GribIndexEntry& e, std::shared_ptr<const Grid> g)
    {
      ++calls;
      if (delay.count())
        std::this_thread::sleep_for(delay);
      if (e.hash == fail_hash)
        throw std::runtime_error("decode failed");
      auto d = std::make_shared<GribMessageData>();
      d->grid = std::move(g);
      d->values.assign(n, static_cast<double>(e.hash));
      return d;
    };
  }
};

std::size_t entry_bytes(std::size_t n)
{
  GribMessageData d;
  d.values.assign(n, 0.0);
  return d.bytes();
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(grib_cache_hits_lru_and_in_use)
{
  FakeDecoder dec;
  GribCacheLimits lim;
  lim.max_bytes = entry_bytes(dec.n) * 3;  // room for three messages
  lim.purge_to = 1.0;                      // purge just under the budget
  GribCache cache(lim, dec.fn(), [] { return std::size_t{0}; });

  auto a = cache.get("f", entry(1), nullptr);
  CHECK(a->values.size() == dec.n && a->values[0] == 1.0);
  CHECK(cache.get("f", entry(1), nullptr) == a);  // hit, same object
  CHECK(dec.calls == 1);
  a.reset();
  {
    auto e2 = cache.get("f", entry(2), nullptr);
    auto e3 = cache.get("f", entry(3), nullptr);
  }
  auto st = cache.stats();
  CHECK(st.entries == 3 && st.hits == 1 && st.misses == 3 && st.evictions == 0);

  // Touch 1 so 2 is the oldest; access times are in seconds.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  cache.get("f", entry(1), nullptr);
  cache.get("f", entry(3), nullptr);
  // Adding a fourth goes over budget: the oldest unused (2) goes first.
  auto held = cache.get("f", entry(4), nullptr);
  st = cache.stats();
  CHECK(st.entries == 3);
  CHECK(st.evictions == 1);
  const int before = dec.calls;
  cache.get("f", entry(1), nullptr);
  cache.get("f", entry(4), nullptr);  // held by us: never evicted
  CHECK(dec.calls == before);
  cache.get("f", entry(2), nullptr);  // was evicted: decoded again
  CHECK(dec.calls == before + 1);

  // Everything in use: nothing can be evicted, the cache just grows.
  std::vector<std::shared_ptr<const GribMessageData>> pins;
  for (std::uint64_t h = 10; h < 16; ++h)
    pins.push_back(cache.get("f", entry(h), nullptr));
  for (std::uint64_t h = 10; h < 16; ++h)
    CHECK(cache.get("f", entry(h), nullptr) == pins[h - 10]);
  CHECK(cache.stats().bytes > lim.max_bytes);
  // Released: the next insert purges back under the budget.
  pins.clear();
  held.reset();
  cache.get("f", entry(20), nullptr);
  CHECK(cache.stats().bytes <= lim.max_bytes);
}

TEST(grib_cache_coalesces_and_handles_failures)
{
  FakeDecoder dec;
  dec.delay = std::chrono::milliseconds(150);
  GribCache cache({}, dec.fn(), [] { return std::size_t{0}; });
  std::vector<std::thread> ts;
  std::vector<std::shared_ptr<const GribMessageData>> got(8);
  for (int k = 0; k < 8; ++k)
    ts.emplace_back([&, k] { got[k] = cache.get("f", entry(7), nullptr); });
  for (auto& t : ts)
    t.join();
  CHECK(dec.calls == 1);
  for (const auto& g : got)
    CHECK(g == got[0]);
  CHECK(cache.stats().coalesced + 1 + cache.stats().hits == 8);

  // A failing decode propagates to every waiter and is not cached.
  dec.fail_hash = 9;
  dec.calls = 0;
  std::atomic<int> failures{0};
  ts.clear();
  for (int k = 0; k < 4; ++k)
    ts.emplace_back(
      [&]
      {
        try
        {
          cache.get("f", entry(9), nullptr);
        }
        catch (const std::runtime_error&)
        {
          ++failures;
        }
      });
  for (auto& t : ts)
    t.join();
  CHECK(failures == 4);
  CHECK(dec.calls >= 1);
  dec.fail_hash = 0;
  CHECK(cache.get("f", entry(9), nullptr)->values[0] == 9.0);

  // No decoder (built without eccodes).
  GribCache none({}, {});
  CHECK(throws<std::runtime_error>([&] { none.get("f", entry(1), nullptr); }));
}

TEST(grib_cache_rss_trigger_and_forced_cleanup)
{
  FakeDecoder dec;
  std::size_t fake_rss = 100;
  GribCacheLimits lim;
  lim.max_bytes = std::size_t{1} << 30;  // budget never the trigger here
  lim.rss_limit = 1000;
  GribCache cache(lim, dec.fn(), [&] { return fake_rss; });
  for (std::uint64_t h = 1; h <= 5; ++h)
    cache.get("f", entry(h), nullptr);
  CHECK(cache.stats().entries == 5);

  // RSS over the limit: maintain() drops everything nobody holds.
  auto held = cache.get("f", entry(3), nullptr);
  cache.maintain();  // RSS fine: nothing happens
  CHECK(cache.stats().entries == 5);
  fake_rss = 5000;
  cache.maintain();
  CHECK(cache.stats().entries == 1);  // only the held one
  CHECK(cache.get("f", entry(3), nullptr) == held);

  // RSS over the limit on insert purges unused entries too.
  fake_rss = 100;
  for (std::uint64_t h = 1; h <= 5; ++h)
    cache.get("f", entry(h), nullptr);
  fake_rss = 5000;
  auto fresh = cache.get("f", entry(6), nullptr);
  CHECK(cache.stats().entries < 6);
  CHECK(fresh->values[0] == 6.0);

  CHECK(process_rss() > 1000000);  // the real one reads /proc
}

TEST(manifest_version_1_with_gribs_is_still_read)
{
  // A T10c manifest (version 1, "gribs"): read as version 2; only the
  // .idx.json sidecars are retired, the GRIB files are left to the grid
  // catalog (T18).
  Manifest base;
  base.sources = SourceRegistry::defaults();
  base.seq = 7;
  base.created_at_s = 1762300800;
  base.grids = {GridEntry{0xabc, "grids/0000000000000abc.grid", 1}};
  nlohmann::json j = base.to_json();
  CHECK(j["version"] == 2 && !j.contains("gribs"));
  j["version"] = 1;
  j["gribs"] = nlohmann::json::array(
    {{{"model", "hrrr"},
      {"run", 1762300800},
      {"path", "grib/hrrr/2025110500/a.grib2"},
      {"index_path", "grib/hrrr/2025110500/a.grib2.idx.json"},
      {"index_crc", 1},
      {"size", 10},
      {"grids", {"0000000000000abc"}}}});
  const Manifest m = Manifest::from_json(j);
  CHECK(m.retired.size() == 1);
  CHECK(
    m.retired.size() == 1 &&
    m.retired[0].path == "grib/hrrr/2025110500/a.grib2.idx.json" &&
    m.retired[0].retired_at_s == base.created_at_s);
  for (const auto& p : m.live_paths())
  {
    CHECK(p.rfind("grib/", 0) != 0);
  }
  CHECK(m.grids == base.grids);
  const nlohmann::json out = m.to_json();
  CHECK(out["version"] == 2 && !out.contains("gribs"));
  CHECK(Manifest::from_json(out) == m);
  // Version 2 does not take "gribs"; other versions are refused.
  nlohmann::json bad = out;
  bad["gribs"] = nlohmann::json::array();
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(bad); }));
  bad = out;
  bad["version"] = 3;
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(bad); }));

  // Published from disk: the sidecar is unlinked after the grace period,
  // the GRIB file stays.
  TempDir td;
  fs::create_directories(td.p / "manifest");
  fs::create_directories(td.p / "grib/hrrr/2025110500");
  fs::create_directories(td.p / "grids");
  std::ofstream(td.p / "grib/hrrr/2025110500/a.grib2") << "GRIB";
  std::ofstream(td.p / "grib/hrrr/2025110500/a.grib2.idx.json") << "{}";
  std::ofstream(td.p / "grids/0000000000000abc.grid") << "g";
  {
    // CURRENT names MANIFEST-...7 with the CRC of its bytes.
    const std::string text = j.dump(1) + "\n";
    std::ofstream(td.p / "manifest" / Manifest::file_name(7)) << text;
    char cur[64];
    std::snprintf(
      cur, sizeof cur, "atmo-current 1 7 %08x\n",
      static_cast<unsigned>(crc32(text.data(), text.size())));
    std::ofstream(td.p / "manifest/CURRENT") << cur;
  }
  ManifestPublisher pub(td.p);
  CHECK(pub.current().seq == 7 && pub.current().retired.size() == 1);
  pub.set_clock([] { return 1762300800 + 100000; });
  const GcReport g = pub.collect_garbage(600);
  CHECK(g.retired_unlinked == 1);
  CHECK(!fs::exists(td.p / "grib/hrrr/2025110500/a.grib2.idx.json"));
  CHECK(fs::exists(td.p / "grib/hrrr/2025110500/a.grib2"));
  CHECK(fs::exists(td.p / "grids/0000000000000abc.grid"));
}

int main()
{
  return tst::run_all();
}
