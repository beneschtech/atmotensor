// Query engine (T9; 04-query-api.md "/v1/point"): gridded values at a point
// (space, time and vertical interpolation) plus nearby observations.
//
// Gridded data is read through FieldStore, so the engine does not care how
// fields are stored: SnapshotFieldStore serves T7 field files, and a
// GRIB-native store (Q10) can implement the same interface.
//
// Field specs: "name[@level]" with name from the variable registry and
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
};

// FieldStore over a snapshot: T7 field files and GRIB-native files
// (T10c).  GRIB messages are decoded on demand through `cache`; without a
// cache they are listed but sample to NaN.  Several GRIB files may serve
// one (model, run, fhr) (e.g. HRRR wrfsfc + wrfprs): their messages are
// merged, the first listed wins on duplicates, messages on a grid other
// than the first one's are ignored.
class SnapshotFieldStore : public FieldStore
{
 public:
  explicit SnapshotFieldStore(
    std::shared_ptr<const Snapshot> snapshot,
    GribCache* cache = nullptr);
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

 private:
  std::shared_ptr<const FieldFile> file(
    std::string_view model,
    const ModelTime& t) const;

  struct GribMsg
  {
    const std::filesystem::path* file;
    const GribIndexEntry* entry;
  };
  struct GribSlice
  {
    GridKey grid = 0;
    std::vector<GribMsg> msgs;
  };
  const GribSlice* grib_slice(std::string_view model, const ModelTime& t) const;

  std::shared_ptr<const Snapshot> snap_;
  GribCache* cache_ = nullptr;
  std::vector<std::pair<std::string, std::vector<ModelTime>>> times_;
  std::vector<std::filesystem::path> grib_files_;  // absolute
  std::map<std::tuple<std::string, std::int64_t, std::uint32_t>, GribSlice>
    gribs_;
};

struct FieldSpec
{
  enum class Level
  {
    query,  // no @level: the query's vertical coordinate
    fixed,  // level_type / level_value given
  };

  std::string text;  // canonical form, e.g. "air_temperature@2m"
  const VariableInfo* var = nullptr;
  Level level = Level::query;
  std::uint8_t level_type = 0;
  float level_value = 0.0f;  // SI

  // std::invalid_argument for unknown variables or malformed levels.
  static FieldSpec parse(std::string_view text);
  // Canonical spec of a stored field ("name@level").
  static std::string format(const FieldInfo& f);
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
    no_time,         // no run covers the time
    outside_grid,    // point not on the model's grid
    no_field,        // variable/level not stored
    level_required,  // several levels, no vertical coordinate given
    out_of_levels,   // vertical coordinate outside the stored levels
  };

  std::string model;
  std::string field;  // the spec as requested (canonical)
  const VariableInfo* var = nullptr;
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
};

// std::invalid_argument for an inconsistent query (both level_hpa and
// alt_m, non-finite coordinates, unknown model).  `obs` may be null if
// q.obs is false.
PointResult point(
  const FieldStore& store,
  const ObsIndex* obs,
  const SourceRegistry& sources,
  const PointQuery& q);

const char* to_string(PointValue::Status s);
// "2026-11-08T18:00:00Z"
std::string iso8601(std::int64_t t_s);
// Parses "2026-11-08T18:00:00Z" (seconds and Z optional: "2026-11-08T18:00").
// std::invalid_argument if malformed.
std::int64_t parse_iso8601(std::string_view text);

// JSON for /v1/point.  Values are rounded to float precision (that is what
// is stored); NaN becomes null.  Valid while the snapshot is held.
nlohmann::json to_json(const PointResult& r);
// JSON for /v1/meta: models with runs, forecast hours and fields; obs
// source classes; station generation.
nlohmann::json meta_json(const Snapshot& s, const FieldStore& store);

}  // namespace atmo
