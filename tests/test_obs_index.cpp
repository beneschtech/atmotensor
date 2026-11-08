// ObsIndex: acceptance tests 3 and 4 of docs/design/01-obs-store.md, a
// randomized test against a brute-force "latest version wins" oracle, and
// the metric-change rebuild (Q5b).
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "atmo/obs_index.h"
#include "atmo/obs_segment.h"
#include "atmo/source.h"
#include "check.h"

using namespace atmo;
namespace fs = std::filesystem;

namespace
{

using SegPtr = std::shared_ptr<const ObsSegment>;
using Tree = KdTreeView4<float>;

struct TempDir
{
  fs::path p;
  TempDir()
  {
    p = fs::temp_directory_path() /
        ("atmo_test_obsidx_" + std::to_string(::getpid()) + "_" +
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

const SourceRegistry& registry()
{
  static const SourceRegistry r = SourceRegistry::defaults();
  return r;
}

const SourceClass& metar()
{
  return registry().at("metar");
}

// A registry whose metar class is replaced by `m`.
SourceRegistry with_metar(const SourceClass& m)
{
  std::vector<SourceClass> v = registry().all();
  for (auto& c : v)
  {
    if (c.name == m.name)
      c = m;
  }
  return SourceRegistry(v);
}

constexpr std::int64_t kBucket = 1762333200;  // 2025-11-05 09:00Z

ObsReport make_report(
  std::uint64_t key,
  std::uint64_t seq,
  float lat,
  float lon,
  std::int64_t t_s)
{
  ObsReport r;
  r.report_key = key;
  r.receipt_seq = seq;
  r.receipt_time_ms = t_s * 1000 + 60000;
  r.obs_time_s = t_s;
  r.lat_deg = lat;
  r.lon_deg = lon;
  r.height_m = 300.0f;
  r.values.push_back(ObsValue{1, static_cast<float>(seq), 0});
  r.raw = "K" + std::to_string(key) + "v" + std::to_string(seq);
  return r;
}

class Segs
{
 public:
  explicit Segs(const fs::path& dir) : dir_(dir) {}

  SegPtr seal(
    const SourceClass& s,
    std::int64_t bucket_start_s,
    const std::vector<ObsReport>& reps)
  {
    ObsSegmentWriter w(s, bucket_start_s);
    for (const auto& r : reps)
      w.add_report(r);
    const fs::path p = dir_ / ("s" + std::to_string(n_++) + ".seg");
    w.seal(p);
    return ObsSegment::open(p);
  }

  SegPtr seal(const std::vector<ObsReport>& reps)
  {
    return seal(metar(), kBucket, reps);
  }

 private:
  fs::path dir_;
  int n_ = 0;
};

ObsQuery
at(double lat, double lon, double t_s, std::uint64_t as_of = kObsLatest)
{
  ObsQuery q;
  q.lat_deg = lat;
  q.lon_deg = lon;
  q.height_m = 300.0;
  q.t_s = t_s;
  q.as_of = as_of;
  return q;
}

bool has(const std::vector<ObsHit>& hits, std::uint64_t key, std::uint64_t seq)
{
  for (const auto& h : hits)
  {
    if (h.report.report_key == key && h.report.receipt_seq == seq)
      return true;
  }
  return false;
}

std::size_t count_key(const std::vector<ObsHit>& hits, std::uint64_t key)
{
  std::size_t n = 0;
  for (const auto& h : hits)
  {
    if (h.report.report_key == key)
      ++n;
  }
  return n;
}

// Brute-force oracle: every row of every segment of `src`, latest version
// <= as_of per report_key, |obs_time - t| <= W, distance with the current
// metric of `cls` and the segment's bucket start as t0.
struct Expect
{
  float d2;
  std::uint64_t key;
  std::uint64_t seq;
};

std::vector<Expect> oracle(
  const SourceClass& cls,
  const std::vector<SegPtr>& segs,
  const ObsQuery& q)
{
  std::map<std::uint64_t, std::uint64_t> latest;
  for (const auto& s : segs)
  {
    if (s->source_name() != cls.name)
      continue;
    for (std::size_t i = 0; i < s->size(); ++i)
    {
      const std::uint64_t k = s->report_keys()[i];
      const std::uint64_t v = s->receipt_seqs()[i];
      if (v > q.as_of)
        continue;
      auto it = latest.find(k);
      if (it == latest.end() || it->second < v)
        latest[k] = v;
    }
  }
  const double w = std::isnan(q.window_s) ? 3.0 * cls.tau_seconds : q.window_s;
  std::vector<Expect> out;
  for (const auto& s : segs)
  {
    if (s->source_name() != cls.name)
      continue;
    Embedding e;
    e.t0_s = static_cast<double>(s->bucket_start_s());
    e.km_per_second = cls.lambda();
    e.vertical_scale = cls.vertical_scale;
    const auto qp = embed<float>(q.lat_deg, q.lon_deg, q.height_m, q.t_s, e);
    for (std::size_t i = 0; i < s->size(); ++i)
    {
      const std::uint64_t k = s->report_keys()[i];
      const std::uint64_t v = s->receipt_seqs()[i];
      auto it = latest.find(k);
      if (it == latest.end() || it->second != v)
        continue;
      const double t = static_cast<double>(s->obs_times_s()[i]);
      if (!(std::fabs(t - q.t_s) <= w))
        continue;
      const auto p = embed<float>(
        s->lats_deg()[i], s->lons_deg()[i], s->heights_m()[i], t, e);
      out.push_back(Expect{Tree::dist2(qp, p), k, v});
    }
  }
  std::sort(
    out.begin(), out.end(), [](const Expect& a, const Expect& b)
    { return a.d2 < b.d2 || (a.d2 == b.d2 && a.key < b.key); });
  return out;
}

bool same(const std::vector<ObsHit>& got, const std::vector<Expect>& want)
{
  if (got.size() != want.size())
    return false;
  for (std::size_t i = 0; i < got.size(); ++i)
  {
    if (
      got[i].report.report_key != want[i].key ||
      got[i].report.receipt_seq != want[i].seq ||
      std::memcmp(&got[i].d2, &want[i].d2, sizeof(float)) != 0)
      return false;
  }
  return true;
}

std::vector<Expect> first_k(std::vector<Expect> v, std::size_t k)
{
  if (v.size() > k)
    v.resize(k);
  return v;
}

std::vector<Expect> within(std::vector<Expect> v, float r)
{
  std::vector<Expect> out;
  for (const auto& e : v)
  {
    if (e.d2 <= r * r)
      out.push_back(e);
  }
  return out;
}

}  // namespace

// Acceptance 3: report A v1 in segment 1, v2 in segment 2.
TEST(index_corrections_latest_wins)
{
  TempDir td;
  Segs segs(td.p);
  const std::int64_t t = kBucket + 1200;
  std::vector<SegPtr> v;
  v.push_back(segs.seal(
    {make_report(1, 10, 35.0f, -97.0f, t),
     make_report(2, 11, 35.5f, -97.5f, t)}));
  v.push_back(segs.seal({make_report(1, 20, 35.01f, -97.0f, t)}));
  const ObsIndex idx(registry(), v);
  CHECK(idx.max_receipt_seq() == 20);
  CHECK(idx.segment_count() == 2);
  CHECK(idx.rebuilt_trees() == 0);

  const auto def = idx.knn("metar", at(35.0, -97.0, double(t)), 10);
  CHECK(def.size() == 2);
  CHECK(has(def, 1, 20));
  CHECK(!has(def, 1, 10));
  CHECK(count_key(def, 1) == 1);
  if (!def.empty())
  {
    CHECK(def[0].report.report_key == 1);
    CHECK(def[0].report.raw == "K1v20");
  }

  const auto old = idx.knn("metar", at(35.0, -97.0, double(t), 10), 10);
  CHECK(has(old, 1, 10));
  CHECK(!has(old, 1, 20));
  CHECK(!has(old, 2, 11));  // seq 11 > as_of
  CHECK(old.size() == 1);

  const auto none = idx.knn("metar", at(35.0, -97.0, double(t), 9), 10);
  CHECK(none.empty());

  for (std::uint64_t as_of : {kObsLatest, std::uint64_t{10}, std::uint64_t{15}})
  {
    const auto r = idx.radius("metar", at(35.0, -97.0, double(t), as_of), 500);
    CHECK(count_key(r, 1) == 1);
    CHECK(has(r, 1, as_of >= 20 ? 20 : 10));
  }
}

// A correction that moves a report: v2 is far from v1, so a query at v1's
// position must not return v1 just because v2 is not among the candidates.
TEST(index_moved_correction_hides_old_position)
{
  TempDir td;
  Segs segs(td.p);
  const std::int64_t t = kBucket + 600;
  std::vector<SegPtr> v;
  v.push_back(segs.seal(
    {make_report(7, 1, 40.0f, -100.0f, t),
     make_report(8, 2, 41.0f, -100.0f, t)}));
  v.push_back(segs.seal({make_report(7, 3, -30.0f, 60.0f, t)}));
  const ObsIndex idx(registry(), v);
  const auto h = idx.knn("metar", at(40.0, -100.0, double(t)), 1);
  CHECK(h.size() == 1);
  if (!h.empty())
    CHECK(h[0].report.report_key == 8);
  const auto r = idx.radius("metar", at(40.0, -100.0, double(t)), 300);
  CHECK(!has(r, 7, 1));
  CHECK(has(r, 8, 2));
  const auto old = idx.knn("metar", at(40.0, -100.0, double(t), 2), 1);
  CHECK(old.size() == 1 && has(old, 7, 1));
}

// Acceptance 4: most of the nearest rows are superseded.
TEST(index_visible_k_with_superseded_neighbours)
{
  TempDir td;
  Segs segs(td.p);
  const std::int64_t t = kBucket + 1800;
  std::vector<ObsReport> old_rows, new_rows, far_rows;
  std::uint64_t seq = 1;
  for (std::uint64_t k = 0; k < 200; ++k)
  {
    // Five stale versions right at the query point.
    for (int ver = 0; ver < 5; ++ver)
    {
      old_rows.push_back(make_report(
        k, seq++, 10.0f + 0.001f * float(k), 20.0f + 0.001f * float(ver), t));
    }
    // The latest version moved 15-20 degrees away.
    new_rows.push_back(
      make_report(k, seq++, 30.0f + 0.01f * float(k), 20.0f, t));
  }
  for (std::uint64_t k = 1000; k < 1010; ++k)
    far_rows.push_back(make_report(k, seq++, 12.0f, 20.0f + float(k - 999), t));
  std::vector<SegPtr> v = {
    segs.seal(old_rows), segs.seal(new_rows), segs.seal(far_rows)};
  const ObsIndex idx(registry(), v);
  const ObsQuery q = at(10.0, 20.0, double(t));
  for (std::size_t k : {1u, 3u, 8u, 10u, 25u, 210u, 500u})
  {
    const auto h = idx.knn("metar", q, k);
    CHECK(h.size() == std::min<std::size_t>(k, 210));
    CHECK(same(h, first_k(oracle(metar(), v, q), k)));
    for (const auto& x : h)
      CHECK(count_key(h, x.report.report_key) == 1);
  }
  const auto ten = idx.knn("metar", q, 10);
  for (const auto& x : ten)
    CHECK(x.report.report_key >= 1000);
}

// The window is a hard filter: |obs_time - t| <= W (3 tau by default).
TEST(index_time_window)
{
  TempDir td;
  Segs segs(td.p);
  std::vector<SegPtr> v;
  // Buckets 0..9 hours after kBucket, one report each in the bucket middle.
  for (std::int64_t b = 0; b < 10; ++b)
  {
    const std::int64_t start = kBucket + b * 3600;
    v.push_back(segs.seal(
      metar(), start,
      {make_report(
        std::uint64_t(b), std::uint64_t(b + 1), 50.0f, 8.0f, start + 1800)}));
  }
  const ObsIndex idx(registry(), v);
  const double t = double(kBucket + 5 * 3600 + 1800);
  ObsQuery q = at(50.0, 8.0, t);
  // tau = 1 h: buckets 2..8 are within 3 h (both ends inclusive).
  auto h = idx.knn("metar", q, 100);
  CHECK(h.size() == 7);
  CHECK(same(h, oracle(metar(), v, q)));
  if (!h.empty())
    CHECK(h[0].report.report_key == 5);
  q.window_s = 3600.0;
  h = idx.knn("metar", q, 100);
  CHECK(h.size() == 3);
  CHECK(same(h, oracle(metar(), v, q)));
  q.window_s = 0.0;
  h = idx.radius("metar", q, 1e6f);
  CHECK(h.size() == 1);
  q.window_s = 1e9;
  CHECK(idx.knn("metar", q, 100).size() == 10);
}

// Randomized: many versions per key spread over segments and buckets,
// random as_of; knn and radius must equal the brute-force oracle exactly.
TEST(index_random_matches_oracle)
{
  TempDir td;
  Segs segs(td.p);
  std::mt19937_64 rng(4242);
  std::uniform_real_distribution<double> lat(25, 50), lon(-125, -65);
  std::vector<SegPtr> v;
  std::uint64_t seq = 1;
  const std::uint64_t n_keys = 400;
  for (std::int64_t b = 0; b < 6; ++b)
  {
    const std::int64_t start = kBucket + b * 3600;
    // Each key lives in one bucket (its obs_time); its versions are spread
    // over the bucket's 1-3 segments.
    const int n_segs = 1 + int(rng() % 3);
    std::vector<std::vector<ObsReport>> rows(static_cast<std::size_t>(n_segs));
    for (std::uint64_t k = std::uint64_t(b) * n_keys;
         k < std::uint64_t(b + 1) * n_keys; ++k)
    {
      const std::int64_t ot = start + std::int64_t(rng() % 3600);
      const int versions = 1 + int(rng() % 4);
      float la = float(lat(rng)), lo = float(lon(rng));
      for (int ver = 0; ver < versions; ++ver)
      {
        if (rng() % 3 == 0)  // some corrections move the report
        {
          la = float(lat(rng));
          lo = float(lon(rng));
        }
        rows[rng() % rows.size()].push_back(
          make_report(k * 7919 + 13, seq++, la, lo, ot));
      }
    }
    for (const auto& r : rows)
      v.push_back(segs.seal(metar(), start, r));
  }
  // A second source class in the same index must not leak into metar.
  const SourceClass& synop = registry().at("synop");
  v.push_back(segs.seal(
    synop, synop.bucket_start_s(kBucket),
    {make_report(1, seq++, 40.0f, -100.0f, kBucket + 100)}));
  const ObsIndex idx(registry(), v);
  CHECK(idx.max_receipt_seq() == seq - 1);
  int bad = 0;
  for (int it = 0; it < 300; ++it)
  {
    ObsQuery q = at(
      lat(rng), lon(rng), double(kBucket + std::int64_t(rng() % (6 * 3600))));
    q.height_m = double(rng() % 3000);
    const int mode = int(rng() % 3);
    if (mode == 1)
      q.as_of = 1 + rng() % (seq + 10);
    if (mode == 2)
      q.window_s = double(rng() % 9000);
    const auto all = oracle(metar(), v, q);
    const std::size_t k = 1 + rng() % 40;
    if (!same(idx.knn("metar", q, k), first_k(all, k)))
      ++bad;
    const float r = float(rng() % 1500);
    if (!same(idx.radius("metar", q, r), within(all, r)))
      ++bad;
  }
  CHECK(bad == 0);
}

// Q5b: segments built with an older metric get their trees rebuilt with
// the current one; a bucket_seconds change alone keeps the prebuilt tree.
TEST(index_metric_change_rebuilds_tree)
{
  TempDir td;
  Segs segs(td.p);
  SourceClass old_m = metar();
  old_m.L_km = 40.0;
  old_m.vertical_scale = 3.0;
  SourceClass half = metar();
  half.bucket_seconds = 1800;

  std::mt19937_64 rng(77);
  std::uniform_real_distribution<double> lat(30, 45), lon(-110, -80);
  std::vector<SegPtr> v;
  std::uint64_t seq = 1;
  auto rows = [&](std::int64_t start, std::int64_t len, std::uint64_t key0)
  {
    std::vector<ObsReport> out;
    for (std::uint64_t k = key0; k < key0 + 150; ++k)
    {
      out.push_back(make_report(
        k, seq++, float(lat(rng)), float(lon(rng)),
        start + std::int64_t(rng() % std::uint64_t(len))));
    }
    return out;
  };
  v.push_back(segs.seal(old_m, kBucket, rows(kBucket, 3600, 0)));
  v.push_back(
    segs.seal(metar(), kBucket + 3600, rows(kBucket + 3600, 3600, 1000)));
  // After the bucket_seconds change: half-hour buckets, same metric.
  v.push_back(
    segs.seal(half, kBucket + 7200, rows(kBucket + 7200, 1800, 2000)));
  // A correction of key 1000 (obs_time inside the old hourly bucket) lands
  // in an overlapping half-hour bucket: the hourly row must be superseded.
  std::int64_t ot_1000 = v[1]->obs_times_s()[0];
  v.push_back(segs.seal(
    half, half.bucket_start_s(ot_1000),
    {make_report(1000, seq++, 31.0f, -81.0f, ot_1000)}));

  const ObsIndex idx(registry(), v);
  CHECK(idx.rebuilt_trees() == 1);
  int bad = 0;
  for (int it = 0; it < 200; ++it)
  {
    const ObsQuery q =
      at(lat(rng), lon(rng), double(kBucket + std::int64_t(rng() % 9000)));
    const auto all = oracle(metar(), v, q);
    const std::size_t k = 1 + rng() % 30;
    if (!same(idx.knn("metar", q, k), first_k(all, k)))
      ++bad;
    if (!same(idx.radius("metar", q, 400.0f), within(all, 400.0f)))
      ++bad;
  }
  CHECK(bad == 0);
  const auto r = idx.radius("metar", at(31.0, -81.0, double(ot_1000)), 1e5f);
  CHECK(count_key(r, 1000) == 1);
  CHECK(has(r, 1000, seq - 1));

  // Against a registry with the old metric, the other two are rebuilt.
  const ObsIndex idx_old(with_metar(old_m), v);
  CHECK(idx_old.rebuilt_trees() == 3);
  const ObsQuery q = at(37.0, -95.0, double(kBucket + 1800));
  CHECK(same(idx_old.knn("metar", q, 20), first_k(oracle(old_m, v, q), 20)));
}

TEST(index_validation_and_edges)
{
  TempDir td;
  Segs segs(td.p);
  const std::int64_t t = kBucket + 60;
  const SegPtr a = segs.seal({make_report(1, 5, 0.0f, 0.0f, t)});
  const SegPtr dup = segs.seal({make_report(1, 5, 1.0f, 1.0f, t)});
  CHECK(throws<std::invalid_argument>([&] { ObsIndex(registry(), {a, dup}); }));
  CHECK(
    throws<std::invalid_argument>([&] { ObsIndex(registry(), {a, nullptr}); }));
  // The same version in overlapping buckets (around a bucket_seconds
  // change) is a duplicate too.
  SourceClass half = metar();
  half.bucket_seconds = 1800;
  const SegPtr dup_half =
    segs.seal(half, kBucket, {make_report(1, 5, 2.0f, 2.0f, t)});
  CHECK(
    throws<std::invalid_argument>([&]
                                  { ObsIndex(registry(), {a, dup_half}); }));
  const SegPtr other_half =
    segs.seal(half, kBucket, {make_report(1, 6, 2.0f, 2.0f, t)});
  CHECK(ObsIndex(registry(), {a, other_half}).segment_count() == 2);
  SourceRegistry only_synop({registry().at("synop")});
  CHECK(throws<std::invalid_argument>([&] { ObsIndex(only_synop, {a}); }));

  const ObsIndex idx(registry(), {a});
  CHECK(
    throws<std::out_of_range>([&]
                              { idx.knn("nosuch", at(0, 0, double(t)), 1); }));
  CHECK(idx.knn("metar", at(0, 0, double(t)), 0).empty());
  CHECK(idx.radius("metar", at(0, 0, double(t)), -1.0f).empty());
  CHECK(idx.radius("metar", at(0, 0, double(t)), NAN).empty());
  CHECK(idx.knn("metar", at(0, 0, double(t)), 5).size() == 1);
  CHECK(idx.knn("synop", at(0, 0, double(t)), 5).empty());

  const ObsIndex empty(registry(), {});
  CHECK(empty.max_receipt_seq() == 0);
  CHECK(empty.knn("metar", at(0, 0, double(t)), 5).empty());

  const SegPtr nothing = segs.seal({});
  const ObsIndex e2(registry(), {nothing, a});
  CHECK(e2.knn("metar", at(0, 0, double(t)), 5).size() == 1);

  // Hits stay valid after the index is moved.
  ObsIndex moved(registry(), {a});
  auto h = moved.knn("metar", at(0, 0, double(t)), 1);
  ObsIndex m2 = std::move(moved);
  CHECK(h.size() == 1 && h[0].report.raw == "K1v5");
  CHECK(m2.knn("metar", at(0, 0, double(t)), 1).size() == 1);
}

int main()
{
  return tst::run_all();
}
