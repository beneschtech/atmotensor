// GRIB-native gridded store (T10c, Q10): GRIB2 files are kept exactly as
// published, under data/grib/<model>/<run YYYYmmddHH>/<file>, next to an
// index sidecar (<file>.idx.json) listing every usable message: byte range,
// content hash, variable, level, forecast hour, grid.  Queries decode the
// messages they need on demand through a process-wide GribCache.
//
// The index and the cache do not need eccodes; ingest (index_grib) and the
// decoder (grib_decoder) do and live in grib.h.
//
// Cache (Chris's design, Q10): a map from message hash to the decoded
// message (values in registry units and normalised grid order, plus the
// grid whose lats/lons it uses) with its last access time.  When a newly
// decoded message is added and the cache is over its byte budget, or the
// process RSS (/proc/self/statm) is over its limit, entries nobody holds
// are dropped oldest-access first (over budget: down to purge_to of the
// budget; RSS over the limit: the oldest (1 - purge_to) of the cache per
// insert).  Entries are handed out as shared_ptr:
// an entry a query still holds is never freed, even if evicted from the
// map meanwhile, so a purge cannot disturb a running query.  maintain() is
// the forced cleanup for a periodic timer.  Concurrent misses for the same
// message decode it once.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/fieldfile.h"
#include "atmo/grid.h"

namespace atmo
{

// One usable message of a GRIB file.
struct GribIndexEntry
{
  std::uint64_t offset = 0;  // byte offset of the message in the file
  std::uint64_t length = 0;  // total message length
  std::uint64_t hash = 0;    // FNV-1a of the message bytes: the cache key
  FieldInfo info;            // param_id, level_type, level_value, flags
  std::uint32_t fhr = 0;
  GridKey grid = 0;

  bool operator==(const GribIndexEntry&) const = default;
};

struct GribIndex
{
  static constexpr const char* kFormat = "atmo-grib-index";
  static constexpr int kVersion = 1;

  std::string model;
  std::int64_t run_s = 0;
  std::string grib;        // the GRIB file, relative to the store root
  std::uint64_t size = 0;  // its size in bytes
  std::uint64_t messages_total = 0;  // usable or not
  std::vector<GribIndexEntry> entries;
  std::vector<std::pair<std::uint64_t, std::string>> skipped;  // (msg, why)

  bool operator==(const GribIndex&) const = default;

  // Entries inside the file, (fhr, field) unique, one grid per fhr.
  // std::invalid_argument otherwise.
  void validate() const;
  nlohmann::json to_json() const;
  std::string to_text() const;
  static GribIndex from_json(const nlohmann::json& j);
  static GribIndex from_text(std::string_view text);
};

// A decoded message: values in the registry's units, normalised grid order
// (index = j*ni + i), NaN = missing; the coordinates are the grid's (one
// shared copy per grid, not per message).
struct GribMessageData
{
  std::uint64_t hash = 0;
  std::shared_ptr<const Grid> grid;
  std::vector<double> values;
  // Unix seconds of the last get(); updated by the cache.
  mutable std::atomic<std::int64_t> last_accessed{0};

  const double* lats() const { return grid->lat_deg(); }
  const double* lons() const { return grid->lon_deg(); }
  // Bytes this entry accounts for in the cache (the values; the grid is
  // shared and stays mapped anyway).
  std::size_t bytes() const;
};

// Decodes one indexed message: file path, its index entry, its grid.
using GribDecoder = std::function<std::shared_ptr<GribMessageData>(
  const std::filesystem::path& file,
  const GribIndexEntry& entry,
  std::shared_ptr<const Grid> grid)>;

struct GribCacheLimits
{
  std::size_t max_bytes = std::size_t{4} << 30;  // decoded values budget
  std::size_t rss_limit = 0;  // process RSS that forces a purge; 0 = off
  double purge_to = 0.75;     // purge down to this fraction of max_bytes
};

class GribCache
{
 public:
  struct Stats
  {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;     // decodes
    std::size_t coalesced = 0;  // waited for another thread's decode
    std::size_t evictions = 0;
  };

  // `decoder` may be empty (built without eccodes): get() then throws.
  // `rss` returns the process RSS in bytes (tests inject a fake).
  GribCache(
    GribCacheLimits limits,
    GribDecoder decoder,
    std::function<std::size_t()> rss = {});
  ~GribCache();
  GribCache(const GribCache&) = delete;
  GribCache& operator=(const GribCache&) = delete;

  // The decoded message, decoding it on a miss (at most once at a time per
  // hash).  Thread-safe.  Exceptions from the decoder propagate.
  std::shared_ptr<const GribMessageData> get(
    const std::filesystem::path& file,
    const GribIndexEntry& entry,
    std::shared_ptr<const Grid> grid);

  // Forced cleanup (call from a timer): if RSS is over the limit, drop
  // every entry nobody holds and return freed memory to the OS.
  void maintain();

  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Resident set size of this process in bytes (/proc/self/statm).
std::size_t process_rss();

}  // namespace atmo
