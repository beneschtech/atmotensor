// Station table (docs/design/01-obs-store.md, "Station table"; decision D10).
//
// The authoritative, editable station list will live in MySQL; ingest
// exports it as an immutable generation file `stations/stations-<gen>.tbl`
// that the manifest names, so every query snapshot sees exactly one
// generation and old answers stay reproducible.  Query daemons only ever
// read these files.
//
// Model
//   * A station has a ref (u32, > 0), a name, flags, typed identifiers and
//     position periods.  The ref identifies the station itself, not one of
//     its periods, and is never reused: a new generation must contain every
//     ref of the previous one.  station_ref 0 means "no station" in reports.
//   * Refs come from two disjoint ranges (Q7), so the web app and ingest can
//     both create stations without coordinating: the web app assigns
//     [1, 2^31), ingest assigns provisional stations [2^31, 2^32).  In each
//     range refs only grow: a new station's ref must be above that range's
//     high-water mark in the previous generation.  A provisional station
//     keeps its ref forever (reports point at it); confirming it clears the
//     flag, and if it duplicates another station it is kept as an alias
//     (merged_into = that station's ref).
//   * Identifiers are typed (ICAO, WMO, WIGOS, platform) and bound to a
//     station for a validity interval, because codes get reassigned.  For one
//     (type, value) the intervals never overlap across the whole table, so
//     (identifier, time) names at most one station.
//   * Periods are [valid_from_s, valid_to_s) with position and elevation; a
//     station that moves or is re-surveyed gets a new period.  Within a
//     station they are sorted and do not overlap.  Mobile platforms and
//     provisional stations (seen in a report, not yet in the authoritative
//     list) may have no periods: their positions come from the reports.
//
// File layout (little-endian; same conventions as obs segments): 4096-byte
// header page, then five 4096-aligned sections, each CRC-32 protected, with
// columns 8-byte aligned inside a section.  The reader recomputes every
// offset from the counts.
//   stations     sorted by ref: ref u32, flags u16, first_period u32,
//                n_periods u32, first_id u32, n_ids u32, name_off u32,
//                name_len u32, merged_into u32
//   periods      contiguous per station: valid_from_s i64, valid_to_s i64,
//                lat_deg f32, lon_deg f32, elevation_m f32
//   ids          identifier bindings sorted by (type, value, valid_from_s):
//                type u16, value_off u32, value_len u32, ref u32,
//                valid_from_s i64, valid_to_s i64
//   station_ids  per station, indices into ids (u32)
//   strings      names and identifier values
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atmo
{

constexpr std::int64_t kTimeMin = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kTimeMax = std::numeric_limits<std::int64_t>::max();

// First ref of the range ingest assigns to provisional stations (Q7).
constexpr std::uint32_t kProvisionalRefBase = 1u << 31;

// Station flags.
constexpr std::uint16_t kStationProvisional = 1u << 0;  // not yet confirmed
constexpr std::uint16_t kStationMobile = 1u << 1;       // ship, buoy, aircraft

enum class StationIdType : std::uint16_t
{
  icao = 1,      // 4 characters, A-Z 0-9 (KOKC)
  wmo = 2,       // 5-digit block + station number (72357)
  wigos = 3,     // WIGOS station identifier (0-20000-0-72357)
  platform = 4,  // ship call sign, buoy or aircraft id
};

struct StationId
{
  StationIdType type = StationIdType::icao;
  std::string value;

  bool operator==(const StationId& o) const = default;
};

struct StationIdBinding
{
  StationId id;
  std::int64_t valid_from_s = kTimeMin;
  std::int64_t valid_to_s = kTimeMax;  // exclusive

  bool operator==(const StationIdBinding& o) const = default;
};

struct StationPeriod
{
  std::int64_t valid_from_s = kTimeMin;
  std::int64_t valid_to_s = kTimeMax;  // exclusive
  float lat_deg = 0.0f;
  float lon_deg = 0.0f;
  float elevation_m = 0.0f;  // geometric, above MSL

  bool operator==(const StationPeriod& o) const = default;
};

// An owned station, as handed to the builder.
struct Station
{
  std::uint32_t ref = 0;  // 0 = let the builder assign one
  std::string name;
  std::uint16_t flags = 0;
  // 0, or the ref of the station this one turned out to duplicate.  The
  // target must exist and must not itself be merged (no chains).
  std::uint32_t merged_into = 0;
  std::vector<StationIdBinding> ids;
  std::vector<StationPeriod> periods;

  bool operator==(const Station& o) const = default;
};

class StationTable;

// Builds one generation.  Start empty (generation 1) or from the previous
// generation, which copies every station so no ref can disappear.
class StationTableBuilder
{
 public:
  StationTableBuilder();
  explicit StationTableBuilder(const StationTable& previous);

  // Adds a new station and returns its ref.  ref 0 assigns the next ref of
  // the provisional range if the station is flagged provisional, else of
  // the web-app range.  An explicit ref (from the web app's export) must be
  // above its range's high-water mark in the previous generation and
  // unused.  Validates the station on its own (std::invalid_argument);
  // identifier overlaps and merged_into targets are checked by seal().
  std::uint32_t add(Station s);

  // Replaces an existing station's data (same ref).  std::out_of_range if
  // the ref does not exist.
  void update(Station s);

  const Station* find(std::uint32_t ref) const;
  std::size_t size() const { return stations_.size(); }
  std::uint64_t generation() const { return generation_; }
  // High-water marks: highest ref in [1, 2^31) and in [2^31, 2^32), 0 if
  // the range is unused.
  std::uint32_t max_ref() const { return max_ref_; }
  std::uint32_t max_provisional_ref() const { return max_prov_ref_; }

  // Writes `dir`/stations-<generation>.tbl (see StationTable::file_name)
  // with the same atomic, never-replace protocol as obs segments, and
  // returns its path.  std::invalid_argument if two stations bind the same
  // identifier for overlapping times, or a merged_into target is missing or
  // merged itself.  A builder seals exactly once.
  std::filesystem::path seal(const std::filesystem::path& dir);

 private:
  std::uint64_t generation_ = 1;
  std::uint32_t base_max_ref_ = 0;
  std::uint32_t base_max_prov_ref_ = 0;
  std::uint32_t max_ref_ = 0;
  std::uint32_t max_prov_ref_ = 0;
  bool sealed_ = false;
  std::map<std::uint32_t, Station> stations_;
};

// One identifier binding in a sealed table.
struct StationIdView
{
  StationIdType type = StationIdType::icao;
  std::string_view value;
  std::uint32_t ref = 0;
  std::int64_t valid_from_s = kTimeMin;
  std::int64_t valid_to_s = kTimeMax;
};

// A station in a sealed table.  Valid while the StationTable is alive.
class StationView
{
 public:
  StationView() = default;
  StationView(const StationTable* table, std::size_t index);

  std::uint32_t ref() const;
  std::string_view name() const;
  std::uint16_t flags() const;
  std::uint32_t merged_into() const;  // 0 = not an alias
  std::size_t n_periods() const;
  StationPeriod period(std::size_t k) const;
  // The period containing t_s, if any.
  std::optional<StationPeriod> period_at(std::int64_t t_s) const;
  std::size_t n_ids() const;
  StationIdView id(std::size_t k) const;

  // Owned copy, e.g. to start the next generation's edit.
  Station to_station() const;

 private:
  const StationTable* table_ = nullptr;
  std::size_t index_ = 0;
};

// Result of an (identifier, time) lookup.
struct StationAt
{
  StationView station;
  std::optional<StationPeriod> period;  // nullopt: no position at that time
};

// A sealed station table generation, mmap'd read-only.  Immutable and
// thread-safe.
class StationTable
{
 public:
  enum class Verify
  {
    full,    // header + every section CRC
    header,  // header CRC only (file already fully verified once)
  };

  // "stations-<gen as 16 hex digits>.tbl"
  static std::string file_name(std::uint64_t generation);

  // Throws std::runtime_error if missing, truncated or corrupt.
  // Structural checks always run.
  static std::shared_ptr<const StationTable> open(
    const std::filesystem::path& path,
    Verify verify = Verify::full);

  const std::filesystem::path& path() const { return path_; }
  std::uint64_t generation() const { return generation_; }
  std::uint32_t max_ref() const { return max_ref_; }
  std::uint32_t max_provisional_ref() const { return max_prov_ref_; }
  std::uint32_t header_crc() const { return header_crc_; }
  std::size_t size() const { return n_stations_; }

  // Stations in ref order.
  StationView station(std::size_t index) const;
  std::optional<StationView> by_ref(std::uint32_t ref) const;

  // Every binding of an identifier, ordered by valid_from_s.
  std::vector<StationIdView> lookup(StationIdType type, std::string_view value)
    const;
  // The station the identifier named at t_s, with its period at t_s.
  std::optional<StationAt>
  lookup(StationIdType type, std::string_view value, std::int64_t t_s) const;

 private:
  friend class StationView;

  StationTable(const std::filesystem::path& path, Verify verify);

  StationIdView id_view(std::size_t k) const;
  StationPeriod period_view(std::size_t k) const;
  std::string_view str(std::uint32_t off, std::uint32_t len) const;
  // First binding not less than (type, value).
  std::size_t lower_bound(StationIdType type, std::string_view value) const;

  std::filesystem::path path_;
  std::shared_ptr<const void> map_;  // keeps the mapping alive
  std::uint64_t generation_ = 0;
  std::uint32_t max_ref_ = 0;
  std::uint32_t max_prov_ref_ = 0;
  std::uint32_t header_crc_ = 0;
  std::size_t n_stations_ = 0;
  std::size_t n_periods_ = 0;
  std::size_t n_ids_ = 0;

  // Column pointers into the mapping.
  const std::uint32_t* st_ref_ = nullptr;
  const std::uint16_t* st_flags_ = nullptr;
  const std::uint32_t* st_first_period_ = nullptr;
  const std::uint32_t* st_n_periods_ = nullptr;
  const std::uint32_t* st_first_id_ = nullptr;
  const std::uint32_t* st_n_ids_ = nullptr;
  const std::uint32_t* st_name_off_ = nullptr;
  const std::uint32_t* st_name_len_ = nullptr;
  const std::uint32_t* st_merged_ = nullptr;
  const std::int64_t* p_from_ = nullptr;
  const std::int64_t* p_to_ = nullptr;
  const float* p_lat_ = nullptr;
  const float* p_lon_ = nullptr;
  const float* p_elev_ = nullptr;
  const std::uint16_t* id_type_ = nullptr;
  const std::uint32_t* id_value_off_ = nullptr;
  const std::uint32_t* id_value_len_ = nullptr;
  const std::uint32_t* id_ref_ = nullptr;
  const std::int64_t* id_from_ = nullptr;
  const std::int64_t* id_to_ = nullptr;
  const std::uint32_t* station_ids_ = nullptr;
  const char* strings_ = nullptr;
  std::size_t strings_len_ = 0;
};

}  // namespace atmo
