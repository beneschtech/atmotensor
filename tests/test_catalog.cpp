// Grid catalog (T16, 07-grib-catalog.md).  The database tests need a
// THROWAWAY database with the web app's migrations applied
// (tests/catalog_testdb.sh prints the setting):
//   ATMO_TEST_CATALOG=/path/catalog-test.json ./test_catalog
// The database name must end in "_test"; every grib_* table is emptied.
// Without ATMO_TEST_CATALOG only the database-free tests run and the
// program exits 77 (ctest: skipped) unless one of them failed.
#include <eccodes.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/catalog.h"
#include "atmo/grib.h"
#include "atmo/gridstore.h"
#include "atmo/http_api.h"
#include "atmo/retention.h"
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
        ("atmo_test_catalog_" + std::to_string(::getpid()) + "_" +
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

using Bytes = std::vector<unsigned char>;
using Sets = std::vector<std::pair<const char*, long>>;

void ck(int err, const char* what)
{
  if (err != 0)
  {
    throw std::runtime_error(
      std::string("eccodes test helper: ") + what + ": " +
      codes_get_error_message(err));
  }
}

Bytes make(
  long pdt,
  const Sets& sets,
  const char* sample = "regular_ll_sfc_grib2")
{
  codes_handle* h = codes_grib_handle_new_from_samples(nullptr, sample);
  if (!h)
  {
    throw std::runtime_error("no sample");
  }
  ck(codes_set_long(h, "centre", 7), "centre");  // NCEP local tables
  ck(codes_set_long(h, "productDefinitionTemplateNumber", pdt), "pdt");
  ck(codes_set_long(h, "dataDate", 20261110), "dataDate");
  ck(codes_set_long(h, "dataTime", 0), "dataTime");
  ck(codes_set_long(h, "generatingProcessIdentifier", 96), "process");
  for (const auto& [k, v] : sets)
  {
    ck(codes_set_long(h, k, v), k);
  }
  const void* buf = nullptr;
  std::size_t n = 0;
  ck(codes_get_message(h, &buf, &n), "get_message");
  Bytes out(
    static_cast<const unsigned char*>(buf),
    static_cast<const unsigned char*>(buf) + n);
  codes_handle_delete(h);
  return out;
}

const Sets kT2m = {
  {"parameterCategory", 0},
  {"parameterNumber", 0},
  {"typeOfFirstFixedSurface", 103},
  {"scaleFactorOfFirstFixedSurface", 0},
  {"scaledValueOfFirstFixedSurface", 2},
  {"forecastTime", 6}};

Bytes precip(long start, long len)
{
  return make(
    8, {{"parameterCategory", 1},
        {"parameterNumber", 8},
        {"typeOfFirstFixedSurface", 1},
        {"typeOfStatisticalProcessing", 1},
        {"forecastTime", start},
        {"lengthOfTimeRange", len}});
}

Bytes undecodable()
{
  Bytes m = make(0, kT2m);
  std::size_t i = 16;
  while (i + 5 < m.size())
  {
    const std::size_t len = (std::size_t(m[i]) << 24) |
                            (std::size_t(m[i + 1]) << 16) |
                            (std::size_t(m[i + 2]) << 8) | m[i + 3];
    if (m[i + 4] == 4)
    {
      m[i + 7] = 0xEE;
      m[i + 8] = 0x11;
      break;
    }
    i += len;
  }
  return m;
}

// The test file: 7 messages, see the checks.
struct Fixture
{
  TempDir dir;
  fs::path root;
  fs::path file;
  Fixture()
  {
    root = dir.p / "store";
    fs::create_directories(root);
    file = dir.p / "test.grib2";
    std::vector<Bytes> msgs = {
      make(0, kT2m),  // 0 t2m
      precip(3, 3),   // 1 3 h bucket
      precip(0, 6),   // 2 run total
      make(
        0, {{"parameterCategory", 191},  // 3 unknown
            {"parameterNumber", 250},
            {"typeOfFirstFixedSurface", 1}}),
      make(
        1, {{"parameterCategory", 0},  // 4 member 3
            {"parameterNumber", 0},
            {"typeOfFirstFixedSurface", 103},
            {"scaledValueOfFirstFixedSurface", 2},
            {"typeOfEnsembleForecast", 3},
            {"perturbationNumber", 3},
            {"numberOfForecastsInEnsemble", 20}}),
      make(0, kT2m, "reduced_gg_pl_32_grib2"),  // 5 reduced
      undecodable(),                            // 6
    };
    std::ofstream o(file, std::ios::binary);
    for (const auto& m : msgs)
    {
      o.write(
        reinterpret_cast<const char*>(m.data()),
        static_cast<std::streamsize>(m.size()));
    }
  }
};

// The database, if configured; reset to an empty catalog.
std::unique_ptr<Catalog> open_db()
{
  const char* cfg = std::getenv("ATMO_TEST_CATALOG");
  if (!cfg || !*cfg)
  {
    return nullptr;
  }
  const CatalogConfig c = CatalogConfig::from_file(cfg);
  if (
    c.database.size() < 5 ||
    c.database.compare(c.database.size() - 5, 5, "_test") != 0)
  {
    throw std::runtime_error("ATMO_TEST_CATALOG: database must end in _test");
  }
  {
    auto db = std::make_unique<Catalog>(c);
    db->query("SET FOREIGN_KEY_CHECKS = 0");
    for (const char* t :
         {"grib_message", "grib_file", "grib_run", "grib_model_grid",
          "grib_model_alias", "grib_common_name", "grib_message_replaced",
          "grib_model", "grib_subcenter", "grib_center", "grib_grid",
          "grib_signature", "grib_parameter"})
    {
      db->query(std::string("TRUNCATE TABLE ") + t);
    }
    db->query("UPDATE grib_catalog_state SET generation = 0 WHERE id = 1");
    db->query("SET FOREIGN_KEY_CHECKS = 1");
  }
  return std::make_unique<Catalog>(c);
}

bool g_have_db = false;

std::string one(Catalog& db, const std::string& sql)
{
  const auto r = db.query(sql);
  if (r.size() != 1 || r[0].empty())
  {
    return "<" + std::to_string(r.size()) + " rows>";
  }
  return r[0][0].value_or("NULL");
}

}  // namespace

// ---- without a database ----------------------------------------------------

TEST(config_parsing_and_permissions)
{
  const auto c = CatalogConfig::from_text(
    R"({"host":"db","port":3307,"user":"u","password":"p","database":"d"})");
  CHECK(
    c.host == "db" && c.port == 3307 && c.user == "u" && c.password == "p" &&
    c.database == "d" && c.socket.empty());
  const auto s =
    CatalogConfig::from_text(R"({"socket":"/s","user":"u","database":"d"})");
  CHECK(s.socket == "/s" && s.host.empty() && s.port == 0);
  for (const char* bad :
       {R"({"user":"u"})", R"({"database":"d"})",
        R"({"user":"u","database":"d","colour":1})",
        R"({"user":"u","database":"d","port":0})",
        R"({"user":"u","database":"d","host":"h","socket":"/s"})",
        R"({"user":"","database":"d"})", "[]", "{"})
  {
    CHECK(
      throws<std::invalid_argument>([&] { CatalogConfig::from_text(bad); }));
  }
  TempDir d;
  const fs::path f = d.p / "c.json";
  std::ofstream(f) << R"({"user":"u","database":"d"})";
  ::chmod(f.c_str(), 0644);
  CHECK(throws<std::invalid_argument>([&] { CatalogConfig::from_file(f); }));
  ::chmod(f.c_str(), 0600);
  CHECK(CatalogConfig::from_file(f).database == "d");
}

TEST(parameter_names)
{
  GribSignature s;
  s.ecc_short_name = "tp";
  s.ecc_name = "Total precipitation";
  CHECK(catalog_parameter_name(s) == "Total precipitation");
  s.stat_interval_s = 3600;
  CHECK(catalog_parameter_name(s) == "Total precipitation 1hr");
  s.stat_interval_s = 0;
  CHECK(catalog_parameter_name(s) == "Total precipitation run total");
  s.stat_interval_s = 900;
  CHECK(catalog_parameter_name(s) == "Total precipitation 15min");
  s.stat_interval_s = 2 * 86400;
  CHECK(catalog_parameter_name(s) == "Total precipitation 2day");
  GribSignature u;
  u.ecc_short_name = "unknown";
  u.discipline = 0;
  u.category = 191;
  u.number = 250;
  CHECK(catalog_parameter_name(u) == "Unknown 0/191/250");
  u.table_name = "Ice growth rate";  // eccodes has no paramId: still unknown
  CHECK(catalog_parameter_name(u) == "Unknown 0/191/250");
}

TEST(parameter_names_prefer_the_level_independent_table)
{
  GribSignature s;
  s.ecc_short_name = "2t";
  s.ecc_name = "2 metre temperature";
  s.ecc_units = "K";
  s.table_name = "Temperature";
  s.table_units = "K";
  CHECK(catalog_parameter_name(s) == "Temperature");
  CHECK(catalog_parameter_units(s) == "K");
  // Centre-local parameter: eccodes' own name and units, units spelled
  // like the table's ("kg m-2", not "kg m**-2").
  GribSignature l;
  l.ecc_short_name = "cnwat";
  l.ecc_name = "Plant canopy surface water";
  l.ecc_units = "kg m**-2";
  l.stat_interval_s = 0;
  CHECK(catalog_parameter_name(l) == "Plant canopy surface water run total");
  CHECK(catalog_parameter_units(l) == "kg m-2");
  // Statistics other than accumulation say which (code table 4.10), so a
  // maximum and a minimum never share a default name.
  GribSignature w;
  w.ecc_name = "Time-maximum 10 metre wind speed";
  w.table_name = "Wind speed";
  w.stat_interval_s = 3600;
  w.stat_process = 2;
  CHECK(catalog_parameter_name(w) == "Wind speed maximum 1hr");
  w.stat_process = 3;
  w.stat_interval_s = 0;
  CHECK(catalog_parameter_name(w) == "Wind speed minimum since run start");
  w.stat_process = 1;
  CHECK(catalog_parameter_name(w) == "Wind speed run total");
  w.stat_process = 0;
  w.stat_interval_s = 6 * 3600;
  CHECK(catalog_parameter_name(w) == "Wind speed average 6hr");
  w.stat_process = 200;  // local
  CHECK(catalog_parameter_name(w) == "Wind speed statistic 200 6hr");
  w.table_name.clear();  // eccodes' name already says "maximum"
  w.stat_process = 2;
  CHECK(catalog_parameter_name(w) == "Time-maximum 10 metre wind speed 6hr");
}

TEST(default_api_names)
{
  CHECK(
    catalog_api_name("7-0-96", "0.25DEG LL WORLDWIDE") ==
    "7-0-96_0.25deg_ll_worldwide");
  CHECK(
    catalog_api_name("RRFS North America", "LAMBERT 3KM 1799x1059") ==
    "rrfs_north_america_lambert_3km_1799x1059");
  CHECK(catalog_api_name("A/B (x)", "") == "a_b__x_");
  CHECK(catalog_api_name("", "") == "unnamed");
  CHECK(catalog_api_name(std::string(200, 'm'), "g").size() == 100);
}

TEST(catalog_grib_describes_every_message)
{
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  CHECK(f.messages_total == 7);
  CHECK(f.messages.size() == 7);
  CHECK(f.skipped.empty());
  CHECK(f.path == "grib/test/2026111000/test.grib2");
  CHECK(fs::exists(fx.root / f.path));
  CHECK(f.size == fs::file_size(fx.file));
  if (f.messages.size() != 7)
  {
    return;
  }
  // One sampleable grid + two placeholders (reduced, undecodable).
  CHECK(f.grids.size() == 3);
  std::size_t supported = 0;
  for (const auto& g : f.grids)
  {
    supported += g.viewable;
    CHECK(
      fs::exists(fx.root / "grids" / (key_hex(g.key) + ".grid")) == g.viewable);
  }
  CHECK(supported == 1);
  const auto& t2m = f.messages[0];
  CHECK(t2m.flags == 0 && t2m.header.level1.type == 103);
  CHECK(t2m.header.offset_end_s == 6 * 3600);
  CHECK(t2m.byte_offset == 0);
  CHECK(f.messages[1].header.sig.stat_interval_s == 10800u);
  CHECK(f.messages[2].header.sig.stat_interval_s == 0u);
  CHECK(!f.messages[3].header.sig.identified());
  CHECK(f.messages[4].header.member == 3);
  CHECK(f.messages[5].flags == kCatalogGridUnsupported);
  CHECK(f.messages[6].flags == kCatalogUndecodable);
  CHECK(f.messages[0].grid == f.messages[4].grid);
  CHECK(f.messages[5].grid != f.messages[0].grid);
  for (const auto& g : f.grids)
  {
    if (g.viewable)
    {
      CHECK(g.name == "1DEG LL 16x31" || !g.name.empty());
      CHECK(g.south <= g.north);
    }
  }
  // Content hash = the bytes on disk.
  std::ifstream in(fx.file, std::ios::binary);
  Bytes all((std::istreambuf_iterator<char>(in)), {});
  CHECK(
    fnv1a64(all.data() + t2m.byte_offset, t2m.byte_length) == t2m.content_hash);
  // Again: the identical file is reused, a different one refused.
  CHECK(catalog_grib(fx.root, fx.file, "test").messages.size() == 7);
  // A different file under the same name (new inode) is refused: one byte
  // of the undecodable message changed (parsed raw, so still readable).
  Bytes changed = all;
  const auto& last = f.messages[6];
  changed[last.byte_offset + last.byte_length - 5] ^= 0x01;
  fs::remove(fx.file);
  std::ofstream(fx.file, std::ios::binary)
    .write(
      reinterpret_cast<const char*>(changed.data()),
      static_cast<std::streamsize>(changed.size()));
  bool refused = false;
  try
  {
    catalog_grib(fx.root, fx.file, "test");
  }
  catch (const std::runtime_error& e)
  {
    refused =
      std::string(e.what()).find("different content") != std::string::npos;
  }
  CHECK(refused);
}

TEST(catalog_file_validation)
{
  CatalogFile f;
  f.path = "grib/x/f";
  f.messages_total = 1;
  CatalogGrid g;
  g.key = 1;
  f.grids = {g};
  CatalogMessage m;
  m.grid = 1;
  m.header.sig.canonical = "v1\n";
  f.messages = {m};
  f.validate();
  auto bad = f;
  bad.messages[0].grid = 2;
  CHECK(throws<std::invalid_argument>([&] { bad.validate(); }));
  bad = f;
  bad.grids.push_back(g);
  CHECK(throws<std::invalid_argument>([&] { bad.validate(); }));
  bad = f;
  bad.path = "/abs";
  CHECK(throws<std::invalid_argument>([&] { bad.validate(); }));
  bad = f;
  bad.path = "grib/../x";
  CHECK(throws<std::invalid_argument>([&] { bad.validate(); }));
  bad = f;
  bad.messages_total = 0;
  CHECK(throws<std::invalid_argument>([&] { bad.validate(); }));
}

// ---- with a database -------------------------------------------------------

TEST(db_ingest_writes_the_hierarchy)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  g_have_db = true;
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  const auto st = db->ingest(f);
  CHECK(st.generation == 1);
  CHECK(db->generation() == 1);
  CHECK(st.messages == 7);
  CHECK(st.removed_messages == 0);
  // t2m (4.0 and 4.1 differ), 3 h, run, unknown, reduced-grid t2m shares
  // the 4.0 t2m signature, undecodable.
  CHECK(st.new_signatures == 6);
  CHECK(st.unknown_signatures == 2);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "7");
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE ingest_seq = 1") == "7");
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE grid_id IS NULL") == "2");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message WHERE flags = 1") == "1");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message WHERE flags = 2") == "1");
  CHECK(one(*db, "SELECT code FROM grib_center") == "7");
  CHECK(one(*db, "SELECT slug FROM grib_model") == "7-0-96");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_grid") == "3");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_grid WHERE viewable = 0 AND "
      "JSON_EXTRACT(summary, '$.supported') IS NULL") == "2");
  // Schema 2: raw process id on the message, default api_name.
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE process_id = 96") == "7");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_model_grid WHERE api_name LIKE "
      "'7-0-96\\_%'") == "3");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_model_grid") == "3");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_run") == "4");  // 3 grids + member
  CHECK(one(*db, "SELECT member_type FROM grib_run WHERE `member` = 3") == "3");
  CHECK(
    one(*db, "SELECT basetime FROM grib_run WHERE `member` = 3") ==
    "2026-11-10 00:00:00");
  CHECK(
    one(*db, "SELECT path FROM grib_file") ==
    "grib/test/2026111000/test.grib2");
  CHECK(one(*db, "SELECT messages_total FROM grib_file") == "7");
  // Parameters, with the interval in the name.
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_parameter WHERE name = "
      "'Total precipitation 3hr'") == "1");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_parameter WHERE name = "
      "'Total precipitation run total'") == "1");
  // Names from code table 4.2, not eccodes' level-dependent "2 metre
  // temperature" (Q17b); the deterministic and member signatures share it.
  CHECK(
    one(
      *db,
      "SELECT CONCAT(COUNT(*), '|', MIN(BINARY name), '|', MIN(units), '|', "
      "MIN(short_name)) FROM grib_parameter WHERE name LIKE '%temperature%'") ==
    "1|Temperature|K|");
  CHECK(
    one(
      *db,
      "SELECT COUNT(DISTINCT parameter_id) FROM grib_signature WHERE "
      "ecc_name LIKE '%temperature%'") == "1");
  CHECK(
    one(
      *db,
      "SELECT COUNT(DISTINCT base_hash) FROM grib_signature WHERE "
      "stat_interval_s IS NOT NULL") == "1");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_signature WHERE status = "
      "'unknown' AND parameter_id IS NULL") == "2");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_signature WHERE status = "
      "'identified' AND parameter_id IS NOT NULL") == "4");
  // Message details.
  CHECK(
    one(
      *db,
      "SELECT CONCAT(offset_start_s, '-', offset_end_s, ' ', "
      "valid_time) FROM grib_message m JOIN grib_signature s ON "
      "s.id = m.signature_id WHERE s.stat_interval_s = 10800") ==
    "10800-21600 2026-11-10 06:00:00");
  CHECK(
    one(
      *db,
      "SELECT CONCAT(level1_type, ':', level1_value, ' ', "
      "level2_type) FROM grib_message WHERE byte_offset = 0") == "103:2 255");
  CHECK(
    one(
      *db,
      "SELECT content_hash FROM grib_message WHERE byte_offset = "
      "0") == key_hex(f.messages[0].content_hash));
}

TEST(db_reingest_keeps_ids_and_web_columns)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  db->ingest(f);
  const std::string ids =
    one(*db, "SELECT GROUP_CONCAT(id ORDER BY id) FROM grib_message");
  db->query("UPDATE grib_model SET name = 'My model', slug = 'mine'");
  db->query("UPDATE grib_model_grid SET name = 'My grid'");
  db->query(
    "UPDATE grib_signature SET status = 'overridden' WHERE status = "
    "'unknown'");
  const auto st = db->ingest(f);
  CHECK(st.generation == 2);
  CHECK(st.new_signatures == 0);
  CHECK(st.removed_messages == 0);
  CHECK(
    one(*db, "SELECT GROUP_CONCAT(id ORDER BY id) FROM grib_message") == ids);
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE ingest_seq = 2") == "7");
  CHECK(one(*db, "SELECT name FROM grib_model") == "My model");
  CHECK(one(*db, "SELECT slug FROM grib_model") == "mine");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_model_grid WHERE name = 'My "
      "grid'") == "3");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_signature WHERE status = "
      "'overridden'") == "2");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_run") == "4");

  // A new version of the file without the last two messages: their rows go.
  CatalogFile shorter = f;
  shorter.messages.resize(5);
  const auto st2 = db->ingest(shorter);
  CHECK(st2.removed_messages == 2);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "5");

  // Latest wins: another file with the same message takes the row over.
  // Identical bytes (same content hash): the id stays.
  const std::string id0 =
    one(*db, "SELECT id FROM grib_message WHERE byte_offset = 0");
  CatalogFile other = f;
  other.path = "grib/test/2026111000/other.grib2";
  other.messages = {f.messages[0]};
  other.messages[0].byte_offset = 4096;
  const auto st3 = db->ingest(other);
  CHECK(st3.messages == 1);
  CHECK(st3.replaced_messages == 0);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "5");
  CHECK(
    one(
      *db,
      "SELECT f.path FROM grib_message m JOIN grib_file f ON "
      "f.id = m.file_id WHERE m.byte_offset = 4096") == other.path);
  CHECK(
    one(*db, "SELECT id FROM grib_message WHERE byte_offset = 4096") == id0);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_file") == "2");

  // A changed copy gets a new id; the old one is recorded as replaced
  // (Q16b), and a further copy keeps the chain flat.
  CatalogFile changed = other;
  changed.path = "grib/test/2026111000/changed.grib2";
  changed.messages[0].content_hash ^= 1;
  const auto st4 = db->ingest(changed);
  CHECK(st4.replaced_messages == 1);
  const std::string id1 = one(
    *db, "SELECT id FROM grib_message WHERE content_hash = " +
           db->quote(key_hex(changed.messages[0].content_hash)));
  CHECK(id1 != id0 && id1 != "<0 rows>");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message WHERE id = " + id0) == "0");
  CHECK(
    one(
      *db, "SELECT new_id FROM grib_message_replaced WHERE old_id = " + id0) ==
    id1);
  CatalogFile again = changed;
  again.path = "grib/test/2026111000/again.grib2";
  again.messages[0].content_hash ^= 2;
  db->ingest(again);
  const std::string id2 = one(
    *db, "SELECT id FROM grib_message WHERE content_hash = " +
           db->quote(key_hex(again.messages[0].content_hash)));
  CHECK(
    one(
      *db,
      "SELECT GROUP_CONCAT(new_id) FROM grib_message_replaced WHERE old_id "
      "IN (" +
        id0 + "," + id1 + ")") == id2 + "," + id2);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "5");
}

TEST(db_model_alias_and_api_name_clash)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  // The web app made a model "gfs" (centre 7, process 81 here, as GFS
  // f000) and aliased the fixture's process 96 to it; another model grid
  // already holds the fixture's default api_name.
  db->query("INSERT INTO grib_center (code, name) VALUES (7, 'NCEP')");
  db->query(
    "INSERT INTO grib_subcenter (center_id, code, name) SELECT id, 0, '' "
    "FROM grib_center");
  db->query(
    "INSERT INTO grib_model (subcenter_id, process_id, slug, name) SELECT "
    "id, 81, 'gfs', 'GFS' FROM grib_subcenter");
  db->query(
    "INSERT INTO grib_model_alias (center_code, subcenter_code, process_id, "
    "model_id) SELECT 7, 0, 96, id FROM grib_model");
  const CatalogGrid* ll = nullptr;
  for (const auto& g : f.grids)
  {
    if (g.viewable)
    {
      ll = &g;
    }
  }
  CHECK(ll != nullptr);
  if (!ll)
  {
    return;
  }
  const std::string taken = catalog_api_name("GFS", ll->name);
  db->query(
    "INSERT INTO grib_grid (grid_key, grid_type, ni, nj, periodic_i, west, "
    "south, east, north, summary) VALUES ('0000000000000001', 0, 1, 1, 0, 0, "
    "0, 0, 0, '{}')");
  db->query(
    "INSERT INTO grib_model_grid (model_id, grid_id, name, api_name) SELECT "
    "m.id, g.id, 'other', " +
    db->quote(taken) + " FROM grib_model m, grib_grid g");
  db->ingest(f);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_model") == "1");  // no 7-0-96
  CHECK(
    one(
      *db,
      "SELECT COUNT(DISTINCT mg.model_id) FROM grib_message msg JOIN "
      "grib_run r ON r.id = msg.run_id JOIN grib_model_grid mg ON mg.id = "
      "r.model_grid_id") == "1");
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE process_id = 96") == "7");
  CHECK(
    one(
      *db,
      "SELECT api_name FROM grib_model_grid mg JOIN grib_grid g ON g.id = "
      "mg.grid_id WHERE g.grid_key = " +
        db->quote(key_hex(ll->key))) == taken + "_2");
  // Re-ingest: same rows, the api_name is not touched again.
  db->query(
    "UPDATE grib_model_grid SET api_name = 'gfs_test' WHERE api_name = " +
    db->quote(taken + "_2"));
  db->ingest(f);
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_model_grid WHERE api_name = 'gfs_test'") ==
    "1");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_model_grid") == "4");
}

TEST(db_realias_moves_runs_both_ways)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  db->ingest(f);  // generation 1, model 7-0-96
  const std::string ids =
    one(*db, "SELECT GROUP_CONCAT(id ORDER BY id) FROM grib_message");
  const std::string model_of_messages =
    "SELECT GROUP_CONCAT(DISTINCT m.slug) FROM grib_message msg JOIN "
    "grib_run r ON r.id = msg.run_id JOIN grib_model_grid mg ON mg.id = "
    "r.model_grid_id JOIN grib_model m ON m.id = mg.model_id";
  db->query(
    "INSERT INTO grib_model (subcenter_id, process_id, slug, name) SELECT "
    "id, 81, 'gfs', 'GFS' FROM grib_subcenter");
  const auto set_alias = [&]
  {
    db->query(
      "INSERT INTO grib_model_alias (center_code, subcenter_code, "
      "process_id, model_id) SELECT 7, 0, 96, id FROM grib_model WHERE slug "
      "= 'gfs'");
  };

  // Alias added: everything moves under "gfs", ids kept; the emptied
  // engine-named model 7-0-96 is removed.
  set_alias();
  auto st = db->realias();
  CHECK(st.generation == 2 && db->generation() == 2);
  CHECK(st.moved_messages == 7);
  CHECK(st.dropped_duplicates == 0);
  CHECK(one(*db, model_of_messages) == "gfs");
  CHECK(
    one(*db, "SELECT GROUP_CONCAT(id ORDER BY id) FROM grib_message") == ids);
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE ingest_seq = 2") == "7");
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE process_id = 96") == "7");
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_model WHERE slug = '7-0-96'") == "0");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_model_grid") == "3");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_model_grid WHERE api_name LIKE "
      "'gfs\\_%'") == "3");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_run") == "4");

  // Nothing to do: no new generation.
  st = db->realias();
  CHECK(st.moved_messages == 0 && db->generation() == 2);

  // Alias removed: back to a (new) 7-0-96; the web-named "gfs" stays.
  db->query("DELETE FROM grib_model_alias");
  st = db->realias();
  CHECK(st.moved_messages == 7);
  CHECK(one(*db, model_of_messages) == "7-0-96");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_model WHERE slug = 'gfs'") == "1");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_model_grid") == "3");

  // Both sides hold the same messages (a second file with the same
  // content, ingested while the alias was gone): the newer copy wins.
  set_alias();
  db->realias();
  db->query("DELETE FROM grib_model_alias");
  const fs::path copy = fx.dir.p / "copy.grib2";
  fs::copy_file(fx.file, copy);
  const auto newer =
    db->ingest(catalog_grib(fx.root, copy, "test"));  // lands in 7-0-96
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "14");
  set_alias();
  st = db->realias();
  CHECK(st.moved_messages == 7 && st.dropped_duplicates == 7);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "7");
  // The dropped older copies answer with their replacements (Q16b).
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_message_replaced r JOIN grib_message m ON "
      "m.id = r.new_id WHERE r.old_id IN (" +
        ids + ")") == "7");
  CHECK(one(*db, model_of_messages) == "gfs");
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_message WHERE id IN (" + ids + ")") ==
    "0");
  CHECK(newer.generation + 1 == st.generation);
}

TEST(db_expire_keeps_the_newest_runs_then_removes_files)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  db->ingest(f);  // 4 runs at 2026-11-10 00Z (3 grids + member 3)
  // Copies of every run 1 and 2 days earlier, each day in its own file.
  for (const auto& [day, name] :
       {std::pair{1, std::string("2026110900")},
        std::pair{2, std::string("2026110800")}})
  {
    const std::string rel = "grib/test/" + name + "/old.grib2";
    fs::create_directories(fx.root / ("grib/test/" + name));
    std::ofstream(fx.root / rel) << "GRIB";
    db->query(
      "INSERT INTO grib_file (path, size, messages_total, ingested_at) "
      "VALUES (" +
      db->quote(rel) + ", 4, 7, UTC_TIMESTAMP())");
    const std::string fid = one(*db, "SELECT LAST_INSERT_ID()");
    for (const auto& r : db->query(
           "SELECT id, model_grid_id, `member`, member_type FROM grib_run "
           "WHERE basetime = '2026-11-10 00:00:00'"))
    {
      db->query(
        "INSERT INTO grib_run (model_grid_id, basetime, `member`, "
        "member_type) VALUES (" +
        *r[1] + ", '2026-11-10 00:00:00' - INTERVAL " + std::to_string(day) +
        " DAY, " + *r[2] + ", " + r[3].value_or("NULL") + ")");
      const std::string nr = one(*db, "SELECT LAST_INSERT_ID()");
      db->query(
        "INSERT INTO grib_message (run_id, signature_id, grid_id, file_id, "
        "level1_type, level1_value, level2_type, level2_value, "
        "offset_start_s, offset_end_s, valid_time, byte_offset, byte_length, "
        "content_hash, flags, ingest_seq, process_id) SELECT " +
        nr + ", signature_id, grid_id, " + fid +
        ", level1_type, level1_value, level2_type, level2_value, "
        "offset_start_s, offset_end_s, valid_time, byte_offset, byte_length, "
        "content_hash, flags, ingest_seq, process_id FROM grib_message "
        "WHERE run_id = " +
        *r[0]);
    }
  }
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_run") == "12");
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_message") == "21");
  const std::uint64_t gen = db->generation();

  // A replaced id whose replacement is in an expiring run, and one whose
  // replacement stays.
  db->query(
    "INSERT INTO grib_message_replaced (old_id, new_id, replaced_at) SELECT "
    "999001, m.id, UTC_TIMESTAMP() FROM grib_message m JOIN grib_run r ON "
    "r.id = m.run_id WHERE r.basetime < '2026-11-09' LIMIT 1");
  db->query(
    "INSERT INTO grib_message_replaced (old_id, new_id, replaced_at) SELECT "
    "999002, m.id, UTC_TIMESTAMP() FROM grib_message m JOIN grib_run r ON "
    "r.id = m.run_id WHERE r.basetime = '2026-11-10' LIMIT 1");
  RetentionPolicy keep2;
  keep2.runs_per_model = 2;
  auto st = db->expire(keep2, fx.root);
  CHECK(
    one(*db, "SELECT GROUP_CONCAT(old_id) FROM grib_message_replaced") ==
    "999002");
  CHECK(st.generation == gen + 1 && db->generation() == gen + 1);
  CHECK(st.runs == 4 && st.messages == 7);
  CHECK(st.files == 1 && st.unlinked == 1);
  CHECK(!fs::exists(fx.root / "grib/test/2026110800"));  // dir removed too
  CHECK(fs::exists(fx.root / "grib/test/2026110900/old.grib2"));
  CHECK(fs::exists(fx.root / f.path));
  CHECK(
    one(*db, "SELECT COUNT(*) FROM grib_run WHERE basetime < '2026-11-09'") ==
    "0");

  // Nothing more to do: no new generation.
  st = db->expire(keep2, fx.root);
  CHECK(st.runs == 0 && db->generation() == gen + 1);

  // A per-model K (by api_name): only that model grid loses its older run;
  // the file still holds the others' messages, so it stays.
  const std::string api = one(
    *db,
    "SELECT mg.api_name FROM grib_model_grid mg JOIN grib_grid g ON g.id = "
    "mg.grid_id WHERE g.viewable = 1");
  keep2.runs_by_model[api] = 1;
  st = db->expire(keep2, fx.root);
  CHECK(st.runs == 2 && st.files == 0 && st.unlinked == 0);  // + member run
  CHECK(fs::exists(fx.root / "grib/test/2026110900/old.grib2"));
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_run") == "6");
}

TEST(db_failed_ingest_rolls_back)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  db->ingest(f);
  // Same hash, different canonical text: a collision must not alias.
  CatalogFile bad = f;
  bad.path = "grib/test/2026111000/bad.grib2";
  bad.messages[0].header.sig.canonical += "x=1\n";
  bad.messages[0].byte_offset = 999;
  CHECK(throws<std::runtime_error>([&] { db->ingest(bad); }));
  CHECK(db->generation() == 1);
  CHECK(one(*db, "SELECT COUNT(*) FROM grib_file") == "1");
  CHECK(
    one(
      *db,
      "SELECT COUNT(*) FROM grib_message WHERE byte_offset = "
      "999") == "0");
  // The connection is usable afterwards.
  CHECK(db->ingest(f).generation == 2);
}

TEST(db_schema_version_is_checked)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  const CatalogConfig c =
    CatalogConfig::from_file(std::getenv("ATMO_TEST_CATALOG"));
  const std::string known = std::to_string(kCatalogSchemaVersion);
  const std::string other = std::to_string(kCatalogSchemaVersion + 1);
  db->query("UPDATE grib_catalog_state SET schema_version = " + other);
  CHECK(throws<std::runtime_error>([&] { Catalog x(c); }));
  db->query("UPDATE grib_catalog_state SET schema_version = " + known);
  CHECK(!throws<std::runtime_error>([&] { Catalog x(c); }));
}

TEST(db_reader_tables_caches_and_reload)
{
  auto db = open_db();
  if (!db)
  {
    return;
  }
  const CatalogConfig c =
    CatalogConfig::from_file(std::getenv("ATMO_TEST_CATALOG"));
  Fixture fx;
  const CatalogFile f = catalog_grib(fx.root, fx.file, "test");
  db->ingest(f);
  CatalogReader reader(c, std::chrono::milliseconds(0), 16);
  auto t = reader.tables();
  CHECK(t->generation == 1);
  CHECK(t->centers.size() == 1 && t->centers[0].code == 7);
  CHECK(t->centers[0].subcenters.size() == 1);
  CHECK(t->models.size() == 1 && t->models[0].slug == "7-0-96");
  CHECK(t->models[0].center_code == 7 && t->models[0].process_id == 96);
  CHECK(t->grids.size() == 3);
  std::size_t supported = 0;
  for (const auto& g : t->grids)
  {
    supported += g.viewable;
  }
  CHECK(supported == 1);
  CHECK(t->runs.size() == 4);
  CHECK(t->runs[0].basetime_s == f.messages[0].header.base_time_s);
  CHECK(t->signatures.size() == 6);
  const CatSignature* tp3 = t->signature_by_hash(f.messages[1].header.sig.hash);
  CHECK(
    tp3 && tp3->name == "Total precipitation 3hr" &&
    tp3->stat_interval_s == 10800u && tp3->units == "kg m-2");
  const CatSignature* unk = t->signature_by_hash(f.messages[3].header.sig.hash);
  CHECK(unk && unk->status == "unknown" && unk->name == "Unknown 0/191/250");

  // Messages per (run, signature) and by id; the second call is cached.
  const CatRun* run = nullptr;
  for (const auto& r : t->runs)
  {
    if (
      r.member == -1 &&
      t->grid(t->model_grid(r.model_grid_id)->grid_id)->viewable)
    {
      run = &r;
    }
  }
  CHECK(run != nullptr);
  if (!run || !tp3)
  {
    return;
  }
  const auto sigs = reader.run_signatures(*t, run->id);
  CHECK(sigs.size() == 4);  // t2m, 3 h, run total, unknown
  const auto ms = reader.messages(*t, run->id, tp3->id);
  CHECK(ms.size() == 1);
  CHECK(ms[0].offset_start_s == 10800 && ms[0].offset_end_s == 21600);
  CHECK(ms[0].level1.type == 1 && ms[0].level2.type == 255);
  CHECK(ms[0].file_path == f.path);
  CHECK(ms[0].content_hash == f.messages[1].content_hash);
  CHECK(ms[0].grid_id.has_value());
  const auto before = reader.stats();
  CHECK(reader.messages(*t, run->id, tp3->id).size() == 1);
  CHECK(reader.stats().hits == before.hits + 1);
  // T17c: offsets of a run, messages at one offset, parameter links.
  const auto offs = reader.run_offsets(*t, run->id);
  CHECK(offs.size() == 2 && offs.front() == 0 && offs.back() == 21600);
  const auto at6 = reader.messages_at(*t, run->id, 21600);
  CHECK(at6.size() == 3);  // t2m f06, the 3 h bucket and the run total
  CHECK(tp3->parameter_id.has_value());
  const auto one_msg = reader.message(*t, ms[0].id);
  CHECK(one_msg && one_msg->byte_offset == ms[0].byte_offset);
  CHECK(!reader.message(*t, 999999));
  CHECK(!reader.replaced_by(*t, 999999));

  // A web edit + a new ingest: new generation, reloaded names.
  db->query(
    "UPDATE grib_parameter SET name = 'Precip (3 h)' WHERE name = "
    "'Total precipitation 3hr'");
  db->ingest(f);
  auto t2 = reader.tables();
  CHECK(t2->generation == 2);
  CHECK(reader.stats().reloads == before.reloads + 1);
  CHECK(
    t2->signature_by_hash(f.messages[1].header.sig.hash)->name ==
    "Precip (3 h)");

  // Common names are part of the tables.
  db->query(
    "INSERT INTO grib_common_name (name, parameter_id) SELECT "
    "'precip_3h', parameter_id FROM grib_signature WHERE stat_interval_s = "
    "10800");
  db->ingest(f);  // a new generation makes the reader reload
  const auto t3 = reader.tables();
  CHECK(
    t3->common_names.size() == 1 && t3->common_names[0].name == "precip_3h");

  // Through the HTTP layer.
  GribApi api(fx.root, reader, nullptr);
  fcgi::Request req;
  const auto resp =
    api.handle("/v1/grib/parameters", {{"run", std::to_string(run->id)}}, req);
  CHECK(resp.status == 200);
  const auto j = nlohmann::json::parse(resp.body);
  CHECK(j["generation"] == 3);
  CHECK(j["parameters"].size() == 4);

  // A replaced id: 410 with the new one (Q16b).
  db->query(
    "INSERT INTO grib_message_replaced (old_id, new_id, replaced_at) "
    "VALUES (999003, " +
    std::to_string(ms[0].id) + ", UTC_TIMESTAMP())");
  CHECK(reader.replaced_by(*t3, 999003) == ms[0].id);
  const auto gone = api.handle("/v1/grib/messages/999003/raw", {}, req);
  CHECK(gone.status == 410);
  const auto gj = nlohmann::json::parse(gone.body);
  CHECK(
    gj["error"]["code"] == "replaced" &&
    gj["error"]["replaced_by"] == ms[0].id);
  CHECK(api.handle("/v1/grib/messages/999004", {}, req).status == 404);
}

int main()
{
  const int rc = tst::run_all();
  if (rc == 0 && !g_have_db)
  {
    std::puts("catalog database tests skipped (ATMO_TEST_CATALOG not set)");
    return 77;
  }
  return rc;
}
