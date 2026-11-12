// Manifest: the list of every file a snapshot may use, and the publish
// protocol that makes a new list visible atomically
// (docs/design/02-manifest-and-processes.md).
//
// Layout under the root (deployed: /opt/atmotensor/data):
//   manifest/MANIFEST-<seq:016x>.json   immutable, one per publish
//   manifest/CURRENT                    "atmo-current 1 <seq> <crc32 hex>\n"
//   manifest/LOCK                       flock held by the one publisher
//
// Every path in a manifest is relative to the root and lives under the
// directory of its kind (grids/, stations/, fields/, obs/<source>/).
//
// Publish protocol (ManifestPublisher::publish):
//   1. the caller has already published the new data files;
//   2. write MANIFEST-<seq+1>.json (temp, fsync, link, fsync dir);
//   3. write CURRENT.tmp (seq + CRC-32 of the manifest bytes), fsync,
//      rename over CURRENT, fsync dir.
// A reader that sees CURRENT at seq N can read MANIFEST-N and every file
// it names; a crash at any point leaves the previous state intact.
//
// Config errors (a bad manifest handed to publish) are
// std::invalid_argument; unreadable or corrupt files on disk are
// std::runtime_error.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/grid.h"
#include "atmo/source.h"

namespace atmo
{

class FieldFile;
class ObsSegment;
class StationTable;

struct StationsEntry
{
  std::uint64_t generation = 0;
  std::string path;  // stations/stations-<gen>.tbl
  std::uint32_t header_crc = 0;

  bool operator==(const StationsEntry&) const = default;
};

struct GridEntry
{
  GridKey key = 0;
  std::string path;  // grids/<key as 16 hex digits>.grid
  std::uint32_t header_crc = 0;

  bool operator==(const GridEntry&) const = default;
};

struct FieldEntry
{
  std::string model;       // [a-z][a-z0-9_-]*, at most 31 characters
  std::int64_t run_s = 0;  // model run (reference) time, unix seconds
  std::uint32_t fhr = 0;   // forecast hour
  GridKey grid_key = 0;    // must be in the manifest's grids
  std::string path;        // fields/<model>/<run YYYYmmddHH>/f<fhr:03>.fld
  std::uint32_t header_crc = 0;

  bool operator==(const FieldEntry&) const = default;

  // The entry describing an opened field file under `root`.
  static FieldEntry from_file(
    const std::filesystem::path& root,
    const FieldFile& file);
};

struct SegmentEntry
{
  std::string source;  // a source class of the manifest
  std::int64_t bucket_start_s = 0;
  // Length of the bucket the segment was written for (may differ from the
  // source class after a bucket_seconds change); > 0.
  std::int64_t bucket_seconds = 0;
  std::string path;  // obs/<source>/<bucket_start>/<seg-id>.seg
  std::uint64_t receipt_seq_min = 0;  // 0/0 for an empty segment
  std::uint64_t receipt_seq_max = 0;
  std::uint64_t report_count = 0;
  std::uint32_t header_crc = 0;

  bool operator==(const SegmentEntry&) const = default;

  // The entry describing an opened segment; `root` is the store root and
  // the segment's path must lie inside it.
  static SegmentEntry from_segment(
    const std::filesystem::path& root,
    const ObsSegment& seg);
};

// A file that a manifest stopped naming, kept until the grace period has
// passed (02-manifest-and-processes.md, "Retirement").
struct RetiredEntry
{
  std::string path;  // relative, under grids/ stations/ fields/ grib/ obs/
  std::int64_t retired_at_s = 0;

  bool operator==(const RetiredEntry&) const = default;
};

struct Manifest
{
  static constexpr const char* kFormat = "atmo-manifest";
  // 2 (T18, D13): no "gribs" list; GRIB files belong to the grid catalog.
  // Version 1 is still read: its gribs' .idx.json sidecars become retired
  // entries (unlinked after the grace period), the GRIB files themselves
  // are left alone (the catalog may use the same files).
  static constexpr int kVersion = 2;

  std::uint64_t seq = 0;  // 0 = nothing published yet
  std::int64_t created_at_s = 0;
  // Next receipt_seq ingest will assign; greater than every segment's
  // receipt_seq_max.
  std::uint64_t next_receipt_seq = 1;
  SourceRegistry sources;
  std::optional<StationsEntry> stations;
  std::vector<GridEntry> grids;
  std::vector<FieldEntry> fields;
  std::vector<SegmentEntry> segments;
  // Managed by ManifestPublisher (publish() and collect_garbage()); never
  // names a file the manifest also lists.
  std::vector<RetiredEntry> retired;

  bool operator==(const Manifest&) const = default;

  // "MANIFEST-<seq as 16 hex digits>.json"
  static std::string file_name(std::uint64_t seq);

  // At least one source class; paths relative, under their kind's
  // directory, no "." or ".." components, unique (live and retired
  // together); grid keys and (model, run, fhr) unique; field grids present;
  // segment sources known, bucket_seconds > 0; receipt seqs below
  // next_receipt_seq.  std::invalid_argument otherwise.
  void validate() const;

  // Every file the manifest lists (not the retired ones).
  std::vector<std::string> live_paths() const;

  nlohmann::json to_json() const;
  std::string to_text() const;
  // Strict: format/version required, unknown keys rejected, validate()
  // run.  std::invalid_argument on any error.
  static Manifest from_json(const nlohmann::json& j);
  static Manifest from_text(std::string_view text);
};

// Steps of ManifestPublisher::publish, for fault injection in tests.
enum class PublishStep
{
  manifest_written,  // MANIFEST-<seq> durable, CURRENT untouched
  current_written,   // CURRENT.tmp durable, not yet renamed
  current_renamed,   // CURRENT replaced, directory not yet fsynced
};

struct GcReport
{
  std::size_t retired_unlinked = 0;  // grace period over
  std::size_t orphans_unlinked = 0;  // named by nothing, older than grace
  std::size_t dirs_removed = 0;      // emptied data directories
  std::optional<std::uint64_t> published_seq;
};

// The sole publisher.  Construction takes manifest/LOCK (flock, never
// waits) and throws std::runtime_error if another process or object holds
// it.  It then loads the current manifest and removes leftovers of a
// publish that crashed before its rename (MANIFEST files above CURRENT,
// CURRENT.tmp).  Not thread-safe; one ingest thread publishes.
class ManifestPublisher
{
 public:
  static constexpr std::size_t kDefaultKeep = 100;

  explicit ManifestPublisher(
    std::filesystem::path root,
    std::size_t keep = kDefaultKeep);
  ~ManifestPublisher();
  ManifestPublisher(const ManifestPublisher&) = delete;
  ManifestPublisher& operator=(const ManifestPublisher&) = delete;

  const std::filesystem::path& root() const { return root_; }
  // The last published manifest (seq 0 and empty if none yet).
  const Manifest& current() const { return current_; }

  static constexpr std::int64_t kDefaultGraceSeconds = 600;

  // Publishes `next` as seq current().seq + 1 with created_at = now.
  // `next.retired` is ignored: the publisher carries current().retired
  // forward and adds every file current() lists but `next` does not
  // (retired_at = now); a retired file listed again is live again.
  // Besides validate(): every named file must exist, next_receipt_seq and
  // the station generation must not go backwards.  std::invalid_argument
  // for those, std::runtime_error on I/O failure.  On any exception the
  // published state is unchanged (current() too), or, if CURRENT was
  // already renamed, it is the new one and a new publisher will see it.
  // Afterwards manifests older than the newest `keep` are deleted.
  const Manifest& publish(Manifest next);

  // Retirement and gc (02-manifest-and-processes.md):
  //  1. retired files whose grace period is over are dropped from the
  //     retired list (one publish), then unlinked;
  //  2. any regular file under stations/, fields/, obs/ or tmp/ (not
  //     grids/ or grib/, which the grid catalog shares, T18)
  //     that the current manifest neither lists nor retires and whose mtime
  //     is at least `grace_s` old is unlinked (crash leftovers, temp files,
  //     files whose retired entry was already dropped);
  //  3. data directories left empty are removed (not the top-level ones).
  // manifest/, config/ and journal/ are never touched.  Missing files are
  // not errors.  std::runtime_error on I/O failure.
  GcReport collect_garbage(std::int64_t grace_s = kDefaultGraceSeconds);

  // Unix seconds used for created_at, retired_at and gc; tests set a fake
  // clock.  Default: the system clock.
  void set_clock(std::function<std::int64_t()> clock);
  std::int64_t now() const;

  // Called after each step; used by tests to simulate a crash by throwing.
  void set_fault_hook(std::function<void(PublishStep)> hook);

 private:
  const Manifest& publish_impl(Manifest next, std::vector<RetiredEntry> keep);
  void recover();
  void prune() const;
  void fault(PublishStep s) const;

  std::filesystem::path root_;
  std::filesystem::path dir_;  // root_/manifest
  std::size_t keep_ = kDefaultKeep;
  int lock_fd_ = -1;
  Manifest current_;
  std::function<void(PublishStep)> hook_;
  std::function<std::int64_t()> clock_;
};

// Seq named by manifest/CURRENT; nullopt if there is no CURRENT yet.
// std::runtime_error if CURRENT is malformed.
std::optional<std::uint64_t> read_current_seq(
  const std::filesystem::path& root);

// The manifest CURRENT names, its bytes checked against CURRENT's CRC.
// Retries if a publish races the read.  nullopt if nothing was published
// yet; std::runtime_error on corruption.
std::optional<Manifest> read_current_manifest(
  const std::filesystem::path& root);

// A retained manifest by seq (no CRC to check it against besides JSON
// validity).  std::runtime_error if missing or invalid.
Manifest read_manifest(const std::filesystem::path& root, std::uint64_t seq);

}  // namespace atmo
