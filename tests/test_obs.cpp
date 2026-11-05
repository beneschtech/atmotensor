// Obs segment write/read: acceptance tests 1, 2, 5, 6 of
// docs/design/01-obs-store.md, plus writer validation and sealing rules.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "atmo/crc32.hpp"
#include "atmo/obs_segment.hpp"
#include "atmo/source.hpp"
#include "check.hpp"

using namespace atmo;
namespace fs = std::filesystem;

namespace
{

using Tree = KdTreeView4<float>;

struct TempDir
{
  fs::path p;
  TempDir()
  {
    p = fs::temp_directory_path() /
        ("atmo_test_obs_" + std::to_string(::getpid()) + "_" +
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

const SourceClass& metar()
{
  static const SourceClass s = SourceRegistry::defaults().at("metar");
  return s;
}

constexpr std::int64_t kBucket = 1762333200;  // 2025-11-05 09:00Z, hour-aligned

std::vector<std::uint8_t> read_file(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>(
    (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::vector<std::uint8_t>& b)
{
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(
    reinterpret_cast<const char*>(b.data()),
    static_cast<std::streamsize>(b.size()));
}

// Random reports in one metar bucket; `dups` of them copy the position and
// time of an earlier report (same place, same time, different report).
std::vector<ObsReport>
random_reports(std::mt19937_64& rng, std::size_t n, std::size_t dups)
{
  std::uniform_real_distribution<double> lat(-89.9, 89.9), lon(-180, 180),
    h(0, 4000), val(-50, 350);
  std::uniform_int_distribution<int> nv(0, 20), rawlen(0, 300), byte(0, 255),
    dt(0, static_cast<int>(metar().bucket_seconds - 1));
  std::vector<ObsReport> out;
  for (std::size_t i = 0; i < n; ++i)
  {
    ObsReport r;
    r.report_key = rng();
    r.receipt_seq = 1000 + i;
    r.receipt_time_ms = (kBucket + 4000) * 1000 + static_cast<std::int64_t>(i);
    if (i >= n - dups && i > 0)
    {
      const ObsReport& src = out[rng() % (n - dups)];
      r.lat_deg = src.lat_deg;
      r.lon_deg = src.lon_deg;
      r.height_m = src.height_m;
      r.obs_time_s = src.obs_time_s;
    }
    else
    {
      r.lat_deg = lat(rng);
      r.lon_deg = lon(rng);
      r.height_m = static_cast<float>(h(rng));
      r.obs_time_s = kBucket + dt(rng);
    }
    r.station_ref = static_cast<std::uint32_t>(rng() % 5000);
    r.flags = static_cast<std::uint16_t>(rng() % 4);
    const int k = nv(rng);
    for (int v = 0; v < k; ++v)
    {
      r.values.push_back(
        ObsValue{
          static_cast<std::uint16_t>(rng() % 60), static_cast<float>(val(rng)),
          static_cast<std::uint16_t>(rng() % 3),
          v % 3 == 0 ? std::numeric_limits<float>::quiet_NaN()
                     : static_cast<float>(h(rng))});
    }
    const int len = rawlen(rng);
    for (int b = 0; b < len; ++b)
      r.raw.push_back(static_cast<char>(byte(rng)));
    out.push_back(std::move(r));
  }
  return out;
}

fs::path seal_reports(
  const fs::path& dir,
  const std::vector<ObsReport>& reps,
  const char* name = "a.seg")
{
  ObsSegmentWriter w(metar(), kBucket);
  for (const auto& r : reps)
    w.add_report(r);
  const fs::path p = dir / name;
  w.seal(p);
  return p;
}

template <class T>
bool bits_equal(const T& a, const T& b)
{
  return std::memcmp(&a, &b, sizeof(T)) == 0;
}

bool report_equal(const ObsSegment& s, std::size_t i, const ObsReport& r)
{
  const ReportView v = s.report(i);
  bool ok =
    v.row == i && v.report_key == r.report_key &&
    v.receipt_seq == r.receipt_seq && v.receipt_time_ms == r.receipt_time_ms &&
    v.obs_time_s == r.obs_time_s && bits_equal(v.lat_deg, r.lat_deg) &&
    bits_equal(v.lon_deg, r.lon_deg) && bits_equal(v.height_m, r.height_m) &&
    v.station_ref == r.station_ref && v.flags == r.flags &&
    v.values.size() == r.values.size() && v.raw == r.raw;
  for (std::size_t k = 0; ok && k < r.values.size(); ++k)
  {
    ok =
      v.values.var_code(k) == r.values[k].var_code &&
      bits_equal(v.values.value(k), r.values[k].value) &&
      v.values.qc(k) == r.values[k].qc &&
      bits_equal(
        v.values.height_m(k),
        std::isnan(r.values[k].height_m) ? r.height_m : r.values[k].height_m);
  }
  return ok;
}

// Brute force over the same rows, with the tree's own distance function.
std::vector<Tree::Hit> oracle(
  const std::vector<ObsReport>& reps,
  const Tree::Point& q,
  const Embedding& e)
{
  std::vector<Tree::Hit> all;
  for (std::size_t i = 0; i < reps.size(); ++i)
  {
    const auto p = embed<float>(
      reps[i].lat_deg, reps[i].lon_deg, reps[i].height_m,
      static_cast<double>(reps[i].obs_time_s), e);
    all.push_back(Tree::Hit{static_cast<Tree::Id>(i), Tree::dist2(q, p)});
  }
  std::sort(all.begin(), all.end(), &Tree::hit_less);
  return all;
}

bool same_hits(const std::vector<Tree::Hit>& a, const std::vector<Tree::Hit>& b)
{
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    if (a[i].id != b[i].id || !bits_equal(a[i].d2, b[i].d2))
      return false;
  }
  return true;
}

}  // namespace

// Acceptance 1: round trip, every column bit-equal.
TEST(segment_round_trip)
{
  TempDir td;
  std::mt19937_64 rng(1);
  const auto reps = random_reports(rng, 700, 40);
  ObsSegmentWriter w(metar(), kBucket);
  for (std::size_t i = 0; i < reps.size(); ++i)
    CHECK(w.add_report(reps[i]) == i);
  const fs::path p =
    td.p / "obs" / "metar" / std::to_string(kBucket) / "000001.seg";
  const SealedSegment info = w.seal(p);  // creates the directories
  CHECK(fs::exists(p));
  CHECK(info.path == p);
  CHECK(info.n_reports == reps.size());
  CHECK(
    info.receipt_seq_min == 1000 &&
    info.receipt_seq_max == 1000 + reps.size() - 1);
  CHECK(fs::file_size(p) % 4096 == 0);
  // Nothing but the segment is left in its directory (no temp files).
  CHECK(
    std::distance(
      fs::directory_iterator(p.parent_path()), fs::directory_iterator()) == 1);

  const auto seg = ObsSegment::open(p);
  CHECK(seg->source_name() == "metar");
  CHECK(seg->bucket_start_s() == kBucket);
  CHECK(seg->matches(metar()));
  const Embedding e = metar().make_embedding(kBucket);
  CHECK(
    seg->embedding().t0_s == e.t0_s &&
    seg->embedding().km_per_second == e.km_per_second &&
    seg->embedding().vertical_scale == e.vertical_scale &&
    seg->embedding().radius_km == e.radius_km);
  CHECK(seg->size() == reps.size());
  CHECK(seg->header_crc() == info.header_crc);
  CHECK(seg->receipt_seq_min() == info.receipt_seq_min);
  CHECK(seg->receipt_seq_max() == info.receipt_seq_max);
  std::size_t nv = 0, raw = 0;
  bool all_equal = true;
  for (std::size_t i = 0; i < reps.size(); ++i)
  {
    all_equal = all_equal && report_equal(*seg, i, reps[i]);
    nv += reps[i].values.size();
    raw += reps[i].raw.size();
  }
  CHECK(all_equal);
  CHECK(seg->n_values() == nv);
  CHECK(seg->raw_bytes() == raw);
  // Tree ids are row indices; every row is in the tree.
  CHECK(seg->tree().size() == reps.size());
  CHECK(seg->tree().well_formed());

  // Different metric -> does not match (its tree would be wrong).
  SourceClass other = metar();
  other.tau_seconds = 1800;
  CHECK(!seg->matches(other));
  other = metar();
  other.retain_seconds = 3600;  // retention does not affect the tree
  CHECK(seg->matches(other));
}

// Acceptance 1: a flipped byte in any section, or truncation, makes open throw.
TEST(segment_corruption_and_truncation)
{
  TempDir td;
  std::mt19937_64 rng(2);
  const auto reps = random_reports(rng, 200, 5);
  const fs::path p = seal_reports(td.p, reps);
  const auto good = read_file(p);
  const auto secs = ObsSegment::open(p)->sections();
  CHECK(secs.size() == 4);  // reports | values | raw | tree

  const fs::path bad = td.p / "bad.seg";
  // Header (inside the struct, and the magic).
  for (std::size_t off : {std::size_t(0), std::size_t(20), std::size_t(100)})
  {
    auto b = good;
    b[off] ^= 0x01;
    write_file(bad, b);
    CHECK(throws<std::runtime_error>([&] { (void)ObsSegment::open(bad); }));
  }
  // Each section: first, middle and last byte.
  for (const auto& s : secs)
  {
    CHECK(s.offset % 4096 == 0);
    CHECK(s.length > 0);
    for (std::size_t off :
         {s.offset, s.offset + s.length / 2, s.offset + s.length - 1})
    {
      auto b = good;
      b[off] ^= 0x40;
      write_file(bad, b);
      CHECK(throws<std::runtime_error>([&] { (void)ObsSegment::open(bad); }));
    }
  }
  // Truncation and extension.
  for (std::size_t len :
       {std::size_t(0), std::size_t(100), std::size_t(4096), good.size() - 4096,
        good.size() - 1})
  {
    auto b = good;
    b.resize(len);
    write_file(bad, b);
    CHECK(throws<std::runtime_error>([&] { (void)ObsSegment::open(bad); }));
  }
  {
    auto b = good;
    b.resize(good.size() + 4096, 0);
    write_file(bad, b);
    CHECK(throws<std::runtime_error>([&] { (void)ObsSegment::open(bad); }));
  }
  CHECK(
    throws<std::runtime_error>(
      [&] { (void)ObsSegment::open(td.p / "absent.seg"); }));
  // The original still opens.
  CHECK(ObsSegment::open(p)->size() == reps.size());
  // Header-only verification skips the section CRCs (used when re-adopting a
  // file already fully verified), but still checks the header.
  {
    auto b = good;
    b[secs[2].offset] ^= 0x40;  // raw section
    write_file(bad, b);
    CHECK(!throws<std::runtime_error>(
      [&] { (void)ObsSegment::open(bad, ObsSegment::Verify::header); }));
    b = good;
    b[20] ^= 0x01;
    write_file(bad, b);
    CHECK(
      throws<std::runtime_error>(
        [&] { (void)ObsSegment::open(bad, ObsSegment::Verify::header); }));
  }
}

// Acceptance 2: knn / radius over a segment == brute force, with duplicates.
TEST(segment_knn_radius_match_brute_force)
{
  TempDir td;
  std::mt19937_64 rng(3);
  const auto reps = random_reports(rng, 1500, 300);
  const auto seg = ObsSegment::open(seal_reports(td.p, reps));
  const Embedding e = seg->embedding();
  std::uniform_real_distribution<double> lat(-90, 90), lon(-180, 180),
    h(0, 5000);
  std::uniform_int_distribution<int> dt(-1800, 5400);
  bool knn_ok = true, rad_ok = true, conv_ok = true;
  for (int q = 0; q < 300; ++q)
  {
    double qlat, qlon, qh, qt;
    if (q % 3 == 0)
    {  // exactly on a (possibly duplicated) report
      const auto& r = reps[rng() % reps.size()];
      qlat = r.lat_deg, qlon = r.lon_deg, qh = r.height_m,
      qt = static_cast<double>(r.obs_time_s);
    }
    else
    {
      qlat = lat(rng), qlon = lon(rng), qh = h(rng),
      qt = static_cast<double>(kBucket + dt(rng));
    }
    const auto qp = embed<float>(qlat, qlon, qh, qt, e);
    CHECK(qp == seg->query_point(qlat, qlon, qh, qt));
    const auto all = oracle(reps, qp, e);
    for (std::size_t k :
         {std::size_t(1), std::size_t(7), std::size_t(64), reps.size() + 3})
    {
      std::vector<Tree::Hit> want(
        all.begin(), all.begin() + std::min(k, all.size()));
      knn_ok = knn_ok && same_hits(seg->tree().knn(qp, k), want);
    }
    conv_ok =
      conv_ok && same_hits(
                   seg->knn(qlat, qlon, qh, qt, 5),
                   std::vector<Tree::Hit>(all.begin(), all.begin() + 5));
    for (float r : {0.0f, 50.0f, 400.0f, 3000.0f})
    {
      std::vector<Tree::Hit> want;
      for (const auto& hgt : all)
      {
        if (hgt.d2 <= r * r)
          want.push_back(hgt);
      }
      rad_ok = rad_ok && same_hits(seg->tree().radius(qp, r), want);
      conv_ok = conv_ok && same_hits(seg->radius(qlat, qlon, qh, qt, r), want);
    }
  }
  CHECK(knn_ok);
  CHECK(rad_ok);
  CHECK(conv_ok);
  // Duplicates: a query on a duplicated position returns both rows at d2 = 0,
  // lower row first.
  const auto& d = reps.back();
  const auto hits = seg->knn(
    d.lat_deg, d.lon_deg, d.height_m, static_cast<double>(d.obs_time_s), 2);
  CHECK(
    hits.size() == 2 && hits[0].d2 == 0 && hits[1].d2 == 0 &&
    hits[0].id < hits[1].id);
}

// Acceptance 5: a mobile platform (same station_ref) is indexed at each
// report's own position.
TEST(segment_mobile_platform_positions)
{
  TempDir td;
  std::vector<ObsReport> reps;
  for (int i = 0; i < 40; ++i)
  {
    ObsReport r;
    r.report_key = 0xA1C0000 + static_cast<std::uint64_t>(i);
    r.receipt_seq = 50 + static_cast<std::uint64_t>(i);
    r.receipt_time_ms = (kBucket + 60 * i + 5) * 1000;
    r.obs_time_s = kBucket + 60 * i;
    r.lat_deg = 40.0 + 0.1 * i;  // a flight track
    r.lon_deg = -100.0 + 0.15 * i;
    r.height_m = 9000.0f + 10.0f * static_cast<float>(i);
    r.station_ref = 0;  // mobile
    r.values = {ObsValue{1, 220.0f + static_cast<float>(i)}};
    r.raw = "ARP UAL" + std::to_string(i);
    reps.push_back(r);
  }
  const auto seg = ObsSegment::open(seal_reports(td.p, reps));
  bool own = true;
  for (std::size_t i = 0; i < reps.size(); ++i)
  {
    const auto hits = seg->knn(
      reps[i].lat_deg, reps[i].lon_deg, reps[i].height_m,
      static_cast<double>(reps[i].obs_time_s), 1);
    own = own && hits.size() == 1 && hits[0].id == i && hits[0].d2 == 0.0f;
    const ReportView v = seg->report(i);
    own = own && v.lat_deg == reps[i].lat_deg && v.lon_deg == reps[i].lon_deg &&
          v.station_ref == 0;
  }
  CHECK(own);
  // Searching half-way along the track finds the neighbouring fixes, not some
  // single "station position".
  const auto hits =
    seg->knn(41.0, -98.5, 9100.0, static_cast<double>(kBucket + 600), 2);
  CHECK(hits.size() == 2 && hits[0].id == 10);
}

// Acceptance 6: a report with 12 values stores its raw bytes once.
TEST(segment_raw_shared_by_values)
{
  TempDir td;
  const std::string msg =
    "METAR KOKC 050953Z 18012G20KT 10SM FEW250 18/09 A2992 RMK AO2 SLP127 "
    "T01830089=";
  ObsReport r;
  r.report_key = 42;
  r.receipt_seq = 7;
  r.receipt_time_ms = (kBucket + 3240) * 1000;
  r.obs_time_s = kBucket + 3180;
  r.lat_deg = 35.39;
  r.lon_deg = -97.60;
  r.height_m = 395;
  r.station_ref = 1234;
  for (std::uint16_t v = 0; v < 12; ++v)
  {
    r.values.push_back(ObsValue{static_cast<std::uint16_t>(v + 1), 290.0f + v});
  }
  r.raw = msg;
  ObsReport r2 = r;  // a second report, different message
  r2.report_key = 43;
  r2.receipt_seq = 8;
  r2.raw = "METAR KTIK 050953Z AUTO 17010KT 10SM CLR 17/08 A2993=";
  r2.values.resize(3);

  const auto seg = ObsSegment::open(seal_reports(td.p, {r, r2}));
  CHECK(seg->n_values() == 15);
  CHECK(seg->raw_bytes() == msg.size() + r2.raw.size());  // once per report
  const ReportView v = seg->report(0);
  CHECK(v.raw == msg);
  CHECK(v.values.size() == 12);
  for (std::size_t k = 0; k < 12; ++k)
  {
    CHECK(v.values.var_code(k) == k + 1);
    CHECK(v.values.value(k) == 290.0f + static_cast<float>(k));
    CHECK(v.values.qc(k) == 0);
    CHECK(v.values.height_m(k) == 395.0f);  // defaults to the report's height
  }
  CHECK(seg->report(1).raw == r2.raw);
  CHECK(seg->report(1).values.size() == 3);
  CHECK(seg->report(1).values.var_code(0) == 1);
  CHECK(throws<std::out_of_range>([&] { (void)seg->report(2); }));
}

// Profiles (QUESTIONS.md Q3): one report per sounding; every value carries
// the geometric height of its level.
TEST(segment_profile_levels)
{
  TempDir td;
  const SourceClass snd = SourceRegistry::defaults().at("sounding");
  const std::int64_t b = snd.bucket_start_s(kBucket);
  ObsReport r;
  r.report_key = 9;
  r.receipt_seq = 1;
  r.receipt_time_ms = b * 1000;
  r.obs_time_s = b;
  r.lat_deg = 35.18;
  r.lon_deg = -97.44;
  r.height_m = 357;
  r.station_ref = 72357;
  for (int lev = 0; lev < 100; ++lev)
  {
    const float z =
      357.0f + 150.0f * static_cast<float>(lev);  // geometric, m MSL
    r.values.push_back(
      ObsValue{1, 100000.0f - 900.0f * static_cast<float>(lev), 0, z});
    r.values.push_back(
      ObsValue{2, 300.0f - 0.6f * static_cast<float>(lev), 0, z});
  }
  ObsSegmentWriter w(snd, b);
  w.add_report(r);
  w.seal(td.p / "s.seg");
  const auto seg = ObsSegment::open(td.p / "s.seg");
  CHECK(seg->size() == 1);
  CHECK(seg->matches(snd) && !seg->matches(metar()));
  const auto v = seg->report(0);
  CHECK(v.values.size() == 200);
  CHECK(v.values.height_m(0) == 357.0f);
  CHECK(
    v.values.height_m(199) == 357.0f + 150.0f * 99 &&
    v.values.var_code(199) == 2);
}

TEST(segment_empty)
{
  TempDir td;
  ObsSegmentWriter w(metar(), kBucket);
  const SealedSegment info = w.seal(td.p / "e.seg");
  CHECK(info.n_reports == 0);
  const auto seg = ObsSegment::open(td.p / "e.seg");
  CHECK(seg->size() == 0 && seg->n_values() == 0 && seg->raw_bytes() == 0);
  CHECK(seg->tree().empty());
  CHECK(seg->knn(0, 0, 0, static_cast<double>(kBucket), 3).empty());
}

TEST(writer_validation)
{
  TempDir td;
  CHECK(
    throws<std::invalid_argument>(
      [] { ObsSegmentWriter w(metar(), kBucket + 1); }));
  SourceClass broken = metar();
  broken.tau_seconds = 0;
  CHECK(
    throws<std::invalid_argument>([&]
                                  { ObsSegmentWriter w(broken, kBucket); }));

  ObsSegmentWriter w(metar(), kBucket);
  ObsReport ok;
  ok.report_key = 1;
  ok.receipt_seq = 1;
  ok.obs_time_s = kBucket;
  ok.lat_deg = 10;
  ok.lon_deg = 10;
  auto rejects = [&](auto mutate)
  {
    ObsReport r = ok;
    mutate(r);
    return throws<std::invalid_argument>([&] { w.add_report(r); });
  };
  CHECK(rejects([](ObsReport& r) { r.obs_time_s = kBucket - 1; }));
  CHECK(rejects([](ObsReport& r) { r.obs_time_s = kBucket + 3600; }));
  CHECK(rejects([](ObsReport& r) { r.lat_deg = 90.5; }));
  CHECK(rejects([](ObsReport& r) { r.lat_deg = NAN; }));
  CHECK(rejects([](ObsReport& r) { r.lon_deg = INFINITY; }));
  CHECK(rejects([](ObsReport& r) { r.lon_deg = 400; }));
  CHECK(rejects([](ObsReport& r) { r.height_m = NAN; }));
  CHECK(rejects([](ObsReport& r) { r.values = {ObsValue{1, NAN}}; }));
  CHECK(
    rejects([](ObsReport& r) { r.values = {ObsValue{1, 1.0f, 0, INFINITY}}; }));
  CHECK(rejects([](ObsReport& r) { r.values.resize(65536); }));
  CHECK(w.size() == 0);  // rejected reports leave no trace
  CHECK(w.add_report(ok) == 0);
  // Same (report_key, receipt_seq) twice is not two versions.
  CHECK(rejects([](ObsReport&) {}));
  ObsReport v2 = ok;
  v2.receipt_seq = 2;
  v2.flags = kObsCorrection;
  CHECK(w.add_report(v2) == 1);  // a correction is a new row

  // Sealing never overwrites an existing file.
  const fs::path p = td.p / "x.seg";
  {
    std::ofstream(p) << "precious";
  }
  CHECK(throws<std::runtime_error>([&] { w.seal(p); }));
  CHECK(read_file(p).size() == 8);
  CHECK(
    std::distance(fs::directory_iterator(td.p), fs::directory_iterator()) == 1);
  // A failed seal can be retried elsewhere; a writer seals exactly once.
  w.seal(td.p / "y.seg");
  CHECK(ObsSegment::open(td.p / "y.seg")->size() == 2);
  CHECK(throws<std::logic_error>([&] { w.seal(td.p / "z.seg"); }));
  CHECK(throws<std::logic_error>([&] { w.add_report(ok); }));
}

// A file whose CRCs are all valid but whose contents are inconsistent (bug
// in a writer, or a deliberately crafted file) must still be rejected, so no
// accessor can read outside the mapping.
TEST(segment_rejects_consistent_crc_but_bad_structure)
{
  TempDir td;
  std::mt19937_64 rng(4);
  const auto reps = random_reports(rng, 50, 0);
  const fs::path p = seal_reports(td.p, reps);
  const auto good = read_file(p);
  const obsseg::Layout L(
    reps.size(), ObsSegment::open(p)->n_values(),
    ObsSegment::open(p)->raw_bytes());
  const fs::path bad = td.p / "bad.seg";
  auto patch = [&](std::size_t off, const void* v, std::size_t n)
  {
    auto b = good;
    std::memcpy(b.data() + off, v, n);
    obsseg::SegHeader h;
    std::memcpy(&h, b.data(), sizeof h);
    for (std::size_t s = 0; s < obsseg::kSections; ++s)
    {
      h.sec[s].crc = crc32(b.data() + h.sec[s].off, h.sec[s].len);
    }
    h.crc_header = crc32(&h, offsetof(obsseg::SegHeader, crc_header));
    std::memcpy(b.data(), &h, sizeof h);
    write_file(bad, b);
    return throws<std::runtime_error>([&] { (void)ObsSegment::open(bad); });
  };
  const std::uint32_t big = 0xFFFFFFF0u;
  CHECK(patch(L.rcol[obsseg::kRawLen], &big, 4));  // raw slice past end
  const std::uint32_t first = 1;
  CHECK(patch(L.rcol[obsseg::kFirst], &first, 4));  // value slices overlap
  const std::int64_t t = kBucket - 1;
  CHECK(patch(L.rcol[obsseg::kObsTime], &t, 8));  // outside bucket
  const std::uint32_t id = 1;
  CHECK(patch(L.tree_ids, &id, 4));  // ids not a permutation
  const std::uint8_t ax = 7;
  CHECK(patch(L.tree_axes, &ax, 1));  // bad split axis
  const float inf = INFINITY;
  CHECK(patch(L.tree_pts, &inf, 4));  // non-finite point
  const float nan = NAN;
  CHECK(patch(L.vcol[obsseg::kVHeight], &nan, 4));  // non-finite value height
  // Sanity: re-checksumming an unmodified copy is accepted.
  const std::uint8_t same = good[L.raw];
  CHECK(!patch(L.raw, &same, 1));
}

int main()
{
  return tst::run_all();
}
