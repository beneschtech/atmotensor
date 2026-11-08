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

TEST(grib_index_round_trip_and_validation)
{
  GribIndex x;
  x.model = "hrrr";
  x.run_s = 1762300800;
  x.grib = "grib/hrrr/2025110500/hrrr.t00z.wrfsfcf00.grib2";
  x.size = 100000;
  x.messages_total = 4;
  x.entries = {entry(1, 0), entry(2, 0, var::kWindU, 10.0f), entry(3, 1)};
  x.entries[1].info.flags = kFieldGridRelative;
  x.entries[2].hash = 0xffffffffffffffffULL;
  x.skipped = {{3, "parameter 0.16.196 not in the variable registry"}};
  x.validate();
  CHECK(GribIndex::from_text(x.to_text()) == x);

  auto rejects = [&](auto mutate)
  {
    GribIndex y = x;
    mutate(y);
    return throws<std::invalid_argument>([&] { y.validate(); });
  };
  CHECK(rejects([](GribIndex& y) { y.model = "HRRR"; }));
  CHECK(rejects([](GribIndex& y) { y.entries[0].offset = 99990; }));
  CHECK(rejects([](GribIndex& y) { y.entries[0].length = 0; }));
  CHECK(rejects([](GribIndex& y) { y.entries.push_back(y.entries[0]); }));
  CHECK(rejects([](GribIndex& y) { y.entries[1].grid = 0xdef; }));
  CHECK(!rejects([](GribIndex& y) { y.entries[2].grid = 0xdef; }));  // fhr 1

  auto j = x.to_json();
  j["messages"][0]["extra"] = 1;
  CHECK(throws<std::invalid_argument>([&] { GribIndex::from_json(j); }));
  j = x.to_json();
  j["messages"][0]["hash"] = "XYZ";
  CHECK(throws<std::invalid_argument>([&] { GribIndex::from_json(j); }));
  j = x.to_json();
  j["format"] = "atmo-manifest";
  CHECK(throws<std::invalid_argument>([&] { GribIndex::from_json(j); }));
}

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

TEST(manifest_gribs_retention_and_gc)
{
  TempDir td;
  const std::int64_t run0 = 1762300800, h = 3600;
  Manifest m;
  m.sources = SourceRegistry::defaults();
  m.grids = {
    GridEntry{0xabc, "grids/0000000000000abc.grid", 1},
    GridEntry{0xdef, "grids/0000000000000def.grid", 2}};
  for (int k = 0; k < 6; ++k)
  {
    GribEntry g;
    g.model = "hrrr";
    g.run_s = run0 + k * h;
    g.path = "grib/hrrr/run" + std::to_string(k) + "/f.grib2";
    g.index_path = g.path + ".idx.json";
    g.index_crc = static_cast<std::uint32_t>(k);
    g.size = 1000;
    g.grids = {k < 2 ? GridKey{0xdef} : GridKey{0xabc}};
    m.gribs.push_back(g);
  }
  m.validate();
  CHECK(Manifest::from_text(m.to_text()) == m);
  CHECK(m.live_paths().size() == 2 + 12);  // grids + GRIB files + indexes

  auto rejects = [&](auto mutate)
  {
    Manifest x = m;
    mutate(x);
    return throws<std::invalid_argument>([&] { x.validate(); });
  };
  CHECK(rejects([](Manifest& x) { x.gribs[0].index_path = "grib/hrrr/x"; }));
  CHECK(rejects([](Manifest& x) { x.gribs[0].path = "fields/hrrr/a"; }));
  CHECK(rejects([](Manifest& x) { x.gribs[0].grids = {0x123}; }));
  CHECK(rejects([](Manifest& x) { x.gribs[0].grids.clear(); }));
  CHECK(rejects([](Manifest& x) { x.gribs.push_back(x.gribs[0]); }));

  // Newest 4 runs stay; the grid only the two oldest used expires.
  const RetentionResult r = apply_retention(m, run0 + 10 * h, {});
  CHECK(r.manifest.gribs.size() == 4);
  CHECK(r.manifest.gribs.front().run_s == run0 + 2 * h);
  CHECK(r.grids_expired == 1);
  CHECK(r.manifest.grids.size() == 1 && r.manifest.grids[0].key == 0xabc);

  // Publish real files, expire, and gc unlinks GRIB file + index.
  ManifestPublisher pub(td.p);
  std::int64_t clock = 1000000;
  pub.set_clock([&] { return clock; });
  Manifest real;
  real.sources = SourceRegistry::defaults();
  real.grids = {GridEntry{0xabc, "grids/0000000000000abc.grid", 1}};
  std::ofstream(td.p / "grid.tmp") << "x";
  fs::create_directories(td.p / "grids");
  fs::rename(td.p / "grid.tmp", td.p / real.grids[0].path);
  for (int k = 0; k < 5; ++k)
  {
    GribEntry g = m.gribs[static_cast<std::size_t>(k + 1)];
    g.grids = {0xabc};
    fs::create_directories((td.p / g.path).parent_path());
    std::ofstream(td.p / g.path) << "GRIB";
    std::ofstream(td.p / g.index_path) << "{}";
    real.gribs.push_back(g);
  }
  pub.publish(real);
  pub.publish(apply_retention(pub.current(), clock, {}).manifest);
  CHECK(pub.current().gribs.size() == 4);
  CHECK(pub.current().retired.size() == 2);  // GRIB file + its index
  clock += 700;
  const GcReport g = pub.collect_garbage(600);
  CHECK(g.retired_unlinked == 2);
  CHECK(!fs::exists(td.p / "grib/hrrr/run1"));  // emptied directory removed
  CHECK(fs::exists(td.p / "grib/hrrr/run2/f.grib2"));
}

int main()
{
  return tst::run_all();
}
