// Grid catalog in MySQL (D13, docs/design/07-grib-catalog.md; T16).
//
// The data types here describe one ingested GRIB file; they are filled by
// catalog_grib() (grib.h, eccodes) and need no database.  The Catalog
// class writes them; it lives in the `atmo_catalog` library, built only
// with a MySQL or MariaDB client library (ATMO_WITH_MYSQL, defines
// ATMO_HAVE_MYSQL).  The schema belongs to the web app's Doctrine
// migrations (../web, src/Entity/Grib); the engine checks
// grib_catalog_state.schema_version and refuses any other version.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atmo/grib_api.h"
#include "atmo/gribsig.h"
#include "atmo/grid.h"
#include "atmo/retention.h"

namespace atmo
{

// 2: model aliases, common names, api_name, message process_id, grid
// viewable (Q18/Q19, web migration Version20261111180000).
// 3: grib_message_replaced (Q16b, web migration Version20261111220000).
constexpr int kCatalogSchemaVersion = 3;

// grib_message.flags
constexpr std::uint32_t kCatalogUndecodable = 1;  // template unknown to eccodes
constexpr std::uint32_t kCatalogGridUnsupported = 2;  // grid_id is NULL

// One grib_grid row.  A grid that cannot be sampled (reduced Gaussian,
// spectral, unstructured, or an undecodable message) still gets a row so
// the model -> grid -> run tree has somewhere to hang: its key is the
// GridIdentity of the raw section 3 bytes (ni, nj where known, else 0),
// `viewable` is false (grib_grid.viewable, Q18d) and there is no GridStore
// file.
struct CatalogGrid
{
  GridKey key = 0;
  bool viewable = true;
  std::uint32_t grid_type = 0;  // gridDefinitionTemplateNumber
  std::uint32_t ni = 0;
  std::uint32_t nj = 0;
  bool periodic_i = false;
  double west = 0.0, south = 0.0, east = 0.0, north = 0.0;
  std::string summary;  // JSON object text
  std::string name;     // default tree label ("0.25DEG LL WORLDWIDE")
};

struct CatalogMessage
{
  GribHeader header;
  GridKey grid = 0;  // always one of CatalogFile::grids
  std::uint64_t byte_offset = 0;
  std::uint64_t byte_length = 0;
  std::uint64_t content_hash = 0;  // FNV-1a of the bytes (GribCache key)
  std::uint32_t flags = 0;
};

struct CatalogSkip
{
  std::size_t message = 0;  // 0-based index in the file
  std::string reason;
};

struct CatalogFile
{
  std::string path;  // relative to the store root
  std::uint64_t size = 0;
  std::uint32_t messages_total = 0;  // every message, catalogued or not
  std::vector<CatalogGrid> grids;
  std::vector<CatalogMessage> messages;
  std::vector<CatalogSkip> skipped;  // not GRIB2 (catalogued: everything else)

  // Every message's grid is listed, keys are unique, path is relative.
  // std::invalid_argument otherwise.
  void validate() const;
};

// Default display name of a parameter (Q17b): the code table 4.2 name
// (level-independent: "Temperature", not eccodes' "2 metre temperature"),
// else eccodes' name for centre-local parameters, plus the interval of a
// statistical one ("Total precipitation 1hr", "... run total").  Unknown
// to eccodes: "Unknown <discipline>/<category>/<number>".
std::string catalog_parameter_name(const GribSignature& sig);
// Default units: the table 4.2 units, else eccodes' units written the same
// way ("kg m**-2" -> "kg m-2").
std::string catalog_parameter_units(const GribSignature& sig);

// Default grib_model_grid.api_name (Q19a): "<model label>_<grid name>",
// lower-cased, every character outside [a-z0-9_.-] -> '_', at most 100
// characters ("unnamed" if empty).  The model label is the model's name,
// or its slug while the name is empty.  The writer appends _2, _3, ... if
// another model grid has it already.
std::string catalog_api_name(
  const std::string& model_label,
  const std::string& grid_name);

// Connection settings: a JSON file readable only by the service user
// (refused if group or others may read it), e.g.
//   {"host": "127.0.0.1", "port": 3306, "user": "atmo",
//    "password": "...", "database": "atmotensor"}
// or {"socket": "/run/mysqld/mysqld.sock", ...} instead of host/port.
// user and database are required.
struct CatalogConfig
{
  std::string host;
  unsigned port = 0;
  std::string socket;
  std::string user;
  std::string password;
  std::string database;

  // std::invalid_argument for bad content or permissions.
  static CatalogConfig from_file(const std::filesystem::path& path);
  static CatalogConfig from_text(const std::string& text);
};

struct CatalogIngestStats
{
  std::uint64_t generation = 0;  // the commit's; = ingest_seq of its rows
  std::size_t messages = 0;
  std::size_t new_signatures = 0;
  std::size_t unknown_signatures = 0;  // of the new ones
  std::size_t removed_messages = 0;    // rows of this file not seen again
  std::size_t replaced_messages = 0;   // older copies with other bytes (new
                                       // id; old id -> grib_message_replaced)
};

struct CatalogExpireStats
{
  std::uint64_t generation = 0;  // unchanged when nothing expired
  std::size_t runs = 0;          // grib_run rows deleted
  std::size_t messages = 0;      // their messages (cascade)
  std::size_t files = 0;         // grib_file rows left without messages
  std::size_t unlinked = 0;      // of those, files removed from disk
};

struct CatalogRealiasStats
{
  std::uint64_t generation = 0;  // unchanged when nothing moved
  std::size_t moved_messages = 0;
  std::size_t dropped_duplicates = 0;  // older copies (latest wins)
  std::size_t removed_runs = 0;
  std::size_t removed_model_grids = 0;
  std::size_t removed_models = 0;
};

class Catalog
{
 public:
  // Connects and checks the schema version.  std::runtime_error if the
  // server cannot be reached or the schema is not kCatalogSchemaVersion.
  explicit Catalog(const CatalogConfig& config);
  ~Catalog();
  Catalog(const Catalog&) = delete;
  Catalog& operator=(const Catalog&) = delete;

  // Writes one file in one transaction (07 "Ingest"): bumps the
  // generation first (which serialises concurrent ingests), upserts
  // centre / subcentre / model / grid / model_grid / run / signature rows
  // (new identified signatures get an eccodes grib_parameter), upserts the
  // messages (a duplicate key replaces the older row: identical bytes keep
  // its id, other bytes get a new id and the old one is recorded in
  // grib_message_replaced, Q16b), and
  // deletes this file's rows that the new version no longer has.  Columns
  // the web app owns are only set on insert.  Rolls back and rethrows on
  // any error (std::runtime_error; std::invalid_argument for a bad file).
  CatalogIngestStats ingest(const CatalogFile& file);

  // Applies the current grib_model_alias rows to what is catalogued
  // (Q18a, Q21d), in one transaction, without reading files: every message
  // moves to the model its (centre, subcentre of its run's model, raw
  // process_id) resolves to now (the alias target, else the model of that
  // process, created if missing), keeping its id.  Where the target run
  // already has the same message, the copy with the higher ingest_seq
  // wins.  Moved rows get the new generation as ingest_seq.  Runs and
  // model grids left empty are deleted, and so are emptied models the web
  // app never named (name '') unless an alias points at them.  Nothing to
  // move: rolled back, generation unchanged.  Limitation: an alias across
  // subcentres, once removed, moves messages back under the target's
  // subcentre (messages do not store their own subcentre).
  CatalogRealiasStats realias();

  // Retention for the catalog (D1, T18): per model grid, the newest K
  // distinct base times stay (K = policy.runs_for(api_name); every member
  // of a kept base time stays), older runs are deleted with their messages
  // in one transaction (new generation); grib_file rows left without
  // messages are deleted too, and after the commit their files are
  // unlinked under `root` (and emptied run directories removed).  A query
  // daemon that still holds the previous generation can fail to read such
  // a file for up to its poll interval (Q23).  Nothing to expire: rolled
  // back, generation unchanged.
  CatalogExpireStats expire(
    const RetentionPolicy& policy,
    const std::filesystem::path& root);

  std::uint64_t generation();

  // Low-level access for tests and tools: runs one statement and returns
  // its rows (NULL -> nullopt).  std::runtime_error on SQL errors.
  using Row = std::vector<std::optional<std::string>>;
  std::vector<Row> query(const std::string& sql);
  std::string quote(const std::string& s);  // 'escaped'

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The query daemon's view of the catalog (T17; grib_api.h CatalogQuery):
// one read-only connection (SET SESSION TRANSACTION READ ONLY).  The small
// tables are read in one consistent snapshot whenever the generation
// changes (checked at most once per `poll`); message rows are fetched per
// (run, signature) and by id and kept in LRU caches of `cache_entries`
// each, which are cleared on every generation change.  SQL runs under one
// mutex; a lost connection is re-opened once per call.  atmo_catalog only.
class CatalogReader : public CatalogQuery
{
 public:
  explicit CatalogReader(
    const CatalogConfig& config,
    std::chrono::milliseconds poll = std::chrono::milliseconds(1000),
    std::size_t cache_entries = 4096);
  ~CatalogReader() override;

  std::shared_ptr<const CatalogTables> tables() override;
  std::vector<std::uint64_t> run_signatures(
    const CatalogTables& t,
    std::uint64_t run_id) override;
  std::vector<CatMessage> messages(
    const CatalogTables& t,
    std::uint64_t run_id,
    std::uint64_t signature_id) override;
  std::optional<CatMessage> message(const CatalogTables& t, std::uint64_t id)
    override;
  std::optional<std::uint64_t> replaced_by(
    const CatalogTables& t,
    std::uint64_t id) override;
  std::vector<std::int64_t> run_offsets(
    const CatalogTables& t,
    std::uint64_t run_id) override;
  std::vector<CatMessage> messages_at(
    const CatalogTables& t,
    std::uint64_t run_id,
    std::int64_t offset_end_s) override;

  struct Stats
  {
    std::size_t reloads = 0;
    std::size_t queries = 0;  // per-run / per-message SQL statements
    std::size_t hits = 0;
  };
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atmo
