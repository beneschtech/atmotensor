// ObsIndex: visible-version queries over every sealed segment of a snapshot
// (docs/design/01-obs-store.md, "Visible-version queries").
//
// Visibility.  `as_of` is a receipt_seq (default: the newest).  For each
// report_key the visible row is the one with the largest receipt_seq <=
// as_of; every other version is superseded.  Visibility is decided from a
// per-bucket version map (sorted (report_key, receipt_seq) pairs of every
// row in the bucket), never from the candidate list alone: a correction can
// move a report, so its newer version may lie outside the candidates.
// Every version of a report shares obs_time, so all of them are in buckets
// that contain that obs_time (one bucket, or two overlapping ones after a
// bucket_seconds change).  Ingest guarantees that versions share obs_time
// (the report_key hash covers it, T11).
//
// Time window.  A query only sees reports with |obs_time - t| <= W, W =
// 3 * tau of the source class by default.  The window is a hard filter, so
// results do not depend on where bucket boundaries fall (bucket_seconds can
// change, Q5b).
//
// Metric.  Every tree a query searches uses the source class's current
// metric (L_km, tau_seconds, vertical_scale): a segment built with another
// metric gets its tree rebuilt in RAM from its columns (Q5b,
// 05-reference-data.md).  A segment whose bucket_seconds differs but whose
// metric is the same keeps its prebuilt tree: bucket length does not affect
// distances.  Each tree embeds time relative to its own bucket start, so
// distances from different trees are directly comparable.
//
// knn.  Each tree is asked for k' = 2k candidates; superseded and
// out-of-window rows are dropped.  Let B be the smallest last-returned d2
// over the trees that still have rows left.  Every row with d2 < B has then
// been seen, so once k visible candidates have d2 < B they are the answer;
// otherwise k' doubles.  Results are ordered by (d2, report_key).
//
// Not here: corridor queries (T12).
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "atmo/geo.h"
#include "atmo/kdtree.h"
#include "atmo/obs_segment.h"
#include "atmo/source.h"

namespace atmo
{

// as_of value that sees every version (the snapshot's newest).
constexpr std::uint64_t kObsLatest = std::numeric_limits<std::uint64_t>::max();

struct ObsQuery
{
  double lat_deg = 0.0;
  double lon_deg = 0.0;
  double height_m = 0.0;
  double t_s = 0.0;  // unix seconds
  std::uint64_t as_of = kObsLatest;
  // Half-width of the time window, seconds; NaN = 3 * tau_seconds.
  double window_s = std::numeric_limits<double>::quiet_NaN();
};

struct ObsHit
{
  float d2 = 0.0f;  // squared distance in the source class's metric (km^2)
  const ObsSegment* segment = nullptr;
  ReportView report;  // valid while the ObsIndex is alive
};

// Immutable once built; queries are thread-safe.
class ObsIndex
{
 public:
  // Every segment's source must be in `sources`, and no (report_key,
  // receipt_seq) may appear twice; otherwise std::invalid_argument.
  ObsIndex(
    const SourceRegistry& sources,
    std::vector<std::shared_ptr<const ObsSegment>> segments);
  ~ObsIndex();
  ObsIndex(ObsIndex&&) noexcept;
  ObsIndex& operator=(ObsIndex&&) noexcept;

  // Largest receipt_seq over all segments (0 if none).
  std::uint64_t max_receipt_seq() const { return max_seq_; }
  std::size_t segment_count() const { return n_segments_; }
  // Segments whose tree was rebuilt in RAM because the metric changed.
  std::size_t rebuilt_trees() const { return rebuilt_; }

  // The k nearest visible reports of `source`.  std::out_of_range if the
  // source class is unknown.
  std::vector<ObsHit>
  knn(std::string_view source, const ObsQuery& q, std::size_t k) const;

  // Every visible report with d2 <= r_km * r_km, ordered by (d2,
  // report_key).  r_km is in the embedded metric.
  std::vector<ObsHit>
  radius(std::string_view source, const ObsQuery& q, float r_km) const;

 private:
  struct Source;

  const Source& source(std::string_view name) const;

  std::vector<std::unique_ptr<Source>> sources_;
  std::uint64_t max_seq_ = 0;
  std::size_t n_segments_ = 0;
  std::size_t rebuilt_ = 0;
};

}  // namespace atmo
