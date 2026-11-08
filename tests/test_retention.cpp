// Retention, retirement and gc (D1; 01-obs-store.md "Retention";
// 02-manifest-and-processes.md "Retirement"), with fake clocks.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/gridstore.h"
#include "atmo/manifest.h"
#include "atmo/obs_segment.h"
#include "atmo/retention.h"
#include "atmo/snapshot.h"
#include "atmo/source.h"
#include "atmo/stations.h"
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
        ("atmo_test_retention_" + std::to_string(::getpid()) + "_" +
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

constexpr std::int64_t kHour = 3600;
constexpr std::int64_t kBucket = 1762333200;  // 2025-11-05 09:00Z

SegmentEntry seg_entry(
  const std::string& source,
  std::int64_t bucket,
  std::int64_t bucket_seconds,
  const std::string& id)
{
  return SegmentEntry{
    source,
    bucket,
    bucket_seconds,
    "obs/" + source + "/" + std::to_string(bucket) + "/" + id + ".seg",
    0,
    0,
    0,
    0};
}

FieldEntry field(
  const std::string& model,
  std::int64_t run,
  std::uint32_t fhr,
  GridKey grid)
{
  return FieldEntry{
    model,
    run,
    fhr,
    grid,
    "fields/" + model + "/" + std::to_string(run) + "/f" + std::to_string(fhr) +
      ".fld",
    0};
}

GridEntry grid_entry(GridKey k)
{
  return GridEntry{k, "grids/" + key_hex(k) + ".grid", 0};
}

// A metar segment with one report, sealed under root.
SegmentEntry seal(
  const fs::path& root,
  std::int64_t bucket,
  const std::string& id,
  std::uint64_t seq)
{
  ObsSegmentWriter w(registry().at("metar"), bucket);
  ObsReport r;
  r.report_key = seq;
  r.receipt_seq = seq;
  r.receipt_time_ms = (bucket + 600) * 1000;
  r.obs_time_s = bucket + 300;
  r.lat_deg = 35.0f;
  r.lon_deg = -97.5f;
  r.values.push_back(ObsValue{1, 1.0f, 0});
  r.raw = "r" + std::to_string(seq);
  w.add_report(r);
  const fs::path p =
    root / "obs" / "metar" / std::to_string(bucket) / (id + ".seg");
  w.seal(p);
  return SegmentEntry::from_segment(root, *ObsSegment::open(p));
}

StationsEntry
seal_stations(const fs::path& root, const StationTable* prev, const char* wmo)
{
  StationTableBuilder b =
    prev ? StationTableBuilder(*prev) : StationTableBuilder();
  Station s;
  s.name = std::string("station ") + wmo;
  s.ids = {
    StationIdBinding{StationId{StationIdType::wmo, wmo}, kTimeMin, kTimeMax}};
  s.periods = {StationPeriod{kTimeMin, kTimeMax, 35.0f, -97.0f, 300.0f}};
  b.add(s);
  const fs::path p = b.seal(root / "stations");
  auto t = StationTable::open(p);
  return StationsEntry{
    t->generation(), "stations/" + p.filename().string(), t->header_crc()};
}

void write_junk(const fs::path& p, std::int64_t mtime_s)
{
  fs::create_directories(p.parent_path());
  std::ofstream(p) << "junk";
  struct timespec ts[2];
  ts[0].tv_sec = static_cast<time_t>(mtime_s);
  ts[0].tv_nsec = 0;
  ts[1] = ts[0];
  ::utimensat(AT_FDCWD, p.c_str(), ts, 0);
}

// Fake clock starting at the real time, so file mtimes compare sensibly.
struct FakeClock
{
  std::int64_t t = static_cast<std::int64_t>(::time(nullptr));
};

}  // namespace

// ---------------------------------------------------------------------------

TEST(retention_obs_buckets)
{
  Manifest m;
  m.sources = registry();
  const std::int64_t retain = registry().at("metar").retain_seconds;
  const std::int64_t retain_snd = registry().at("sounding").retain_seconds;
  const std::int64_t now = kBucket + 100 * kHour;
  // Bucket ending exactly at now - retain stays (expiry needs end < cutoff).
  const std::int64_t edge = now - retain - kHour;
  m.segments = {
    seg_entry("metar", edge - kHour, kHour, "old"),
    seg_entry("metar", edge - kHour, kHour, "old_correction"),
    seg_entry("metar", edge, kHour, "edge"),
    seg_entry("metar", now - kHour, kHour, "new"),
    // Written with 6 h buckets before a bucket_seconds change: its own
    // length counts, so it is still inside the window.
    seg_entry("metar", edge - 5 * kHour, 6 * kHour, "long"),
    seg_entry("sounding", now - retain_snd - 2 * kHour, kHour, "snd_old"),
    seg_entry("sounding", now - retain_snd, kHour, "snd_new"),
  };
  const RetentionResult r = apply_retention(m, now, RetentionPolicy{});
  CHECK(r.segments_expired == 3);
  CHECK(r.changed());
  std::vector<std::string> kept;
  for (const auto& s : r.manifest.segments)
    kept.push_back(s.path.substr(s.path.rfind('/') + 1));
  CHECK(
    (kept == std::vector<std::string>{
               "edge.seg", "new.seg", "long.seg", "snd_new.seg"}));
  CHECK(r.manifest.sources == m.sources);

  // Nothing to expire: unchanged.
  const RetentionResult r2 = apply_retention(r.manifest, now, {});
  CHECK(!r2.changed());
  CHECK(r2.manifest == r.manifest);
}

TEST(retention_field_runs_and_grids)
{
  Manifest m;
  m.sources = registry();
  const GridKey conus = 0x1111, global = 0x2222, old_domain = 0x3333;
  m.grids = {grid_entry(conus), grid_entry(global), grid_entry(old_domain)};
  for (int run = 0; run < 6; ++run)
  {
    const std::int64_t t = kBucket + run * kHour;
    // The two oldest HRRR runs used an older domain.
    const GridKey g = run < 2 ? old_domain : conus;
    m.fields.push_back(field("hrrr", t, 0, g));
    m.fields.push_back(field("hrrr", t, 1, g));
  }
  for (int run = 0; run < 3; ++run)
    m.fields.push_back(field("gfs", kBucket + run * 6 * kHour, 3, global));

  RetentionPolicy p;  // K = 4
  p.runs_by_model["gfs"] = 1;
  CHECK(p.runs_for("hrrr") == 4 && p.runs_for("gfs") == 1);
  const RetentionResult r = apply_retention(m, kBucket, p);
  CHECK(r.fields_expired == 4 + 2);
  CHECK(r.grids_expired == 1);
  CHECK(r.manifest.grids.size() == 2);
  std::size_t hrrr = 0;
  for (const auto& f : r.manifest.fields)
  {
    if (f.model == "hrrr")
    {
      ++hrrr;
      CHECK(f.run_s >= kBucket + 2 * kHour);
    }
    else
    {
      CHECK(f.run_s == kBucket + 12 * kHour);
    }
  }
  CHECK(hrrr == 8);
}

TEST(manifest_retired_list_round_trip_and_validation)
{
  Manifest m;
  m.sources = registry();
  m.segments = {seg_entry("metar", kBucket, kHour, "a")};
  m.retired = {
    RetiredEntry{"obs/metar/1/b.seg", 100},
    RetiredEntry{"stations/" + StationTable::file_name(1), 200}};
  m.validate();
  CHECK(Manifest::from_text(m.to_text()) == m);
  CHECK(m.live_paths() == std::vector<std::string>{m.segments[0].path});

  auto rejects = [&](auto mutate)
  {
    Manifest x = m;
    mutate(x);
    return throws<std::invalid_argument>([&] { x.validate(); });
  };
  CHECK(rejects([](Manifest& x) { x.retired[0].path = x.segments[0].path; }));
  CHECK(rejects([](Manifest& x) { x.retired.push_back(x.retired[0]); }));
  CHECK(rejects([](Manifest& x) { x.retired[0].path = "manifest/CURRENT"; }));
  CHECK(rejects([](Manifest& x) { x.retired[0].path = "obs/../config/x"; }));
  CHECK(rejects([](Manifest& x) { x.retired[0].path = "obs"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].bucket_seconds = 0; }));
}

TEST(publish_retires_and_gc_unlinks_after_grace)
{
  TempDir td;
  FakeClock clock;
  ManifestPublisher pub(td.p);
  pub.set_clock([&] { return clock.t; });

  Manifest m;
  m.sources = registry();
  m.stations = seal_stations(td.p, nullptr, "72357");
  m.segments = {seal(td.p, kBucket, "a", 1), seal(td.p, kBucket, "b", 2)};
  m.next_receipt_seq = 3;
  m = pub.publish(m);
  CHECK(m.retired.empty());
  CHECK(m.created_at_s == clock.t);

  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto old_snap = l.current();

  // Drop segment a and replace the station generation: both are retired
  // automatically, with the publish time.
  clock.t += 10;
  const std::int64_t retired_at = clock.t;
  const fs::path a_path = td.p / m.segments[0].path;
  const fs::path gen1 = td.p / m.stations->path;
  {
    auto t1 = StationTable::open(gen1);
    m.stations = seal_stations(td.p, t1.get(), "72358");
  }
  m.segments.erase(m.segments.begin());
  m.retired.clear();  // ignored by publish anyway
  m = pub.publish(m);
  CHECK(m.retired.size() == 2);
  for (const auto& r : m.retired)
    CHECK(r.retired_at_s == retired_at);

  // Retirement survives a restart (it lives in the manifest).
  CHECK(read_current_manifest(td.p)->retired == m.retired);

  // Within the grace period nothing is unlinked.
  clock.t = retired_at + 599;
  GcReport g = pub.collect_garbage(600);
  CHECK(g.retired_unlinked == 0 && !g.published_seq);
  CHECK(fs::exists(a_path) && fs::exists(gen1));

  // After it: one publish drops the entries, then the files go.
  clock.t = retired_at + 600;
  g = pub.collect_garbage(600);
  CHECK(g.retired_unlinked == 2);
  CHECK(g.published_seq == std::optional<std::uint64_t>(3));
  CHECK(pub.current().retired.empty());
  CHECK(!fs::exists(a_path) && !fs::exists(gen1));
  CHECK(fs::exists(td.p / m.segments[0].path));

  // A reader still holding the old snapshot keeps working (mmap).
  ObsQuery q;
  q.lat_deg = 35.0;
  q.lon_deg = -97.5;
  q.t_s = static_cast<double>(kBucket + 300);
  auto hits = old_snap->obs().knn("metar", q, 5);
  CHECK(hits.size() == 2);
  CHECK(old_snap->stations()->lookup(StationIdType::wmo, "72357").size() == 1);
  // The new snapshot works too.
  CHECK(l.refresh());
  CHECK(l.current()->obs().knn("metar", q, 5).size() == 1);
  CHECK(l.current()->stations()->generation() == 2);

  // Nothing left to do.
  g = pub.collect_garbage(600);
  CHECK(g.retired_unlinked == 0 && g.orphans_unlinked == 0 && !g.published_seq);
}

TEST(retired_file_listed_again_is_live_again)
{
  TempDir td;
  FakeClock clock;
  ManifestPublisher pub(td.p);
  pub.set_clock([&] { return clock.t; });
  Manifest m;
  m.sources = registry();
  m.segments = {seal(td.p, kBucket, "a", 1)};
  m.next_receipt_seq = 2;
  m = pub.publish(m);
  const SegmentEntry a = m.segments[0];
  m.segments.clear();
  m = pub.publish(m);
  CHECK(m.retired.size() == 1);
  // Listed again before gc (e.g. a grid used by a new run): no longer
  // retired, and gc leaves it alone.
  m.segments = {a};
  m = pub.publish(m);
  CHECK(m.retired.empty());
  clock.t += 100000;
  pub.collect_garbage(600);
  CHECK(fs::exists(td.p / a.path));
}

TEST(gc_orphans_and_empty_dirs)
{
  TempDir td;
  FakeClock clock;
  ManifestPublisher pub(td.p);
  pub.set_clock([&] { return clock.t; });
  Manifest m;
  m.sources = registry();
  m.segments = {seal(td.p, kBucket, "live", 1)};
  m.next_receipt_seq = 2;
  m = pub.publish(m);

  const std::int64_t old_t = clock.t - 1000, young_t = clock.t - 10;
  // A live file with an old mtime must stay.
  {
    struct timespec ts[2] = {
      {static_cast<time_t>(old_t), 0}, {static_cast<time_t>(old_t), 0}};
    ::utimensat(AT_FDCWD, (td.p / m.segments[0].path).c_str(), ts, 0);
  }
  const fs::path orphan_seg = td.p / "obs/metar/1762329600/crashed.seg";
  const fs::path orphan_tmp = td.p / "tmp/staging/a.bin";
  const fs::path orphan_temp =
    td.p / "stations/stations-0000000000000009.tbl.tmp.123.0";
  const fs::path young = td.p / "fields/hrrr/2025110509/f000.fld";
  const fs::path young_tmp = td.p / "tmp/b.bin";
  write_junk(orphan_seg, old_t);
  write_junk(orphan_tmp, old_t);
  write_junk(orphan_temp, old_t);
  write_junk(young, young_t);
  write_junk(young_tmp, young_t);
  // Never touched, whatever their age.
  const fs::path cfg = td.p / "config/sources.json";
  const fs::path jrn = td.p / "journal/metar-2025110509.jrn";
  const fs::path man = td.p / "manifest/notes.txt";
  write_junk(cfg, old_t);
  write_junk(jrn, old_t);
  write_junk(man, old_t);

  const GcReport g = pub.collect_garbage(600);
  CHECK(g.orphans_unlinked == 3);
  CHECK(!g.published_seq);
  CHECK(
    !fs::exists(orphan_seg) && !fs::exists(orphan_tmp) &&
    !fs::exists(orphan_temp));
  CHECK(fs::exists(young) && fs::exists(young_tmp));
  CHECK(fs::exists(cfg) && fs::exists(jrn) && fs::exists(man));
  CHECK(fs::exists(td.p / m.segments[0].path));
  // Emptied directories go; top-level and non-empty ones stay.
  CHECK(!fs::exists(orphan_seg.parent_path()));
  CHECK(!fs::exists(td.p / "tmp/staging"));
  CHECK(fs::exists(td.p / "tmp") && fs::exists(td.p / "stations"));
  CHECK(fs::exists(young.parent_path()));
  CHECK(g.dirs_removed == 2);
}

TEST(gc_crash_before_unlink_is_harmless)
{
  TempDir td;
  FakeClock clock;
  std::optional<ManifestPublisher> opt;
  opt.emplace(td.p);
  ManifestPublisher& pub = *opt;
  pub.set_clock([&] { return clock.t; });
  Manifest m;
  m.sources = registry();
  m.segments = {seal(td.p, kBucket, "a", 1)};
  m.next_receipt_seq = 2;
  m = pub.publish(m);
  const fs::path a = td.p / m.segments[0].path;
  m.segments.clear();
  pub.publish(m);
  clock.t += 601;

  // The publish inside gc fails: nothing is unlinked, the file stays
  // retired.
  pub.set_fault_hook(
    [](PublishStep s)
    {
      if (s == PublishStep::manifest_written)
        throw std::runtime_error("simulated crash");
    });
  CHECK(throws<std::runtime_error>([&] { pub.collect_garbage(600); }));
  CHECK(fs::exists(a));
  CHECK(pub.current().retired.size() == 1);
  pub.set_fault_hook(nullptr);

  // A new publisher (after the "crash") finishes the job.
  opt.reset();
  opt.emplace(td.p);
  opt->set_clock([&] { return clock.t; });
  const GcReport g = opt->collect_garbage(600);
  CHECK(g.retired_unlinked == 1);
  CHECK(!fs::exists(a));
}

TEST(retention_cycle_end_to_end)
{
  TempDir td;
  FakeClock clock;
  clock.t = kBucket + 2 * kHour;
  ManifestPublisher pub(td.p);
  pub.set_clock([&] { return clock.t; });
  Manifest m;
  m.sources = registry();
  m.segments = {
    seal(td.p, kBucket, "a", 1), seal(td.p, kBucket + kHour, "b", 2)};
  m.next_receipt_seq = 3;
  m = pub.publish(m);

  // 72 h later bucket a has expired, b not yet.
  const std::int64_t retain = registry().at("metar").retain_seconds;
  clock.t = kBucket + kHour + retain + 1;
  RetentionResult r = apply_retention(pub.current(), clock.t, {});
  CHECK(r.segments_expired == 1);
  pub.publish(r.manifest);
  clock.t += 600;
  // Only the retired file goes (the others are named; their real-time
  // mtimes are newer than this fake clock anyway), and its bucket
  // directory with it.
  const GcReport g = pub.collect_garbage(600);
  CHECK(g.retired_unlinked == 1 && g.orphans_unlinked == 0);
  CHECK(!fs::exists(td.p / "obs/metar" / std::to_string(kBucket)));
  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  CHECK(l.current()->segments().size() == 1);
}

int main()
{
  return tst::run_all();
}
