// Observation segments (docs/design/01-obs-store.md): one immutable file per
// sealed batch of decoded reports of one source class and one time bucket.
//
// File layout (little-endian; POSIX).  Every section starts 4096-aligned and
// is CRC-32 protected; the header has its own CRC.  Inside a section each
// column starts 8-byte aligned (16 for tree points); padding is zero and is
// covered by the section CRC.  All offsets follow from the three counts, so
// the reader recomputes them and rejects any header that disagrees.
//
//   [0, 4096)  header page: SegHeader at 0, rest zero
//   reports    n_reports rows, struct-of-arrays:
//                report_key u64, receipt_seq u64, receipt_time_ms i64,
//                obs_time i64, lat f32, lon f32, height_m f32,
//                station_ref u32, flags u16, n_values u16, first_value u32,
//                raw_off u64, raw_len u32
//   values     n_values rows, contiguous per report:
//                var_code u16, value f32, qc u16, height_m f32,
//                dlat_deg f32, dlon_deg f32
//   raw        raw message bytes, stored once per report
//   tree       prebuilt KdTree4<float>: points float[4][n], ids u32[n],
//                axes u8[n]; ids are report row indices.  Points are
//                embed<float>(lat, lon, height_m, obs_time, embedding()).
//
// Every value carries a geometric height (metres above MSL, same meaning
// as the report's height_m), so vertical distance is measured in the same
// units for reports and profile levels (QUESTIONS.md Q3).  A sounding is
// one report whose values are its levels, each with its own height_m;
// for a single-level report every value has the report's height.  Heights
// are geometric, not geopotential: ingest converts reported geopotential
// heights, and derives heights for pressure-only levels (e.g. from a recent
// GRIB geopotential-height field), before calling add_report().
//
// Values also carry a horizontal offset (dlat_deg, dlon_deg) from the
// report's position, for balloon drift: a sounding's upper levels sit tens
// of km downwind of the launch site (Q3a).  0 for single-level reports.
//
// Numbers are f32 wherever the precision allows (docs/CONVENTIONS.md, for
// OpenCL): f32 lat/lon resolve ~1.7 m.  Times stay i64 (f32 cannot hold
// epoch seconds), ids and sequence numbers u64.
//
// Sealing: temp file in the target directory -> fsync -> link() to the final
// name (fails if it exists, so a sealed file is never replaced) -> unlink temp
// -> fsync directory.  A crash leaves no file or a complete one (plus maybe a
// stray *.tmp.* file for gc).  The segment is not *visible* until a manifest
// names it (02-manifest-and-processes.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atmo/geo.h"
#include "atmo/kdtree.h"
#include "atmo/source.h"

namespace atmo
{

// Report flags (bit 0, bit 1; the rest reserved).
constexpr std::uint16_t kObsCorrection = 1u << 0;  // COR / amendment
constexpr std::uint16_t kObsDecoderWarnings = 1u << 1;

struct ObsValue
{
  std::uint16_t var_code = 0;  // canonical variable registry (variables.md)
  float value = 0.0f;          // SI units, finite
  std::uint16_t qc = 0;        // 0 = unchecked/ok
  // Geometric height above MSL (m) of this value; NaN = the report's
  // height_m (single-level reports).  Stored finite on disk.
  float height_m = std::numeric_limits<float>::quiet_NaN();
  // Horizontal offset of this value from the report's (lat, lon), degrees;
  // 0 for single-level reports.
  float dlat_deg = 0.0f;
  float dlon_deg = 0.0f;
};

// One decoded report as handed to the writer.
struct ObsReport
{
  std::uint64_t report_key = 0;   // same for every version of a report
  std::uint64_t receipt_seq = 0;  // orders versions; latest = max
  std::int64_t receipt_time_ms = 0;
  std::int64_t obs_time_s = 0;  // unix seconds, inside the segment's bucket
  float lat_deg = 0.0f;         // position at obs_time_s
  float lon_deg = 0.0f;
  float height_m = 0.0f;
  std::uint32_t station_ref = 0;  // 0 = none / mobile
  std::uint16_t flags = 0;
  std::vector<ObsValue> values;  // at most 65535
  std::string raw;               // original message bytes
};

// Pointers to the value columns of a segment (or of one report's slice).
struct ValueColumns
{
  const std::uint16_t* var = nullptr;
  const float* value = nullptr;
  const std::uint16_t* qc = nullptr;
  const float* height_m = nullptr;
  const float* dlat_deg = nullptr;
  const float* dlon_deg = nullptr;
};

// The values of one report, pointing into the mapped file.
class ValuesView
{
 public:
  ValuesView() = default;
  // Rows [first, first + n) of `cols`.
  ValuesView(const ValueColumns& cols, std::size_t first, std::size_t n);

  std::size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  std::uint16_t var_code(std::size_t k) const { return c_.var[k]; }
  float value(std::size_t k) const { return c_.value[k]; }
  std::uint16_t qc(std::size_t k) const { return c_.qc[k]; }
  float height_m(std::size_t k) const { return c_.height_m[k]; }
  float dlat_deg(std::size_t k) const { return c_.dlat_deg[k]; }
  float dlon_deg(std::size_t k) const { return c_.dlon_deg[k]; }

 private:
  ValueColumns c_;
  std::size_t n_ = 0;
};

// One report row.  Spans stay valid while the ObsSegment is alive.
struct ReportView
{
  std::size_t row = 0;
  std::uint64_t report_key = 0;
  std::uint64_t receipt_seq = 0;
  std::int64_t receipt_time_ms = 0;
  std::int64_t obs_time_s = 0;
  float lat_deg = 0.0f;
  float lon_deg = 0.0f;
  float height_m = 0.0f;
  std::uint32_t station_ref = 0;
  std::uint16_t flags = 0;
  ValuesView values;
  std::string_view raw;
};

// What the manifest needs to know about a sealed segment.
struct SealedSegment
{
  std::filesystem::path path;
  std::uint64_t n_reports = 0;
  std::uint64_t n_values = 0;
  std::uint64_t raw_bytes = 0;
  std::uint64_t receipt_seq_min = 0;  // 0/0 for an empty segment
  std::uint64_t receipt_seq_max = 0;
  std::uint32_t header_crc = 0;
};

// On-disk layout (see the comment at the top of this file).
namespace obsseg
{

constexpr std::size_t kPage = 4096;
constexpr std::uint32_t kVersion = 1;
constexpr char kMagic[8] = {'A', 'T', 'O', 'B', 'S', 'G', '0', '1'};
constexpr std::size_t kSections = 4;  // reports, values, raw, tree
constexpr const char* kSectionNames[kSections] = {
  "reports", "values", "raw", "tree"};

struct SegSection
{
  std::uint64_t off;
  std::uint64_t len;
  std::uint32_t crc;
  std::uint32_t reserved;
};

struct SegHeader
{
  char magic[8];
  std::uint32_t version;
  std::uint32_t flags;  // reserved, 0
  char source[32];      // NUL-terminated source class name
  std::int64_t bucket_start_s;
  std::int64_t bucket_seconds;
  double L_km;
  double tau_seconds;
  double vertical_scale;
  double radius_km;
  std::uint64_t n_reports;
  std::uint64_t n_values;
  std::uint64_t raw_bytes;
  std::uint64_t receipt_seq_min;
  std::uint64_t receipt_seq_max;
  std::int64_t obs_time_min_s;
  std::int64_t obs_time_max_s;
  SegSection sec[kSections];
  std::uint32_t reserved;
  std::uint32_t crc_header;  // over every byte before this field
};
static_assert(sizeof(SegHeader) == 256, "SegHeader layout");
static_assert(SourceClass::kMaxName < sizeof(SegHeader{}.source), "name fits");

// Report columns, in file order.
enum RCol
{
  kKey,
  kSeq,
  kRecvTime,
  kObsTime,
  kLat,
  kLon,
  kHeight,
  kStation,
  kFlags,
  kNVal,
  kFirst,
  kRawOff,
  kRawLen,
  kNumRCols
};
constexpr std::size_t kRColSize[kNumRCols] = {8, 8, 8, 8, 4, 4, 4,
                                              4, 2, 2, 4, 8, 4};

// Value columns, in file order.
enum VCol
{
  kVar,
  kValue,
  kQc,
  kVHeight,
  kVDLat,
  kVDLon,
  kNumVCols
};
constexpr std::size_t kVColSize[kNumVCols] = {2, 4, 2, 4, 4, 4};

// Absolute file offsets of every section and column.
struct Layout
{
  std::size_t sec_off[kSections] = {};
  std::size_t sec_len[kSections] = {};
  std::size_t rcol[kNumRCols] = {};
  std::size_t vcol[kNumVCols] = {};
  std::size_t raw = 0, tree_pts = 0, tree_ids = 0, tree_axes = 0;
  std::size_t total = 0;

  // Counts must be < 2^32 (reports, values) and raw_bytes < 2^48, so that
  // nothing can overflow.
  Layout(std::uint64_t n_reports, std::uint64_t n_values, std::uint64_t raw);
};

}  // namespace obsseg

// Collects reports for one (source class, bucket) and seals them into a file.
class ObsSegmentWriter
{
 public:
  ObsSegmentWriter(const SourceClass& source, std::int64_t bucket_start_s);

  // Validates and appends; returns the report's row index.  On error
  // (std::invalid_argument) the writer is unchanged.
  std::size_t add_report(const ObsReport& r);

  std::size_t size() const { return key_.size(); }
  std::int64_t bucket_start_s() const { return bucket_start_s_; }
  const SourceClass& source() const { return source_; }

  // Writes the segment to `path` (creating parent directories).  Throws
  // std::runtime_error on I/O failure or if `path` already exists; the
  // writer can then seal to another path.  A writer seals exactly once.
  SealedSegment seal(const std::filesystem::path& path);

 private:
  // Frees the buffered rows once they are on disk.
  void release();

  SourceClass source_;
  std::int64_t bucket_start_s_ = 0;
  Embedding embedding_;
  bool sealed_ = false;

  std::set<std::pair<std::uint64_t, std::uint64_t>> versions_;
  std::vector<std::uint64_t> key_, seq_;
  std::vector<std::int64_t> receipt_time_ms_, obs_time_s_;
  std::vector<float> lat_deg_, lon_deg_;
  std::vector<float> height_m_;
  std::vector<std::uint32_t> station_;
  std::vector<std::uint16_t> flags_, nval_;
  std::vector<std::uint32_t> first_;
  std::vector<std::uint64_t> raw_off_;
  std::vector<std::uint32_t> raw_len_;
  std::vector<std::uint16_t> var_, qc_;
  std::vector<float> value_, value_height_m_, value_dlat_deg_, value_dlon_deg_;
  std::vector<char> raw_;
};

// A sealed segment, mmap'd read-only.  Immutable and thread-safe.
class ObsSegment
{
 public:
  using Tree = KdTreeView4<float>;

  enum class Verify
  {
    full,    // header + every section CRC (first open of a file)
    header,  // header CRC only (file already fully verified once)
  };

  struct SectionInfo
  {
    const char* name;
    std::size_t offset;
    std::size_t length;
    std::uint32_t crc;
  };

  // Throws std::runtime_error if the file is missing, truncated or corrupt.
  // Structural checks (slices, tree ids) always run, whatever `verify` says,
  // so no accessor can read outside the mapping.
  static std::shared_ptr<const ObsSegment> open(
    const std::filesystem::path& path,
    Verify verify = Verify::full);

  const std::filesystem::path& path() const { return path_; }
  std::string_view source_name() const { return h_.source; }
  std::int64_t bucket_start_s() const { return h_.bucket_start_s; }
  std::int64_t bucket_seconds() const { return h_.bucket_seconds; }
  double L_km() const { return h_.L_km; }
  double tau_seconds() const { return h_.tau_seconds; }
  double vertical_scale() const { return h_.vertical_scale; }
  const Embedding& embedding() const { return emb_; }

  // True if this segment's tree was built with `s`'s metric (name, bucket,
  // L, tau, vertical scale, Earth radius).  Retention does not matter.
  bool matches(const SourceClass& s) const;

  std::size_t size() const { return n_; }
  std::size_t n_values() const { return h_.n_values; }
  std::size_t raw_bytes() const { return h_.raw_bytes; }
  std::uint64_t receipt_seq_min() const { return h_.receipt_seq_min; }
  std::uint64_t receipt_seq_max() const { return h_.receipt_seq_max; }
  std::int64_t obs_time_min_s() const { return h_.obs_time_min_s; }
  std::int64_t obs_time_max_s() const { return h_.obs_time_max_s; }
  std::uint32_t header_crc() const { return h_.crc_header; }

  std::vector<SectionInfo> sections() const;

  // Columns, size() rows each.
  const std::uint64_t* report_keys() const { return key_; }
  const std::uint64_t* receipt_seqs() const { return seq_; }
  const std::int64_t* receipt_times_ms() const { return receipt_time_ms_; }
  const std::int64_t* obs_times_s() const { return obs_time_s_; }
  const float* lats_deg() const { return lat_deg_; }
  const float* lons_deg() const { return lon_deg_; }
  const ValueColumns& value_columns() const { return vcols_; }
  const float* heights_m() const { return height_m_; }
  const std::uint32_t* station_refs() const { return station_; }
  const std::uint16_t* flags() const { return flags_; }

  // std::out_of_range if i >= size().
  ReportView report(std::size_t i) const;

  // The prebuilt tree, straight from the mapping.  Hit ids are row indices.
  const Tree& tree() const { return tree_; }

  Tree::Point query_point(
    double lat_deg,
    double lon_deg,
    double height_m,
    double t_s) const;
  std::vector<Tree::Hit> knn(
    double lat_deg,
    double lon_deg,
    double height_m,
    double t_s,
    std::size_t k) const;
  // r_km is in the embedded metric (km, with time weighted by lambda).
  std::vector<Tree::Hit> radius(
    double lat_deg,
    double lon_deg,
    double height_m,
    double t_s,
    float r_km) const;

 private:
  ObsSegment(const std::filesystem::path& path, Verify verify);

  std::filesystem::path path_;
  std::shared_ptr<const void> map_;  // keeps the mapping alive
  const unsigned char* base_ = nullptr;
  obsseg::SegHeader h_{};
  obsseg::Layout layout_{0, 0, 0};
  Embedding emb_;
  std::size_t n_ = 0;
  Tree tree_;

  // Column pointers into the mapping, set once by the constructor.
  const std::uint64_t* key_ = nullptr;
  const std::uint64_t* seq_ = nullptr;
  const std::int64_t* receipt_time_ms_ = nullptr;
  const std::int64_t* obs_time_s_ = nullptr;
  const float* lat_deg_ = nullptr;
  const float* lon_deg_ = nullptr;
  const float* height_m_ = nullptr;
  const std::uint32_t* station_ = nullptr;
  const std::uint16_t* flags_ = nullptr;
  const std::uint16_t* nval_ = nullptr;
  const std::uint32_t* first_ = nullptr;
  const std::uint64_t* raw_off_ = nullptr;
  const std::uint32_t* raw_len_ = nullptr;
  ValueColumns vcols_;
  const char* raw_ = nullptr;
};

}  // namespace atmo
