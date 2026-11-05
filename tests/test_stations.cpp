// Station table (T3): round trip, ref stability across generations, lookup
// by identifier and by (identifier, time), validation, corruption.
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "atmo/stations.hpp"
#include "check.hpp"

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
        ("atmo_test_stations_" + std::to_string(::getpid()) + "_" +
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

constexpr std::int64_t k2020 = 1577836800;  // 2020-01-01T00:00Z
constexpr std::int64_t k2024 = 1704067200;  // 2024-01-01T00:00Z

StationIdBinding bind(
  StationIdType t,
  const char* v,
  std::int64_t from_s = kTimeMin,
  std::int64_t to_s = kTimeMax)
{
  return StationIdBinding{StationId{t, v}, from_s, to_s};
}

StationPeriod at(
  double lat_deg,
  double lon_deg,
  float elev_m,
  std::int64_t from_s = kTimeMin,
  std::int64_t to_s = kTimeMax)
{
  return StationPeriod{from_s, to_s, lat_deg, lon_deg, elev_m};
}

// Oklahoma City: ICAO + WMO, re-surveyed in 2020 (elevation changed).
Station okc()
{
  Station s;
  s.name = "Oklahoma City Will Rogers";
  s.ids = {
    bind(StationIdType::icao, "KOKC"), bind(StationIdType::wmo, "72353")};
  s.periods = {
    at(35.39, -97.60, 392.0f, kTimeMin, k2020),
    at(35.39, -97.60, 395.0f, k2020, kTimeMax)};
  return s;
}

// Norman: WMO upper-air site, WIGOS id too.
Station oun()
{
  Station s;
  s.name = "Norman";
  s.ids = {
    bind(StationIdType::wmo, "72357"),
    bind(StationIdType::wigos, "0-20000-0-72357")};
  s.periods = {at(35.18, -97.44, 357.0f)};
  return s;
}

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

}  // namespace

TEST(stations_round_trip)
{
  TempDir td;
  StationTableBuilder b;
  CHECK(b.generation() == 1);
  const std::uint32_t r1 = b.add(okc());
  const std::uint32_t r2 = b.add(oun());
  Station ship;
  ship.name = "RV Example";
  ship.flags = kStationMobile;
  ship.ids = {bind(StationIdType::platform, "WDC1234")};
  const std::uint32_t r3 = b.add(ship);  // mobile: no periods
  CHECK(r1 == 1 && r2 == 2 && r3 == 3);
  const fs::path p = b.seal(td.p / "stations");
  CHECK(p.filename() == StationTable::file_name(1));
  CHECK(p.filename() == "stations-0000000000000001.tbl");
  CHECK(fs::file_size(p) % 4096 == 0);
  // Nothing else in the directory (no temp files).
  CHECK(
    std::distance(
      fs::directory_iterator(p.parent_path()), fs::directory_iterator()) == 1);

  const auto t = StationTable::open(p);
  CHECK(t->generation() == 1);
  CHECK(t->size() == 3);
  CHECK(t->max_ref() == 3);
  Station want = okc();
  want.ref = r1;
  const auto v = t->by_ref(r1);
  CHECK(v.has_value());
  if (v)
  {
    // Identifiers come back in table order: (type, value).
    CHECK(v->to_station().name == want.name);
    CHECK(v->to_station().periods == want.periods);
    CHECK(v->n_ids() == 2);
    CHECK(v->id(0).type == StationIdType::icao && v->id(0).value == "KOKC");
    CHECK(v->id(1).type == StationIdType::wmo && v->id(1).value == "72353");
    CHECK(v->flags() == 0);
  }
  const auto s3 = t->by_ref(r3);
  CHECK(s3 && s3->flags() == kStationMobile && s3->n_periods() == 0);
  CHECK(s3 && s3->name() == "RV Example");
  CHECK(!t->by_ref(0).has_value());
  CHECK(!t->by_ref(4).has_value());
  // Stations iterate in ref order.
  CHECK(t->station(0).ref() == 1 && t->station(2).ref() == 3);
}

TEST(stations_refs_stable_across_generations)
{
  TempDir td;
  StationTableBuilder b1;
  const std::uint32_t okc_ref = b1.add(okc());
  const std::uint32_t oun_ref = b1.add(oun());
  const auto g1 = StationTable::open(b1.seal(td.p));

  // Generation 2 starts from generation 1: every station is carried over.
  StationTableBuilder b2(*g1);
  CHECK(b2.generation() == 2);
  CHECK(b2.size() == 2);
  CHECK(b2.find(okc_ref) && b2.find(okc_ref)->name == okc().name);
  // An edit (station moved in 2024) keeps the ref.
  Station moved = *b2.find(oun_ref);
  moved.periods.back().valid_to_s = k2024;
  moved.periods.push_back(at(35.20, -97.45, 360.0f, k2024));
  b2.update(moved);
  // New stations get refs above everything that existed.
  Station tik;
  tik.name = "Tinker AFB";
  tik.ids = {bind(StationIdType::icao, "KTIK")};
  tik.periods = {at(35.41, -97.39, 389.0f)};
  const std::uint32_t tik_ref = b2.add(tik);
  CHECK(tik_ref == 3);
  // An explicit ref (from MySQL) must not reuse an old one.
  Station dup = tik;
  dup.ids = {bind(StationIdType::icao, "KPWA")};
  dup.ref = okc_ref;
  CHECK(throws<std::invalid_argument>([&] { b2.add(dup); }));
  dup.ref = 2;
  CHECK(throws<std::invalid_argument>([&] { b2.add(dup); }));
  dup.ref = 100;  // gaps are fine (AUTO_INCREMENT can skip)
  CHECK(b2.add(dup) == 100);
  CHECK(b2.max_ref() == 100);
  Station unknown = tik;
  unknown.ref = 55;
  CHECK(throws<std::out_of_range>([&] { b2.update(unknown); }));
  const auto g2 = StationTable::open(b2.seal(td.p));

  CHECK(g2->generation() == 2 && g2->size() == 4 && g2->max_ref() == 100);
  CHECK(g2->by_ref(okc_ref)->name() == okc().name);
  CHECK(g2->by_ref(oun_ref)->n_periods() == 2);
  CHECK(g2->by_ref(tik_ref)->name() == "Tinker AFB");
  // Generation 1 is untouched and still readable.
  CHECK(g1->size() == 2 && g1->by_ref(oun_ref)->n_periods() == 1);
  CHECK(!g1->by_ref(tik_ref).has_value());
  // Next generation after a gap continues above max_ref.
  StationTableBuilder b3(*g2);
  Station x = tik;
  x.ids = {bind(StationIdType::icao, "KHSD")};
  // A gap below max_ref (never assigned) is not handed out either: refs
  // only grow, so an explicit ref must be above the previous max_ref.
  Station gap = x;
  gap.ref = 50;
  CHECK(throws<std::invalid_argument>([&] { b3.add(gap); }));
  CHECK(b3.add(x) == 101);
}

TEST(stations_lookup_by_id_and_time)
{
  TempDir td;
  StationTableBuilder b;
  const std::uint32_t okc_ref = b.add(okc());
  const std::uint32_t oun_ref = b.add(oun());
  // An ICAO code reassigned: KXXX was station A until 2020, then station B.
  Station a;
  a.name = "Old field";
  a.ids = {bind(StationIdType::icao, "KXXX", kTimeMin, k2020)};
  a.periods = {at(36.0, -98.0, 400.0f, kTimeMin, k2020)};
  Station bb;
  bb.name = "New field";
  bb.ids = {bind(StationIdType::icao, "KXXX", k2020)};
  bb.periods = {at(36.5, -98.5, 420.0f, k2020)};
  const std::uint32_t a_ref = b.add(a);
  const std::uint32_t b_ref = b.add(bb);
  // Provisional: seen in a report, no position known yet.
  Station prov;
  prov.name = "KZZZ (provisional)";
  prov.flags = kStationProvisional;
  prov.ids = {bind(StationIdType::icao, "KZZZ")};
  const std::uint32_t prov_ref = b.add(prov);
  const auto t = StationTable::open(b.seal(td.p));

  // By id: every binding, in time order.
  auto all = t->lookup(StationIdType::icao, "KXXX");
  CHECK(all.size() == 2);
  if (all.size() == 2)
  {
    CHECK(all[0].ref == a_ref && all[0].valid_to_s == k2020);
    CHECK(all[1].ref == b_ref && all[1].valid_from_s == k2020);
  }
  CHECK(t->lookup(StationIdType::icao, "KOKC").size() == 1);
  const auto wmo = t->lookup(StationIdType::wmo, "72357");
  CHECK(wmo.size() == 1 && wmo[0].ref == oun_ref);
  const auto wigos = t->lookup(StationIdType::wigos, "0-20000-0-72357");
  CHECK(wigos.size() == 1 && wigos[0].ref == oun_ref);
  // Type matters: the same text under another type is a different id.
  CHECK(t->lookup(StationIdType::wmo, "KOKC").empty());
  CHECK(t->lookup(StationIdType::icao, "KNOPE").empty());

  // By (id, time): the right station and the right period.
  auto old_ = t->lookup(StationIdType::icao, "KXXX", k2020 - 1);
  CHECK(old_ && old_->station.ref() == a_ref && old_->period);
  auto new_ = t->lookup(StationIdType::icao, "KXXX", k2020);  // boundary
  CHECK(new_ && new_->station.ref() == b_ref);
  CHECK(new_ && new_->period && new_->period->elevation_m == 420.0f);
  auto okc_old = t->lookup(StationIdType::icao, "KOKC", k2020 - 3600);
  auto okc_new = t->lookup(StationIdType::icao, "KOKC", k2024);
  CHECK(okc_old && okc_old->period && okc_old->period->elevation_m == 392.0f);
  CHECK(okc_new && okc_new->period && okc_new->period->elevation_m == 395.0f);
  CHECK(okc_new && okc_new->station.ref() == okc_ref);
  // Provisional station: found, but no position.
  auto p = t->lookup(StationIdType::icao, "KZZZ", k2024);
  CHECK(p && p->station.ref() == prov_ref && !p->period);
  CHECK(p && (p->station.flags() & kStationProvisional));
  CHECK(!t->lookup(StationIdType::icao, "KNOPE", k2024));
  // period_at directly.
  CHECK(t->by_ref(okc_ref)->period_at(k2020)->elevation_m == 395.0f);
}

TEST(stations_validation)
{
  TempDir td;
  StationTableBuilder b;
  auto rejects = [&](auto mutate)
  {
    Station s = okc();
    mutate(s);
    return throws<std::invalid_argument>([&] { b.add(s); });
  };
  CHECK(rejects(
    [](Station& s)
    {
      // overlapping periods
      s.periods[0].valid_to_s = k2020 + 1;
    }));
  CHECK(rejects([](Station& s) { std::swap(s.periods[0], s.periods[1]); }));
  CHECK(rejects([](Station& s) { s.periods[0].valid_from_s = k2020; }));
  CHECK(rejects([](Station& s) { s.periods[1].lat_deg = 91; }));
  CHECK(rejects([](Station& s) { s.periods[1].elevation_m = NAN; }));
  CHECK(rejects([](Station& s) { s.periods.clear(); }));  // fixed: needs one
  CHECK(rejects([](Station& s) { s.ids.clear(); }));
  CHECK(rejects([](Station& s) { s.ids[0].id.value = "KOK"; }));
  CHECK(rejects([](Station& s) { s.ids[0].id.value = "kokc"; }));
  CHECK(rejects([](Station& s) { s.ids[1].id.value = "7235"; }));
  CHECK(rejects([](Station& s) { s.ids[1].id.value = "7235A"; }));
  CHECK(
    rejects([](Station& s)
            { s.ids.push_back(bind(StationIdType::platform, "has space")); }));
  CHECK(rejects([](Station& s)
                { s.ids.push_back(bind(StationIdType::platform, "")); }));
  CHECK(rejects(
    [](Station& s)
    {
      s.ids.push_back(
        bind(StationIdType::platform, "0123456789012345678901234567890123"));
    }));
  CHECK(
    rejects([](Station& s) { s.ids[0].valid_from_s = s.ids[0].valid_to_s; }));
  CHECK(rejects([](Station& s) { s.ids.push_back(s.ids[0]); }));  // dup
  CHECK(rejects([](Station& s) { s.name = std::string(300, 'x'); }));
  CHECK(rejects([](Station& s) { s.flags = 0x80; }));  // unknown flag
  CHECK(b.size() == 0);                                // nothing half-added

  // The same identifier on two stations at overlapping times is caught at
  // seal (it spans stations).
  b.add(okc());
  Station other = oun();
  other.ids.push_back(bind(StationIdType::icao, "KOKC", k2024));
  b.add(other);
  CHECK(throws<std::invalid_argument>([&] { b.seal(td.p); }));
  // Fix it and seal; a builder then seals only once.
  other.ref = 2;
  other.ids.pop_back();
  b.update(other);
  b.seal(td.p);
  CHECK(throws<std::logic_error>([&] { b.seal(td.p); }));
  CHECK(throws<std::logic_error>([&] { b.add(okc()); }));
  // Sealing never replaces an existing generation file.
  StationTableBuilder again;
  again.add(okc());
  CHECK(throws<std::runtime_error>([&] { again.seal(td.p); }));
}

TEST(stations_corruption_and_truncation)
{
  TempDir td;
  StationTableBuilder b;
  b.add(okc());
  b.add(oun());
  const fs::path p = b.seal(td.p);
  const auto good = read_file(p);
  const fs::path bad = td.p / "bad.tbl";
  // Every byte of the used part of each page: header (first 256 bytes) and
  // each section's first bytes.
  std::vector<std::size_t> offsets = {0, 8, 20, 100, 255};
  for (std::size_t page = 4096; page < good.size(); page += 4096)
  {
    offsets.push_back(page);
    offsets.push_back(page + 3);
  }
  for (std::size_t off : offsets)
  {
    auto c = good;
    c[off] ^= 0x10;
    write_file(bad, c);
    CHECK(throws<std::runtime_error>([&] { (void)StationTable::open(bad); }));
  }
  for (std::size_t len : {std::size_t(0), std::size_t(4095), good.size() - 1})
  {
    auto c = good;
    c.resize(len);
    write_file(bad, c);
    CHECK(throws<std::runtime_error>([&] { (void)StationTable::open(bad); }));
  }
  CHECK(
    throws<std::runtime_error>(
      [&] { (void)StationTable::open(td.p / "absent.tbl"); }));
  CHECK(StationTable::open(p)->size() == 2);
}

TEST(stations_random_lookup_matches_brute_force)
{
  TempDir td;
  std::mt19937_64 rng(7);
  StationTableBuilder b;
  std::vector<Station> owned;
  // 2000 stations; ICAO codes drawn from a small pool and reassigned over
  // time so lookups have to pick by interval.
  std::uniform_int_distribution<int> letter(0, 5), year(0, 9);
  std::map<std::string, std::int64_t> next_free;  // per code: first free time
  for (int i = 0; i < 2000; ++i)
  {
    Station s;
    s.name = "S" + std::to_string(i);
    std::string code = "K";
    for (int c = 0; c < 3; ++c)
    {
      code.push_back(static_cast<char>('A' + letter(rng)));
    }
    std::int64_t from = next_free.count(code) ? next_free[code] : kTimeMin;
    const std::int64_t to = (from == kTimeMin ? k2020 : from) +
                            static_cast<std::int64_t>(year(rng) + 1) * 31536000;
    next_free[code] = to;
    s.ids = {bind(StationIdType::icao, code.c_str(), from, to)};
    s.periods = {at(30.0 + i * 0.001, -100.0, 300.0f)};
    s.ref = b.add(s);
    owned.push_back(s);
  }
  const auto t = StationTable::open(b.seal(td.p));
  CHECK(t->size() == owned.size());
  bool ok = true;
  std::uniform_int_distribution<std::int64_t> when(
    k2020 - 400000000, k2024 + 400000000);
  for (int q = 0; q < 5000 && ok; ++q)
  {
    const Station& pick = owned[rng() % owned.size()];
    const std::string& code = pick.ids[0].id.value;
    const std::int64_t tq = when(rng);
    std::uint32_t want = 0;
    for (const Station& s : owned)
    {
      const auto& bnd = s.ids[0];
      if (bnd.id.value == code && bnd.valid_from_s <= tq && tq < bnd.valid_to_s)
      {
        want = s.ref;
      }
    }
    const auto got = t->lookup(StationIdType::icao, code, tq);
    ok =
      ok && (want == 0 ? !got.has_value() : got && got->station.ref() == want);
    // Round trip of the owned record.
    ok = ok && t->by_ref(pick.ref)->to_station() == pick;
  }
  CHECK(ok);
}

int main()
{
  return tst::run_all();
}
