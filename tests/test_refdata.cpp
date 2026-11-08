// Reference-data import (docs/design/05-reference-data.md): stations.json
// parsing, publishing sources and stations through the manifest, keeping
// the previous version on error, metric changes, provisional stations and
// the outbox.
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/manifest.h"
#include "atmo/obs_segment.h"
#include "atmo/refdata.h"
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
        ("atmo_test_refdata_" + std::to_string(::getpid()) + "_" +
         std::to_string(std::random_device{}()));
    fs::create_directories(p / "config");
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

// Like the web app: temp file + rename, so a reader never sees a partial
// file (and the inode changes).
void export_file(
  const fs::path& root,
  const std::string& name,
  const std::string& text)
{
  const fs::path tmp = root / "config" / (name + ".tmp");
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << text;
  }
  fs::rename(tmp, root / "config" / name);
}

void export_sources(const fs::path& root, const SourceRegistry& r)
{
  export_file(root, "sources.json", r.to_json().dump(1));
}

void export_stations(const fs::path& root, const std::vector<Station>& v)
{
  StationsExport ex;
  ex.stations = v;
  export_file(root, "stations.json", ex.to_json().dump(1));
}

constexpr std::int64_t k2020 = 1577836800;

StationIdBinding bind(
  StationIdType t,
  const char* v,
  std::int64_t from_s = kTimeMin,
  std::int64_t to_s = kTimeMax)
{
  return StationIdBinding{StationId{t, v}, from_s, to_s};
}

Station okc()
{
  Station s;
  s.ref = 1;
  s.name = "Oklahoma City Will Rogers";
  s.ids = {
    bind(StationIdType::wmo, "72353"),
    bind(StationIdType::icao, "KOKC")};  // deliberately unsorted
  s.periods = {
    StationPeriod{kTimeMin, k2020, 35.39f, -97.60f, 392.0f},
    StationPeriod{k2020, kTimeMax, 35.39f, -97.60f, 395.0f}};
  return s;
}

Station oun()
{
  Station s;
  s.ref = 2;
  s.name = "Norman";
  s.ids = {bind(StationIdType::wmo, "72357")};
  s.periods = {StationPeriod{kTimeMin, kTimeMax, 35.18f, -97.44f, 357.0f}};
  return s;
}

Station unknown_ship(const char* call_sign)
{
  Station s;
  s.name = std::string("unknown ") + call_sign;
  s.flags = kStationMobile;
  s.ids = {bind(StationIdType::platform, call_sign)};
  return s;
}

constexpr std::int64_t kBucket = 1762333200;

SegmentEntry seal_metar(
  const fs::path& root,
  const SourceClass& metar,
  const std::string& id,
  std::uint64_t seq)
{
  ObsSegmentWriter w(metar, kBucket);
  ObsReport r;
  r.report_key = seq;
  r.receipt_seq = seq;
  r.receipt_time_ms = (kBucket + 600) * 1000;
  r.obs_time_s = kBucket + 300;
  r.lat_deg = 35.0f;
  r.lon_deg = -97.5f;
  r.values.push_back(ObsValue{1, 1.0f, 0});
  r.raw = "x";
  w.add_report(r);
  const fs::path p =
    root / "obs" / "metar" / std::to_string(kBucket) / (id + ".seg");
  w.seal(p);
  return SegmentEntry::from_segment(root, *ObsSegment::open(p));
}

std::vector<fs::path> outbox_files(const fs::path& root)
{
  std::vector<fs::path> v;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(root / "config" / "outbox", ec))
    v.push_back(e.path());
  std::sort(v.begin(), v.end());
  return v;
}

nlohmann::json read_json(const fs::path& p)
{
  std::ifstream in(p);
  return nlohmann::json::parse(in);
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(stations_json_round_trip_and_validation)
{
  Station merged = oun();
  merged.ref = 7;
  merged.ids = {bind(StationIdType::icao, "KOUN", 0, k2020)};
  merged.merged_into = 2;
  Station prov = unknown_ship("WDC1234");
  prov.ref = kProvisionalRefBase;
  prov.flags |= kStationProvisional;

  StationsExport ex;
  ex.stations = {okc(), oun(), merged, prov};
  const std::string text = ex.to_json().dump();
  CHECK(StationsExport::from_text(text) == ex);
  // Open-ended times are null.
  const auto j = ex.to_json();
  CHECK(j["stations"][1]["periods"][0]["valid_from"].is_null());
  CHECK(j["stations"][2]["ids"][0]["valid_from"] == 0);
  CHECK(j["stations"][2]["merged_into"] == 2);
  CHECK(j["stations"][3]["flags"].size() == 2);
  CHECK(j["stations"][0]["merged_into"].is_null());

  // Informational top-level keys are allowed.
  auto k = j;
  k["exported_at"] = "2026-11-05T12:00:00Z";
  CHECK(StationsExport::from_json(k) == ex);

  auto rejects = [&](auto mutate)
  {
    auto x = j;
    mutate(x);
    return throws<std::invalid_argument>([&] { StationsExport::from_json(x); });
  };
  CHECK(rejects([](auto& x) { x["format"] = "atmo-sources"; }));
  CHECK(rejects([](auto& x) { x["version"] = 2; }));
  CHECK(rejects([](auto& x) { x.erase("stations"); }));
  CHECK(rejects([](auto& x) { x["stations"][0]["extra"] = 1; }));
  CHECK(rejects([](auto& x) { x["stations"][0].erase("periods"); }));
  CHECK(rejects([](auto& x) { x["stations"][0]["ref"] = 0; }));
  CHECK(rejects([](auto& x) { x["stations"][0]["ref"] = -1; }));
  CHECK(rejects([](auto& x) { x["stations"][0]["ref"] = 1.5; }));
  CHECK(rejects([](auto& x) { x["stations"][0]["ref"] = 4294967296ull; }));
  CHECK(rejects([](auto& x) { x["stations"][1]["ref"] = 1; }));  // duplicate
  CHECK(rejects([](auto& x) { x["stations"][0]["flags"] = {"weird"}; }));
  CHECK(
    rejects([](auto& x) { x["stations"][3]["flags"] = {"mobile", "mobile"}; }));
  CHECK(rejects([](auto& x) { x["stations"][0]["merged_into"] = 0; }));
  CHECK(rejects([](auto& x) { x["stations"][0]["ids"][0]["type"] = "iata"; }));
  CHECK(
    rejects([](auto& x) { x["stations"][0]["ids"][0]["valid_to"] = "2020"; }));
  CHECK(
    rejects([](auto& x) { x["stations"][0]["periods"][0]["lat_deg"] = "35"; }));
  CHECK(rejects([](auto& x)
                { x["stations"][0]["periods"][0]["lat_deg"] = 1e300; }));
  CHECK(
    throws<std::invalid_argument>(
      [] { StationsExport::from_text("{\"format\":"); }));
  CHECK(
    throws<std::invalid_argument>(
      [] { StationsExport::from_file("/nonexistent/stations.json"); }));
}

TEST(import_publishes_sources_and_stations)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  RefDataImporter imp(pub);

  // Nothing to import yet.
  auto r = imp.poll();
  CHECK(!r.published_seq);
  CHECK(r.errors.size() == 1);       // sources.json missing
  CHECK(imp.poll().errors.empty());  // reported once

  // stations.json alone cannot be published (no source classes yet).
  export_stations(td.p, {okc(), oun()});
  r = imp.poll();
  CHECK(!r.published_seq);
  CHECK(read_current_seq(td.p) == std::nullopt);

  // sources.json arrives: both are published in one manifest.
  export_sources(td.p, SourceRegistry::defaults());
  r = imp.poll();
  CHECK(r.errors.empty());
  CHECK(r.sources_updated && r.stations_updated);
  CHECK(r.published_seq == std::optional<std::uint64_t>(1));
  CHECK(pub.current().sources == SourceRegistry::defaults());
  CHECK(pub.current().stations && pub.current().stations->generation == 1);

  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  auto s = l.current();
  CHECK(s->stations()->size() == 2);
  auto at = s->stations()->lookup(StationIdType::icao, "KOKC", k2020 + 10);
  CHECK(at && at->station.ref() == 1);
  CHECK(at && at->period && at->period->elevation_m == 395.0f);

  // Unchanged: nothing happens; rewritten with the same content: parsed,
  // nothing published (identifier order does not matter).
  CHECK(!imp.poll().published_seq);
  export_sources(td.p, SourceRegistry::defaults());
  Station okc2 = okc();
  std::swap(okc2.ids[0], okc2.ids[1]);
  // Both identifier orders: at least one differs from the stored order.
  for (const Station& o : {okc(), okc2})
  {
    export_stations(td.p, {oun(), o});
    r = imp.poll();
    CHECK(r.errors.empty());
    CHECK(!r.published_seq);
  }
  CHECK(pub.current().seq == 1);
}

TEST(import_keeps_previous_version_on_error)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  RefDataImporter imp(pub);
  export_sources(td.p, SourceRegistry::defaults());
  export_stations(td.p, {okc(), oun()});
  CHECK(imp.poll().published_seq == std::optional<std::uint64_t>(1));

  // Invalid sources.json (tau 0): rejected, error reported once.
  auto j = SourceRegistry::defaults().to_json();
  j["sources"][0]["tau_seconds"] = 0;
  export_file(td.p, "sources.json", j.dump());
  auto r = imp.poll();
  CHECK(r.errors.size() == 1);
  CHECK(!r.published_seq);
  CHECK(imp.poll().errors.empty());
  CHECK(pub.current().sources == SourceRegistry::defaults());

  // Invalid stations.json (KOKC bound to two stations at once): rejected
  // as a whole, generation 1 stays.
  Station dup = oun();
  dup.ids.push_back(bind(StationIdType::icao, "KOKC"));
  export_stations(td.p, {okc(), dup});
  r = imp.poll();
  CHECK(r.errors.size() == 1);
  CHECK(!r.published_seq);
  CHECK(pub.current().stations->generation == 1);

  // Dropping a web-app ref is an error too.
  export_stations(td.p, {okc()});
  r = imp.poll();
  CHECK(r.errors.size() == 1);
  CHECK(pub.current().stations->generation == 1);

  // Inventing a provisional ref as well.
  Station p = unknown_ship("WDC1");
  p.ref = kProvisionalRefBase + 5;
  p.flags |= kStationProvisional;
  export_stations(td.p, {okc(), oun(), p});
  r = imp.poll();
  CHECK(r.errors.size() == 1);
  CHECK(pub.current().stations->generation == 1);

  // A new web-app station above the high-water mark is added.
  Station three = oun();
  three.ref = 3;
  three.ids = {bind(StationIdType::wmo, "72358")};
  export_stations(td.p, {okc(), oun(), three});
  r = imp.poll();
  CHECK(r.errors.empty());
  CHECK(pub.current().stations->generation == 2);

  // Fixed files are imported; one manifest carries both.
  j["sources"][0]["tau_seconds"] = 1800;
  export_file(td.p, "sources.json", j.dump());
  Station okc_moved = okc();
  okc_moved.periods[1].elevation_m = 396.0f;
  export_stations(td.p, {okc_moved, oun(), three});
  r = imp.poll();
  CHECK(r.errors.empty());
  CHECK(r.sources_updated && r.stations_updated);
  CHECK(pub.current().stations->generation == 3);
  CHECK(pub.current().sources.all()[0].tau_seconds == 1800.0);
  // Refs stay stable across generations.
  auto t = StationTable::open(td.p / pub.current().stations->path);
  CHECK(t->by_ref(1) && t->by_ref(1)->period(1).elevation_m == 396.0f);
  CHECK(t->by_ref(3) && t->by_ref(3)->name() == "Norman");
}

TEST(import_metric_change_and_source_removal)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  RefDataImporter imp(pub);
  const SourceRegistry def = SourceRegistry::defaults();
  export_sources(td.p, def);
  CHECK(imp.poll().published_seq);

  Manifest m = pub.current();
  m.segments.push_back(seal_metar(td.p, def.at("metar"), "a", 1));
  m.next_receipt_seq = 2;
  pub.publish(m);

  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  CHECK(l.current()->obs().rebuilt_trees() == 0);

  // Changing the metar metric: published; the snapshot rebuilds the tree
  // of the old segment (Q5b).
  std::vector<SourceClass> v = def.all();
  for (auto& c : v)
    if (c.name == "metar")
      c.L_km = 150.0;
  export_sources(td.p, SourceRegistry(v));
  auto r = imp.poll();
  CHECK(r.sources_updated && r.errors.empty());
  CHECK(l.refresh());
  CHECK(l.current()->sources().at("metar").L_km == 150.0);
  CHECK(l.current()->obs().rebuilt_trees() == 1);
  CHECK(l.current()->segments().size() == 1);

  // Removing metar while a metar segment is retained: rejected.
  std::vector<SourceClass> w;
  for (const auto& c : v)
    if (c.name != "metar")
      w.push_back(c);
  export_sources(td.p, SourceRegistry(w));
  r = imp.poll();
  CHECK(r.errors.size() == 1);
  CHECK(!r.published_seq);
  CHECK(pub.current().sources.find("metar") != nullptr);
}

TEST(provisional_stations_and_outbox)
{
  TempDir td;
  {
    ManifestPublisher pub(td.p);
    RefDataImporter imp(pub);
    CHECK(
      throws<std::invalid_argument>(
        [&] { imp.add_provisional({unknown_ship("WDC1")}); }));
    export_sources(td.p, SourceRegistry::defaults());
    export_stations(td.p, {okc(), oun()});
    CHECK(imp.poll().published_seq);
    CHECK(outbox_files(td.p).empty());

    Station with_ref = unknown_ship("WDC9");
    with_ref.ref = 5;
    CHECK(
      throws<std::invalid_argument>([&] { imp.add_provisional({with_ref}); }));

    const auto refs =
      imp.add_provisional({unknown_ship("WDC1"), unknown_ship("WDC2")});
    CHECK(refs.size() == 2);
    CHECK(refs[0] == kProvisionalRefBase);
    CHECK(refs[1] == kProvisionalRefBase + 1);
    CHECK(pub.current().stations->generation == 2);
    auto files = outbox_files(td.p);
    CHECK(files.size() == 1);
    CHECK(
      !files.empty() &&
      files[0].filename() == "provisional-0000000000000002.json");
    if (!files.empty())
    {
      const auto j = read_json(files[0]);
      CHECK(j["format"] == StationsExport::kOutboxFormat);
      CHECK(j["generation"] == 2);
      CHECK(j["stations"].size() == 2);
      CHECK(j["stations"][0]["ref"] == kProvisionalRefBase);
      CHECK(j["stations"][0]["flags"].size() == 2);
    }

    // An export from before the web app imported them keeps them.
    Station okc_moved = okc();
    okc_moved.name = "Will Rogers World Airport";
    export_stations(td.p, {okc_moved, oun()});
    auto r = imp.poll();
    CHECK(r.errors.empty() && r.stations_updated);
    auto t = StationTable::open(td.p / pub.current().stations->path);
    CHECK(t->by_ref(kProvisionalRefBase).has_value());
    CHECK(t->max_provisional_ref() == kProvisionalRefBase + 1);

    // The web app consumed the file.  The next provisional station's file
    // lists every station it does not know yet.
    fs::remove(files[0]);
    CHECK(
      imp.add_provisional({unknown_ship("WDC3")})[0] ==
      kProvisionalRefBase + 2);
    files = outbox_files(td.p);
    CHECK(files.size() == 1);
    if (!files.empty())
      CHECK(read_json(files[0])["stations"].size() == 3);
    fs::remove(files[0]);

    // The web app imported and confirmed the first two (flag cleared,
    // ref kept) and merged the second into OUN.
    Station c1 = unknown_ship("WDC1");
    c1.ref = kProvisionalRefBase;
    Station c2 = unknown_ship("WDC2");
    c2.ref = kProvisionalRefBase + 1;
    c2.merged_into = 2;
    export_stations(td.p, {okc_moved, oun(), c1, c2});
    r = imp.poll();
    CHECK(r.errors.empty() && r.stations_updated);
    t = StationTable::open(td.p / pub.current().stations->path);
    CHECK(t->by_ref(kProvisionalRefBase)->flags() == kStationMobile);
    CHECK(t->by_ref(kProvisionalRefBase + 1)->merged_into() == 2);
    CHECK(t->by_ref(kProvisionalRefBase + 2).has_value());  // still pending

    imp.add_provisional({unknown_ship("WDC4")});
    files = outbox_files(td.p);
    CHECK(files.size() == 1);
    if (!files.empty())
    {
      const auto j = read_json(files[0]);
      CHECK(j["stations"].size() == 2);  // WDC3 and WDC4
      CHECK(j["stations"][0]["ref"] == kProvisionalRefBase + 2);
    }
    fs::remove(files[0]);
  }
  // Restart (e.g. after a crash between publishing and writing the
  // outbox): pending stations are announced again.
  ManifestPublisher pub(td.p);
  RefDataImporter imp(pub);
  const auto files = outbox_files(td.p);
  CHECK(files.size() == 1);
  if (!files.empty())
    CHECK(read_json(files[0])["stations"].size() == 2);
}

TEST(import_replaces_unpublished_generation_leftover)
{
  TempDir td;
  ManifestPublisher pub(td.p);
  RefDataImporter imp(pub);
  export_sources(td.p, SourceRegistry::defaults());
  export_stations(td.p, {okc()});
  CHECK(imp.poll().published_seq);

  // A generation 2 sealed by a publish that crashed before the manifest.
  {
    auto t = StationTable::open(td.p / pub.current().stations->path);
    StationTableBuilder b(*t);
    b.add(unknown_ship("JUNK"));
    b.seal(td.p / "stations");
  }
  export_stations(td.p, {okc(), oun()});
  auto r = imp.poll();
  CHECK(r.errors.empty());
  CHECK(pub.current().stations->generation == 2);
  SnapshotLoader l(td.p);
  CHECK(l.refresh());
  CHECK(l.current()->stations()->size() == 2);
  CHECK(l.current()->stations()->by_ref(2)->name() == "Norman");
}

int main()
{
  return tst::run_all();
}
