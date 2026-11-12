// Query engine (T9; 04-query-api.md "/v1/point"): gridded values at a point
// (space, time and vertical interpolation) plus nearby observations.
//
// Gridded data is read through FieldStore, so the engine does not care how
// fields are stored: SnapshotFieldStore serves T7 field files, and a
// GRIB-native store (Q10) can implement the same interface.
//
// Field specs: "name[@level]", the name resolved by the FieldStore (the
// variable registry by default; the grid catalog's common / parameter
// names with best-guess matching for CatalogFieldStore, Q19/Q20), and
//   @sfc        ground or water surface       (GRIB2 level type 1)
//   @msl        mean sea level                (101)
//   @<x>m       x metres above ground         (103), e.g. @2m, @10m
//   @<x>hPa     isobaric surface              (100), e.g. @850hPa
//   @L<t>=<v>   any level type t, value v SI  (e.g. @L104=0.44)
// Without @level the query's vertical coordinate applies: level_hpa
// (log-pressure interpolation between isobaric levels) or alt_m (geometric
// altitude above MSL, located through geopotential height on isobaric
// levels, Q3); without either, a field that exists on exactly one level
// uses that level.  An explicit isobaric level between two stored ones is
// interpolated in log p too.  (API details: Q11.)
//
// Time: per model, the newest run whose forecast hours bracket the query
// time; linear interpolation between the two hours (exact hour: one).
// Winds: grid-relative u/v (kFieldGridRelative) are rotated to
// earth-relative with the grid's rotation at the point.
#pragma once

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/fieldfile.h"
#include "atmo/gribstore.h"
#include "atmo/grid.h"
#include "atmo/obs_index.h"
#include "atmo/variable.h"

namespace atmo
{

class Snapshot;

// GRIB2 code table 4.5 level types used by field specs.
constexpr std::uint8_t kLevelSurface = 1;
constexpr std::uint8_t kLevelIsobaric = 100;
constexpr std::uint8_t kLevelMsl = 101;
constexpr std::uint8_t kLevelHeightAgl = 103;

// What point() and the tiles need to know about a parameter beyond its
// values: the u/v pair for rotation and geopotential height for altitude.
enum class ParamRole
{
  other,
  wind_u,
  wind_v,
  geopotential_height,
};

struct ParamInfo
{
  std::string name;  // field name as specs and responses spell it
  std::string units;
  ParamRole role = ParamRole::other;
};

// A model or field name that cannot be used (Q20): HTTP status and error
// code for the API layer; `candidates` for ambiguous_name.
class NameError : public std::invalid_argument
{
 public:
  NameError(
    int status,
    std::string code,
    const std::string& message,
    std::vector<std::string> candidates = {});
  int status;
  std::string code;
  std::vector<std::string> candidates;
};

// A best guess (Q20b): `asked` matched exactly one name partially.
struct NameWarning
{
  std::string argument;  // "models", "fields", "model", "field"
  std::string asked;
  std::string used;
};

struct ResolvedName
{
  std::string name;         // canonical
  std::uint16_t param = 0;  // fields: the store's parameter id
  bool guess = false;
};

struct FieldKey
{
  std::uint16_t param_id = 0;
  std::uint8_t level_type = 0;
  float level_value = 0.0f;

  bool operator==(const FieldKey&) const = default;
};

struct ModelTime
{
  std::int64_t run_s = 0;
  std::uint32_t fhr = 0;

  std::int64_t valid_s() const;
  bool operator==(const ModelTime&) const = default;
};

// One field at one time, for bulk access (tiles): values by grid point.
// Immutable; holds whatever it needs (a decoded GRIB message, a mapped
// field file) for as long as it lives.
class FieldView
{
 public:
  virtual ~FieldView();
  // Value at grid point (i, j) of the view's grid; NaN = missing.
  virtual double at(std::uint32_t i, std::uint32_t j) const = 0;
  const Grid& grid() const { return *grid_; }
  const FieldInfo& info() const { return info_; }

  // Bilinear inside `cell` (the corner i+1 wraps on periodic grids); NaN
  // if a corner is missing or the cell does not exist.
  double sample(const Grid::Cell& cell) const;

 protected:
  FieldView(std::shared_ptr<const Grid> grid, FieldInfo info);

 private:
  std::shared_ptr<const Grid> grid_;
  FieldInfo info_;
};

// Read access to gridded fields.  Implementations are immutable and
// thread-safe.
class FieldStore
{
 public:
  virtual ~FieldStore();
  // Model names, sorted.
  virtual std::vector<std::string> models() const = 0;
  // Every (run, fhr) of a model, sorted by run then fhr.
  virtual std::vector<ModelTime> times(std::string_view model) const = 0;
  // nullptr if the model has no data at that time.
  virtual std::shared_ptr<const Grid> grid(
    std::string_view model,
    const ModelTime& t) const = 0;
  // Fields present at that time (with flags, min, max).
  virtual std::vector<FieldInfo> fields(
    std::string_view model,
    const ModelTime& t) const = 0;
  // Bilinear value in `cell` of grid(model, t); NaN if missing or absent.
  virtual float sample(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& key,
    const Grid::Cell& cell) const = 0;
  // The whole field for bulk access; nullptr if absent (or not decodable
  // here, e.g. GRIB without a cache).  May decode (GRIB).
  virtual std::shared_ptr<const FieldView> view(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& key) const = 0;

  // Names.  The defaults use the variable registry (param id = variable
  // code) and exact model names; NameError(400, "bad_request") for
  // unknown ones.
  virtual ParamInfo param(std::uint16_t id) const;
  virtual ResolvedName resolve_field(std::string_view name) const;
  virtual ResolvedName resolve_model(std::string_view name) const;
};

// FieldStore over a snapshot's T7 field files (atmo_grib_ingest --tiles).
// GRIB files are served by CatalogFieldStore (grib_api.h) since T18.
class SnapshotFieldStore : public FieldStore
{
 public:
  explicit SnapshotFieldStore(std::shared_ptr<const Snapshot> snapshot);
  ~SnapshotFieldStore() override;

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

 private:
  std::shared_ptr<const FieldFile> file(
    std::string_view model,
    const ModelTime& t) const;

  std::shared_ptr<const Snapshot> snap_;
  std::vector<std::pair<std::string, std::vector<ModelTime>>> times_;
};

struct FieldSpec
{
  enum class Level
  {
    query,  // no @level: the query's vertical coordinate
    fixed,  // level_type / level_value given
  };

  std::string text;  // canonical form, e.g. "air_temperature@2m"
  std::string name;  // the name part as given
  // The registry's entry if it knows `name` (else nullptr): set by parse.
  const VariableInfo* var = nullptr;
  // The store's parameter id: set by resolve() (point() and the tiles
  // resolve specs that are not resolved yet).
  std::uint16_t param = 0;
  Level level = Level::query;
  std::uint8_t level_type = 0;
  float level_value = 0.0f;  // SI

  // Syntax only: std::invalid_argument for a malformed name or level.
  static FieldSpec parse(std::string_view text);
  // Resolves the name through `store` (NameError if it cannot); sets
  // `param` and the canonical `text`; a best guess is appended to
  // `warnings` (if given) as argument `argument`.
  void resolve(
    const FieldStore& store,
    std::vector<NameWarning>* warnings,
    const std::string& argument = "fields");
  // Canonical spec of a stored field ("name@level"); the name from the
  // registry, or from `store` if given.
  static std::string format(const FieldInfo& f);
  static std::string format(const FieldInfo& f, const FieldStore& store);
};

struct PointQuery
{
  double lat_deg = 0.0;
  double lon_deg = 0.0;
  std::int64_t t_s = 0;  // unix seconds
  // At most one vertical coordinate.
  std::optional<double> level_hpa;
  std::optional<double> alt_m;  // geometric, above MSL
  std::vector<FieldSpec> fields;
  std::vector<std::string> models;  // empty = every model
  bool obs = false;
  float obs_radius_km = 50.0f;  // in each source class's space-time metric
  std::uint64_t as_of = kObsLatest;
};

struct PointValue
{
  enum class Status
  {
    ok,
    no_time,          // no run covers the time
    outside_grid,     // point not on the model's grid
    no_field,         // variable/level not stored
    level_required,   // several levels, no vertical coordinate given
    out_of_levels,    // vertical coordinate outside the stored levels
    ambiguous_field,  // several signatures of the parameter (catalog)
  };

  std::string model;
  std::string field;                  // the spec as requested (canonical)
  const VariableInfo* var = nullptr;  // registry entry, if any
  std::string units;
  Status status = Status::ok;
  // NaN unless status is ok and the point has data.
  double value = std::numeric_limits<double>::quiet_NaN();
  std::int64_t run_s = 0;
  double fhr = 0.0;  // fractional when interpolated in time
};

struct PointResult
{
  std::vector<PointValue> values;  // model order, then field order
  std::vector<std::pair<std::string, ObsHit>> obs;  // (source, hit)
  std::vector<NameWarning> warnings;  // best guesses (models, fields)
};

// std::invalid_argument for an inconsistent query (both level_hpa and
// alt_m, non-finite coordinates); NameError for model / field names the
// store cannot resolve (q.models and q.fields are resolved here).  `obs` may be
// null if q.obs is false.
PointResult point(
  const FieldStore& store,
  const ObsIndex* obs,
  const SourceRegistry& sources,
  const PointQuery& q);

const char* to_string(PointValue::Status s);
// Rotation of the grid's +i axis at a cell (bilinear in sin/cos, so a wrap
// at +-pi cannot spoil it).
double grid_alpha_at(const Grid& g, const Grid::Cell& c);
// "2026-11-08T18:00:00Z"
std::string iso8601(std::int64_t t_s);
// Parses "2026-11-08T18:00:00Z" (seconds and Z optional: "2026-11-08T18:00").
// std::invalid_argument if malformed.
std::int64_t parse_iso8601(std::string_view text);

// JSON for /v1/point.  Values are rounded to float precision (that is what
// is stored); NaN becomes null.  Valid while the snapshot is held.
nlohmann::json to_json(const PointResult& r);
// The "warnings" array of a response (Q20b).
nlohmann::json warnings_json(const std::vector<NameWarning>& w);
// JSON for /v1/meta: models with runs, forecast hours and fields; obs
// source classes; station generation.
nlohmann::json meta_json(const Snapshot& s, const FieldStore& store);

}  // namespace atmo
