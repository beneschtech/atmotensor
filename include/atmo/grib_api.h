// Grid catalog on the query side (T17, D13; docs/api/query-api-v1.md
// "Grid catalog").  The engine core does not link MySQL: GribApi answers
// /v1/grib/* from the CatalogQuery interface, which the atmo_catalog
// library implements on MySQL (CatalogReader, catalog.h) and tests fake in
// memory.
//
// CatalogQuery contract: tables() returns the small tables as of one
// generation and reloads them when the catalog's generation changes
// (checked at most once per `poll`); the per-run lookups take the tables
// they belong to, so a request sees one consistent generation.  Message
// rows are cached per generation (07: no SQL per tile).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atmo/fcgi.h"
#include "atmo/gribsig.h"
#include "atmo/gribstore.h"
#include "atmo/grid.h"
#include "atmo/query.h"

namespace atmo
{

struct CatSubcenter
{
  std::uint64_t id = 0;
  std::uint32_t code = 0;
  std::string name;
};

struct CatCenter
{
  std::uint64_t id = 0;
  std::uint32_t code = 0;
  std::string name;
  std::vector<CatSubcenter> subcenters;  // by code
};

struct CatModel
{
  std::uint64_t id = 0;
  std::uint64_t center_id = 0;
  std::uint64_t subcenter_id = 0;
  std::uint32_t center_code = 0;
  std::uint32_t subcenter_code = 0;
  std::uint32_t process_id = 0;
  std::string slug;
  std::string name;
};

struct CatGrid
{
  std::uint64_t id = 0;
  GridKey key = 0;
  std::uint32_t type = 0;
  std::uint32_t ni = 0;
  std::uint32_t nj = 0;
  bool periodic_i = false;
  bool viewable = true;  // false: point/tiles answer 409 (Q18d)
  std::string summary;   // JSON object text (grib_grid.summary)
  double west = 0.0, south = 0.0, east = 0.0, north = 0.0;
};

struct CatModelGrid
{
  std::uint64_t id = 0;
  std::uint64_t model_id = 0;
  std::uint64_t grid_id = 0;
  std::string name;
  std::string api_name;  // model name in /v1/point, /v1/tiles (Q19a)
};

struct CatRun
{
  std::uint64_t id = 0;
  std::uint64_t model_grid_id = 0;
  std::int64_t basetime_s = 0;
  int member = -1;
  std::optional<int> member_type;
};

struct CatSignature
{
  std::uint64_t id = 0;
  std::uint64_t hash = 0;
  std::uint64_t base_hash = 0;
  std::optional<std::uint32_t> stat_interval_s;
  std::uint32_t discipline = 0, category = 0, number = 0, pdt = 0;
  std::string canonical;
  std::string status;  // identified | unknown | overridden
  std::optional<std::uint64_t> parameter_id;  // grib_parameter, if linked
  // From grib_parameter if linked, else generic / eccodes values.
  std::string name;
  std::string short_name;
  std::string units;
  double si_scale = 1.0;
  double si_offset = 0.0;
};

struct CatMessage
{
  std::uint64_t id = 0;
  std::uint64_t run_id = 0;
  std::uint64_t signature_id = 0;
  std::optional<std::uint64_t> grid_id;  // NULL: cannot be sampled
  std::string file_path;                 // relative to the store root
  GribLevel level1;
  GribLevel level2;
  std::int64_t offset_start_s = 0;
  std::int64_t offset_end_s = 0;
  std::int64_t valid_time_s = 0;
  std::uint64_t byte_offset = 0;
  std::uint64_t byte_length = 0;
  std::uint64_t content_hash = 0;
  std::uint32_t flags = 0;
  std::uint32_t process_id = 0;  // as in the file (the model may be an alias)
};

struct CatCommonName
{
  std::string name;
  std::uint64_t parameter_id = 0;
};

// The small tables of one generation, each sorted by id.
struct CatalogTables
{
  std::uint64_t generation = 0;
  std::vector<CatCenter> centers;
  std::vector<CatModel> models;
  std::vector<CatModelGrid> model_grids;
  std::vector<CatGrid> grids;
  std::vector<CatRun> runs;
  std::vector<CatSignature> signatures;
  std::vector<CatCommonName> common_names;  // by name

  // By id; nullptr if absent.
  const CatCenter* center(std::uint64_t id) const;
  const CatModel* model(std::uint64_t id) const;
  const CatModelGrid* model_grid(std::uint64_t id) const;
  const CatGrid* grid(std::uint64_t id) const;
  const CatRun* run(std::uint64_t id) const;
  const CatSignature* signature(std::uint64_t id) const;
  const CatSignature* signature_by_hash(std::uint64_t hash) const;
  // Sorts every list by id (call after filling).
  void sort();
};

class CatalogQuery
{
 public:
  virtual ~CatalogQuery();
  // Thread-safe.  std::runtime_error if the catalog cannot be read (the
  // last good tables are kept by implementations that have them).
  virtual std::shared_ptr<const CatalogTables> tables() = 0;
  // Signature ids present in a run.
  virtual std::vector<std::uint64_t> run_signatures(
    const CatalogTables& t,
    std::uint64_t run_id) = 0;
  // Every message of (run, signature), any level and offset.
  virtual std::vector<CatMessage> messages(
    const CatalogTables& t,
    std::uint64_t run_id,
    std::uint64_t signature_id) = 0;
  virtual std::optional<CatMessage> message(
    const CatalogTables& t,
    std::uint64_t id) = 0;
  // The live message that replaced `id` (a newer copy with other bytes,
  // Q16b); nullopt if `id` was never replaced (or its replacement is gone).
  virtual std::optional<std::uint64_t> replaced_by(
    const CatalogTables& t,
    std::uint64_t id) = 0;
  // Distinct offset_end_s of a run, ascending (T17c).
  virtual std::vector<std::int64_t> run_offsets(
    const CatalogTables& t,
    std::uint64_t run_id) = 0;
  // Every message of a run ending at that offset (valid at base + offset).
  virtual std::vector<CatMessage> messages_at(
    const CatalogTables& t,
    std::uint64_t run_id,
    std::int64_t offset_end_s) = 0;
};

// Decoded values of one GRIB2 message in the file's point order, NaN where
// missing (grib_values() in grib.h; injected so the core needs no eccodes).
using GribValuesDecoder =
  std::function<std::vector<double>(const void* msg, std::size_t len)>;

class GribApi;

// /v1/point, /v1/tiles and /v1/meta over the grid catalog (T17c, Q19-Q21):
// one store per catalog generation (GribApi::field_store()).
//   models    model grids' api_names (viewable grid, member -1 runs)
//   times     whole-hour offsets (offset_end_s) of those runs
//   params    ids 1.. over grib_parameter rows (and unlinked signatures);
//             name = the first common name, else the parameter name
//             lower-cased with spaces -> '_'; role from the GRIB2 numbers
//             of its instantaneous signatures (0/2/2 u, 0/2/3 v, 0/3/5
//             geopotential height)
//   fields    a parameter present twice at one level and time (two
//             signatures) is listed once with kFieldAmbiguous; a
//             fixed-length statistical parameter of N seconds without a
//             message at offset N uses its run-total sibling's 0..N
//             message (first-bucket repair, Q22c)
//   names     resolve_*: exact (case-insensitive), else the only name
//             containing the text (guess), NameError 400 ambiguous_name /
//             ambiguous_field, 404 not_found (Q20)
class CatalogFieldStore : public FieldStore
{
 public:
  CatalogFieldStore(
    std::shared_ptr<const CatalogTables> tables,
    CatalogQuery& catalog,
    const GribApi& api);
  ~CatalogFieldStore() override;

  std::uint64_t generation() const { return tables_->generation; }

  std::vector<std::string> models() const override;
  std::vector<ModelTime> times(std::string_view model) const override;
  std::shared_ptr<const Grid> grid(std::string_view model, const ModelTime& t)
    const override;
  std::vector<FieldInfo> fields(std::string_view model, const ModelTime& t)
    const override;
  float sample(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& key,
    const Grid::Cell& cell) const override;
  std::shared_ptr<const FieldView> view(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& key) const override;
  ParamInfo param(std::uint16_t id) const override;
  ResolvedName resolve_field(std::string_view name) const override;
  ResolvedName resolve_model(std::string_view name) const override;

  // The field name /v1/point uses for a signature (tree, Q20d).
  std::string field_name(std::uint64_t signature_id) const;
  // Common names of a signature's parameter.
  std::vector<std::string> common_names(std::uint64_t signature_id) const;

 private:
  struct Param
  {
    std::optional<std::uint64_t> parameter_id;  // else one unlinked sig
    ParamInfo info;
    std::string normalised;           // parameter name, lower, spaces -> '_'
    std::vector<std::string> common;  // lower-cased common names
  };
  struct Slot
  {
    FieldInfo info;
    const CatMessage* msg = nullptr;
    const CatSignature* sig = nullptr;
  };
  const CatRun* run_of(std::string_view model, std::int64_t run_s) const;
  // Messages of (model, t) by (param, level); nullptr if absent.
  std::vector<Slot> slots(
    std::string_view model,
    const ModelTime& t,
    std::vector<CatMessage>& keep) const;

  std::shared_ptr<const CatalogTables> tables_;
  CatalogQuery& catalog_;
  const GribApi& api_;
  std::vector<Param> params_;                          // id = index + 1
  std::map<std::uint64_t, std::uint16_t> sig_param_;   // signature -> id
  std::map<std::string, const CatModelGrid*> models_;  // api_name
  // Fixed-length statistical signatures by base_hash (first-bucket repair).
  std::map<std::uint64_t, std::vector<const CatSignature*>> fixed_by_base_;
};

// /v1/grib/* (see the API contract).  Thread-safe.
class GribApi
{
 public:
  // `catalog` and `cache` must outlive the API.  Without a cache, point
  // and tile requests answer 503; without `values`, /values answers 503.
  GribApi(
    std::filesystem::path root,
    CatalogQuery& catalog,
    GribCache* cache,
    GribValuesDecoder values = nullptr);
  ~GribApi();

  static bool handles(std::string_view path);
  fcgi::Response handle(
    const std::string& path,
    const std::map<std::string, std::string>& query,
    const fcgi::Request& req) const;

  // The catalog as a FieldStore for /v1/point, /v1/tiles, /v1/meta: the
  // current generation's (rebuilt when it changes).
  std::shared_ptr<const CatalogFieldStore> field_store() const;

  // For CatalogFieldStore: the grid of a viewable catalog grid; a decoded
  // message (nullptr without a cache), scaled to its parameter's units.
  std::shared_ptr<const Grid> load_grid(GridKey key) const;
  std::shared_ptr<const FieldView> decode(
    const CatMessage& msg,
    const CatGrid& grid,
    const CatSignature& sig,
    FieldInfo info) const;
  // Whether winds on `grid` are grid-relative (a property of the grid's
  // section 3): decodes `msg` the first time per grid, remembered after.
  // nullopt without a cache.
  std::optional<bool> uv_relative_to_grid(
    const CatMessage& msg,
    const CatGrid& grid,
    const CatSignature& sig) const;

 private:
  std::filesystem::path root_;
  CatalogQuery& catalog_;
  GribCache* cache_;
  GribValuesDecoder values_;
  mutable std::mutex mu_;
  mutable std::map<GridKey, std::shared_ptr<const Grid>> grids_;
  mutable std::shared_ptr<const CatalogFieldStore> store_;
  mutable std::map<GridKey, bool> uv_relative_;
};

}  // namespace atmo
