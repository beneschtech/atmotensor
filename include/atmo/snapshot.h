// Snapshot: a consistent, immutable view of every file named by one
// manifest (docs/design/02-manifest-and-processes.md, "Query daemon:
// snapshots").
//
// SnapshotLoader::refresh() reads CURRENT; when the seq has moved on it
// opens every file of the new manifest up front (never lazily, so expiry
// cannot pull a file out from under a request), checks each file's header
// CRC and metadata against its manifest entry, builds the ObsIndex, and
// swaps the new snapshot in atomically.  Files already open in the previous
// snapshot (same path and header CRC) reuse their mapping; new files get a
// full CRC check.  Each request takes current() once and uses only that.
//
// Not here yet: field files (T7; their entries are carried in the manifest
// but not opened), watching CURRENT with inotify (the query daemon, T10),
// pinning a retained older snapshot (T10).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "atmo/grid.h"
#include "atmo/manifest.h"
#include "atmo/obs_index.h"
#include "atmo/obs_segment.h"
#include "atmo/stations.h"

namespace atmo
{

// Immutable once built; thread-safe.
class Snapshot
{
 public:
  using SegPtr = std::shared_ptr<const ObsSegment>;
  using GridPtr = std::shared_ptr<const Grid>;
  using StationsPtr = std::shared_ptr<const StationTable>;

  // Opens everything `m` names under `root`.  `previous` (may be null)
  // donates mappings of unchanged files.  std::runtime_error if a file is
  // missing, corrupt or disagrees with its entry.
  Snapshot(
    const std::filesystem::path& root,
    Manifest m,
    const Snapshot* previous);
  ~Snapshot();

  std::uint64_t seq() const { return manifest_.seq; }
  const Manifest& manifest() const { return manifest_; }
  const SourceRegistry& sources() const { return manifest_.sources; }

  // nullptr if the manifest names no station generation.
  const StationsPtr& stations() const { return stations_; }
  // nullptr if the grid is not in the snapshot.
  GridPtr grid(GridKey key) const;
  // In manifest order.
  const std::vector<SegPtr>& segments() const { return segments_; }
  const ObsIndex& obs() const { return *obs_; }

  // Files opened for this snapshot (not reused from the previous one).
  std::size_t files_opened() const { return opened_; }

 private:
  Manifest manifest_;
  StationsPtr stations_;
  std::map<GridKey, GridPtr> grids_;
  std::vector<SegPtr> segments_;
  std::unique_ptr<ObsIndex> obs_;
  std::size_t opened_ = 0;
};

class SnapshotLoader
{
 public:
  explicit SnapshotLoader(std::filesystem::path root);

  // Adopts the manifest CURRENT names if it is newer than current().
  // Returns true if a new snapshot was swapped in.  On error
  // (std::runtime_error) current() stays as it was.  Thread-safe;
  // concurrent calls are serialised, and the seq never goes backwards.
  bool refresh();

  // nullptr until the first successful refresh() that found a manifest.
  std::shared_ptr<const Snapshot> current() const;

 private:
  std::filesystem::path root_;
  std::mutex refresh_mu_;
  std::atomic<std::shared_ptr<const Snapshot>> cur_;
};

}  // namespace atmo
