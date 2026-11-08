// Manifest, publisher lock and snapshots: acceptance tests 1-4 of
// docs/design/02-manifest-and-processes.md plus manifest validation.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atmo/gridstore.h"
#include "atmo/manifest.h"
#include "atmo/obs_segment.h"
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
        ("atmo_test_manifest_" + std::to_string(::getpid()) + "_" +
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

constexpr std::int64_t kBucket = 1762333200;  // 2025-11-05 09:00Z

ObsReport make_report(std::uint64_t key, std::uint64_t seq, float lat)
{
  ObsReport r;
  r.report_key = key;
  r.receipt_seq = seq;
  r.receipt_time_ms = (kBucket + 600) * 1000;
  r.obs_time_s = kBucket + 300;
  r.lat_deg = lat;
  r.lon_deg = -97.5f;
  r.height_m = 300.0f;
  r.values.push_back(ObsValue{1, static_cast<float>(seq), 0});
  r.raw = "K" + std::to_string(key) + "v" + std::to_string(seq);
  return r;
}

// Seals obs/metar/<bucket>/<id>.seg under `root` with one report per
// (key, seq) pair and returns its manifest entry.
SegmentEntry seal_segment(
  const fs::path& root,
  const std::string& id,
  const std::vector<std::pair<std::uint64_t, std::uint64_t>>& reps)
{
  ObsSegmentWriter w(registry().at("metar"), kBucket);
  for (const auto& [key, seq] : reps)
  {
    w.add_report(
      make_report(key, seq, 35.0f + 0.01f * static_cast<float>(key)));
  }
  const fs::path p =
    root / "obs" / "metar" / std::to_string(kBucket) / (id + ".seg");
  w.seal(p);
  return SegmentEntry::from_segment(root, *ObsSegment::open(p));
}

Manifest base_manifest()
{
  Manifest m;
  m.sources = registry();
  return m;
}

// Every file of the snapshot opens with a full CRC check and matches its
// entry; the obs index answers.  Returns false on any problem.
bool verify_snapshot(const fs::path& root, const Snapshot& s)
{
  try
  {
    const Manifest& m = s.manifest();
    if (m.segments.size() != s.segments().size())
      return false;
    for (const auto& e : m.segments)
    {
      auto seg = ObsSegment::open(root / e.path, ObsSegment::Verify::full);
      if (seg->header_crc() != e.header_crc)
        return false;
    }
    ObsQuery q;
    q.lat_deg = 35.0;
    q.lon_deg = -97.5;
    q.t_s = static_cast<double>(kBucket + 300);
    const auto hits = s.obs().knn("metar", q, 3);
    // Older versions are superseded: one visible report per key.
    std::set<std::uint64_t> keys;
    for (const auto& seg : s.segments())
      keys.insert(seg->report_keys(), seg->report_keys() + seg->size());
    return hits.size() == std::min<std::size_t>(3, keys.size());
  }
  catch (...)
  {
    return false;
  }
}

// A tiny regular lat/lon grid, stored under root/grids.
GridEntry put_grid(const fs::path& root)
{
  GridIdentity id;
  const std::string canon = "latlon 4x3 test";
  id.canon.assign(canon.begin(), canon.end());
  id.ni = 4;
  id.nj = 3;
  GridData d;
  for (int j = 0; j < 3; ++j)
  {
    for (int i = 0; i < 4; ++i)
    {
      d.lat_deg.push_back(30.0 + j);
      d.lon_deg.push_back(-100.0 + i);
    }
  }
  GridStore store(root / "grids");
  store.put(id, d);
  GridEntry e;
  e.key = id.key();
  e.path = "grids/" + key_hex(e.key) + ".grid";
  e.header_crc = store.header_crc(e.key);
  return e;
}

StationsEntry put_stations(const fs::path& root)
{
  StationTableBuilder b;
  Station s;
  s.name = "Norman";
  s.ids = {StationIdBinding{
    StationId{StationIdType::wmo, "72357"}, kTimeMin, kTimeMax}};
  s.periods = {StationPeriod{kTimeMin, kTimeMax, 35.18f, -97.44f, 357.0f}};
  b.add(s);
  const fs::path p = b.seal(root / "stations");
  auto t = StationTable::open(p);
  return StationsEntry{
    t->generation(), "stations/" + p.filename().string(), t->header_crc()};
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(manifest_json_round_trip_and_validation)
{
  TempDir td;
  Manifest m = base_manifest();
  m.seq = 7;
  m.created_at_s = 1762333300;
  m.next_receipt_seq = 10;
  m.stations = StationsEntry{3, "stations/" + StationTable::file_name(3), 99};
  m.grids.push_back(
    GridEntry{
      0xabcdef0123456789ULL, "grids/abcdef0123456789.grid", 0xffffffffu});
  m.fields.push_back(
    FieldEntry{
      "hrrr", 1762333200, 3, 0xabcdef0123456789ULL,
      "fields/hrrr/2025110509/f003.fld", 5});
  m.segments.push_back(
    SegmentEntry{
      "metar", kBucket, 3600, "obs/metar/1762333200/a.seg", 1, 9, 4, 6});
  m.segments.push_back(
    SegmentEntry{
      "metar", kBucket, 3600, "obs/metar/1762333200/empty.seg", 0, 0, 0, 7});
  m.validate();
  CHECK(Manifest::from_text(m.to_text()) == m);
  CHECK(Manifest::file_name(0x1a) == "MANIFEST-000000000000001a.json");

  auto rejects = [](auto mutate)
  {
    Manifest x = base_manifest();
    x.next_receipt_seq = 10;
    x.grids.push_back(GridEntry{1, "grids/0000000000000001.grid", 0});
    x.segments.push_back(
      SegmentEntry{"metar", kBucket, 3600, "obs/metar/1/a.seg", 1, 2, 2, 0});
    x.validate();
    mutate(x);
    return throws<std::invalid_argument>([&] { x.validate(); });
  };
  CHECK(rejects([](Manifest& x) { x.segments[0].path = "obs/metar/../a"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].path = "/obs/metar/a"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].path = "obs/synop/a.seg"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].path = "obs/metar//a"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].path = "obs/metar"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].source = "nope"; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].receipt_seq_max = 10; }));
  CHECK(rejects([](Manifest& x) { x.segments[0].receipt_seq_min = 3; }));
  CHECK(rejects([](Manifest& x) { x.segments.push_back(x.segments[0]); }));
  CHECK(rejects([](Manifest& x) { x.grids[0].path = "grids/1.grid"; }));
  CHECK(rejects([](Manifest& x) { x.grids.push_back(x.grids[0]); }));
  CHECK(rejects(
    [](Manifest& x)
    {
      x.fields.push_back(
        FieldEntry{"hrrr", 0, 0, 2, "fields/hrrr/2025110509/f000.fld", 0});
    }));
  CHECK(rejects(
    [](Manifest& x)
    {
      x.fields.push_back(
        FieldEntry{"Bad", 0, 0, 1, "fields/Bad/2025110509/f000.fld", 0});
    }));
  CHECK(
    rejects([](Manifest& x)
            { x.stations = StationsEntry{1, "stations/whatever.tbl", 0}; }));
  CHECK(rejects([](Manifest& x) { x.next_receipt_seq = 0; }));

  // Strict JSON: unknown and missing keys, bad key hex, wrong format.
  nlohmann::json j = m.to_json();
  j["extra"] = 1;
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(j); }));
  j = m.to_json();
  j["segments"][0].erase("report_count");
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(j); }));
  j = m.to_json();
  j["grids"][0]["key"] = "ABCDEF0123456789";
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(j); }));
  j = m.to_json();
  j["segments"][0]["receipt_seq_min"] = -1;
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(j); }));
  j = m.to_json();
  j["format"] = "atmo-sources";
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_json(j); }));
  CHECK(throws<std::invalid_argument>([&] { Manifest::from_text("{"); }));
}

TEST(publisher_publish_read_prune)
{
  TempDir td;
  CHECK(!read_current_seq(td.p));
  CHECK(!read_current_manifest(td.p));
  {
    ManifestPublisher pub(td.p, 3);
    CHECK(pub.current().seq == 0);
    for (int i = 1; i <= 5; ++i)
    {
      Manifest m = pub.current();
      if (i == 1)
        m = base_manifest();
      const std::uint64_t seq = m.next_receipt_seq;
      m.segments.push_back(
        seal_segment(td.p, "s" + std::to_string(i), {{100u + i, seq}}));
      m.next_receipt_seq = seq + 1;
      const Manifest& out = pub.publish(m);
      CHECK(out.seq == static_cast<std::uint64_t>(i));
      CHECK(out.created_at_s > 0);
    }
    auto cur = read_current_manifest(td.p);
    CHECK(cur && *cur == pub.current());
    CHECK(read_current_seq(td.p) == std::optional<std::uint64_t>(5));
    // keep = 3: manifests 3..5 remain.
    std::size_t manifests = 0;
    for (const auto& e : fs::directory_iterator(td.p / "manifest"))
    {
      const std::string n = e.path().filename().string();
      manifests += n.rfind("MANIFEST-", 0) == 0;
      CHECK(n.find(".tmp") == std::string::npos);
    }
    CHECK(manifests == 3);
    CHECK(read_manifest(td.p, 3).seq == 3);
    CHECK(read_manifest(td.p, 3).segments.size() == 3);
    CHECK(throws<std::runtime_error>([&] { read_manifest(td.p, 2); }));

    // Rejected publishes leave the state alone.
    Manifest bad = pub.current();
    bad.segments.push_back(
      SegmentEntry{
        "metar", kBucket, 3600, "obs/metar/1/missing.seg", 0, 0, 0, 0});
    CHECK(throws<std::invalid_argument>([&] { pub.publish(bad); }));
    bad = pub.current();
    bad.next_receipt_seq -= 1;
    bad.segments.clear();
    CHECK(throws<std::invalid_argument>([&] { pub.publish(bad); }));
    CHECK(pub.current().seq == 5);
    CHECK(read_current_seq(td.p) == std::optional<std::uint64_t>(5));
  }
  // A new publisher picks up where the old one stopped.
  ManifestPublisher pub2(td.p, 3);
  CHECK(pub2.current().seq == 5);
  CHECK(pub2.publish(pub2.current()).seq == 6);

  // A corrupted manifest is detected through CURRENT's CRC.
  {
    const fs::path p = td.p / "manifest" / Manifest::file_name(6);
    std::string text;
    {
      std::ifstream in(p);
      text.assign(std::istreambuf_iterator<char>(in), {});
    }
    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add);
    const auto pos = text.find("\"next_receipt_seq\": ") + 20;
    text[pos] = text[pos] == '9' ? '8' : '9';
    std::ofstream(p, std::ios::trunc) << text;
    CHECK(throws<std::runtime_error>([&] { read_current_manifest(td.p); }));
    SnapshotLoader l(td.p);
    CHECK(throws<std::runtime_error>([&] { l.refresh(); }));
    CHECK(l.current() == nullptr);
  }
  // A malformed CURRENT too.
  std::ofstream(td.p / "manifest" / "CURRENT", std::ios::trunc)
    << "atmo-current 1 6 zz\n";
  CHECK(throws<std::runtime_error>([&] { read_current_seq(td.p); }));
}

// Acceptance 1: one publisher, 8 readers, 100 manifests.
TEST(accept1_publish_adopt_concurrent)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  SnapshotLoader loader(td.p);
  std::atomic<bool> done{false};
  std::atomic<int> failures{0}, regressions{0};
  std::atomic<long> checked{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 8; ++r)
  {
    readers.emplace_back(
      [&]
      {
        std::uint64_t last = 0;
        bool final_pass = false;
        while (!final_pass)
        {
          final_pass = done.load();
          try
          {
            loader.refresh();
          }
          catch (...)
          {
            ++failures;
          }
          auto s = loader.current();
          if (!s)
            continue;
          if (s->seq() < last)
            ++regressions;
          last = s->seq();
          if (!verify_snapshot(td.p, *s))
            ++failures;
          ++checked;
        }
      });
  }

  Manifest m = base_manifest();
  for (int i = 0; i < 100; ++i)
  {
    const std::uint64_t seq = m.next_receipt_seq;
    // Two versions of a report per publish, plus a correction of the
    // previous publish's report, so visibility changes as well.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reps = {
      {1000u + static_cast<unsigned>(i), seq},
      {1000u + static_cast<unsigned>(i), seq + 1}};
    if (i > 0)
      reps.push_back({999u + static_cast<unsigned>(i), seq + 2});
    m.segments.push_back(seal_segment(td.p, "p" + std::to_string(i), reps));
    m.next_receipt_seq = seq + 3;
    m = pub.publish(m);
  }
  done = true;
  for (auto& t : readers)
    t.join();

  CHECK(failures == 0);
  CHECK(regressions == 0);
  CHECK(checked > 0);
  auto s = loader.current();
  CHECK(s && s->seq() == 100);
  CHECK(s && s->segments().size() == 100);
}

// Acceptance 2: a publisher that dies after any step leaves a consistent
// state (old or new), and a new publisher continues from it.
TEST(accept2_crash_points)
{
  const PublishStep steps[] = {
    PublishStep::manifest_written, PublishStep::current_written,
    PublishStep::current_renamed};
  for (int k = -1; k < 3; ++k)
  {
    TempDir td;
    std::uint64_t want_seq = 0;
    {
      ManifestPublisher pub(td.p);
      Manifest m = base_manifest();
      m.segments.push_back(seal_segment(td.p, "a", {{1, 1}}));
      m.next_receipt_seq = 2;
      m = pub.publish(m);
      // The new data file is written in every case.
      m.segments.push_back(seal_segment(td.p, "b", {{1, 2}, {2, 3}}));
      m.next_receipt_seq = 4;
      if (k < 0)
      {
        // Crash before publish: the file exists but nothing names it.
        want_seq = 1;
      }
      else
      {
        const PublishStep crash_at = steps[k];
        pub.set_fault_hook(
          [crash_at](PublishStep s)
          {
            if (s == crash_at)
              throw std::runtime_error("simulated crash");
          });
        CHECK(throws<std::runtime_error>([&] { pub.publish(m); }));
        want_seq = crash_at == PublishStep::current_renamed ? 2 : 1;
        CHECK(pub.current().seq == want_seq);
      }
    }
    // A fresh reader sees a complete snapshot: old or new, never partial.
    SnapshotLoader l(td.p);
    CHECK(l.refresh());
    auto s = l.current();
    CHECK(s && s->seq() == want_seq);
    CHECK(s && s->segments().size() == want_seq);
    CHECK(s && verify_snapshot(td.p, *s));
    CHECK(s && s->obs().max_receipt_seq() == (want_seq == 1 ? 1u : 3u));

    // A new publisher recovers (no collision with a half-published
    // MANIFEST file) and publishes the next seq.
    ManifestPublisher pub(td.p);
    CHECK(pub.current().seq == want_seq);
    CHECK(!fs::exists(td.p / "manifest" / "CURRENT.tmp"));
    CHECK(!fs::exists(td.p / "manifest" / Manifest::file_name(want_seq + 1)));
    Manifest m = pub.current();
    m.segments.push_back(seal_segment(td.p, "c", {{3, 4}}));
    m.next_receipt_seq = 5;
    CHECK(pub.publish(m).seq == want_seq + 1);
    CHECK(l.refresh());
    CHECK(l.current()->seq() == want_seq + 1);
    CHECK(verify_snapshot(td.p, *l.current()));
  }
}

// Acceptance 3: a retired file disappears from the manifest and is
// unlinked while a reader still holds the old snapshot.
TEST(accept3_retirement)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  Manifest m = base_manifest();
  m.segments.push_back(seal_segment(td.p, "old", {{1, 1}}));
  m.segments.push_back(seal_segment(td.p, "keep", {{2, 2}}));
  m.next_receipt_seq = 3;
  m = pub.publish(m);

  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  CHECK(!l.refresh());  // nothing new
  auto s1 = l.current();
  CHECK(s1->files_opened() == 2);

  const fs::path old_path = td.p / m.segments[0].path;
  m.segments.erase(m.segments.begin());
  pub.publish(m);
  CHECK(fs::remove(old_path));

  // The old snapshot still answers from the unlinked file.
  ObsQuery q;
  q.lat_deg = 35.01;
  q.lon_deg = -97.5;
  q.t_s = static_cast<double>(kBucket + 300);
  auto hits = s1->obs().knn("metar", q, 2);
  CHECK(hits.size() == 2);
  CHECK(!hits.empty() && hits[0].report.report_key == 1);
  CHECK(!hits.empty() && hits[0].report.raw == "K1v1");

  // The new snapshot works, without the retired file, reusing the other.
  CHECK(l.refresh());
  auto s2 = l.current();
  CHECK(s2->seq() == 2);
  CHECK(s2->files_opened() == 0);
  CHECK(s2->segments().size() == 1);
  CHECK(s2->segments()[0] == s1->segments()[1]);
  hits = s2->obs().knn("metar", q, 2);
  CHECK(hits.size() == 1);
  CHECK(!hits.empty() && hits[0].report.report_key == 2);
}

// Acceptance 4: a second publisher cannot take the lock.
TEST(accept4_second_publisher_fails)
{
  TempDir td;
  auto try_in_child = [&]() -> int
  {
    const pid_t pid = ::fork();
    if (pid == 0)
    {
      int rc = 2;
      try
      {
        ManifestPublisher p(td.p);
        rc = 0;  // got the lock
      }
      catch (const std::runtime_error&)
      {
        rc = 1;  // lock held
      }
      ::_exit(rc);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  };
  {
    ManifestPublisher pub(td.p);
    CHECK(try_in_child() == 1);
    // Also from the same process (separate open file description).
    CHECK(throws<std::runtime_error>([&] { ManifestPublisher p2(td.p); }));
    CHECK(pub.publish(base_manifest()).seq == 1);
  }
  // Released when the publisher goes away.
  CHECK(try_in_child() == 0);
  ManifestPublisher again(td.p);
  CHECK(again.current().seq == 1);
}

TEST(snapshot_grids_stations_and_mismatch)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  Manifest m = base_manifest();
  m.grids.push_back(put_grid(td.p));
  m.stations = put_stations(td.p);
  m.segments.push_back(seal_segment(td.p, "a", {{1, 1}}));
  m.next_receipt_seq = 2;
  m = pub.publish(m);

  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto s1 = l.current();
  CHECK(s1->files_opened() == 3);
  CHECK(s1->grid(m.grids[0].key) != nullptr);
  CHECK(s1->grid(m.grids[0].key)->identity().ni == 4);
  CHECK(s1->grid(12345) == nullptr);
  CHECK(s1->stations() && s1->stations()->generation() == 1);
  CHECK(s1->stations()->lookup(StationIdType::wmo, "72357").size() == 1);

  // Unchanged files are reused by the next snapshot.
  m.segments.push_back(seal_segment(td.p, "b", {{2, 2}}));
  m.next_receipt_seq = 3;
  m = pub.publish(m);
  CHECK(l.refresh());
  auto s2 = l.current();
  CHECK(s2->files_opened() == 1);
  CHECK(s2->grid(m.grids[0].key) == s1->grid(m.grids[0].key));
  CHECK(s2->stations() == s1->stations());

  // An entry that disagrees with its file is refused; the old snapshot
  // stays current.
  Manifest bad = m;
  bad.segments.push_back(seal_segment(td.p, "c", {{3, 3}}));
  bad.segments.back().header_crc ^= 1;
  bad.next_receipt_seq = 4;
  pub.publish(bad);
  CHECK(throws<std::runtime_error>([&] { l.refresh(); }));
  CHECK(l.current() == s2);

  bad.segments.back().header_crc ^= 1;
  bad.grids[0].header_crc ^= 1;
  pub.publish(bad);
  CHECK(throws<std::runtime_error>([&] { l.refresh(); }));
  CHECK(l.current() == s2);

  bad.grids[0].header_crc ^= 1;
  bad.segments.back().report_count = 2;
  bad.next_receipt_seq = 5;
  pub.publish(bad);
  CHECK(throws<std::runtime_error>([&] { l.refresh(); }));

  // Fixed: adopted.
  bad.segments.back().report_count = 1;
  pub.publish(bad);
  CHECK(l.refresh());
  CHECK(l.current()->seq() == 6);
}

int main()
{
  return tst::run_all();
}
