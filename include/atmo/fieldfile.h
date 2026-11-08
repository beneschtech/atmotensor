// Gridded field files (docs/design/03-field-tiles.md): one immutable file
// per (model, run, forecast hour), fields/<model>/<run YYYYmmddHH>/
// f<fhr:03>.fld, holding the configured fields of that GRIB file on one grid
// as float32 tiles.
//
// Tiles.  A tile covers T x T grid points (default 256) plus a halo of H
// points on every side (default 1), stored as (T+2H)^2 float32 values, row
// major, i fastest, in a 4 KiB-aligned slot.  Tile (tx, ty) holds the
// points i in [tx*T - H, tx*T + T + H), j likewise.  Points outside the grid
// are NaN; with periodic_i the halo wraps across the seam.  So the four
// corners of any cell (i, j) lie inside the tile of point (i, j), and
// bilinear sampling never needs a second tile.
//
// File layout (little-endian; same conventions as gridstore.h):
//   [0, 4096)      header page: FieldHeader at 0
//   [4096, ...)    tile slots, field-major then row-major (ty, tx); slot
//                  (f, t) at 4096 + (f * tiles + t) * slot_bytes
//   directory      n_fields FieldDirEntry, after the slots
//   tile tables    per field, tiles_x * tiles_y FieldTileEntry (offset,
//                  CRC-32), 8-aligned after the directory
// The directory and tables come after the slots (not before, as first
// sketched in the spec) so the writer can stream tiles to disk field by
// field without knowing the field count up front; the reader recomputes
// every offset from the counts either way.  The header has its own CRC,
// directory + tables one CRC together, and each tile one CRC, checked the
// first time the tile is touched.
//
// Values are SI units (ingest converts).  Winds are stored as published:
// kFieldGridRelative marks grid-relative u/v, which the query layer turns
// into earth-relative with rotate_to_earth() and the grid's rotation.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atmo/grid.h"

namespace atmo
{

namespace detail
{
class TempFile;
}

// FieldInfo::flags
constexpr std::uint8_t kFieldGridRelative = 1u << 0;  // u/v along +i/+j

struct FieldInfo
{
  std::uint16_t param_id = 0;   // variable code (registry, T8b)
  std::uint8_t level_type = 0;  // GRIB2 code table 4.5 (1 surface, 100
                                // isobaric, 103 height above ground, ...)
  std::uint8_t flags = 0;       // kField*
  float level_value = 0.0f;     // SI (Pa for isobaric, m for heights)
  // Filled by the writer: range of the finite values (NaN if none).
  float min = 0.0f;
  float max = 0.0f;

  bool same_field(const FieldInfo& o) const;
};

namespace fieldfile
{

constexpr char kMagic[8] = {'A', 'T', 'F', 'L', 'D', '0', '0', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kPage = 4096;

struct FieldHeader
{
  char magic[8];
  std::uint32_t version;
  std::uint32_t flags;  // bit 0: periodic_i
  char model[32];       // NUL-terminated
  std::int64_t run_s;
  std::uint32_t fhr;
  std::uint32_t ni, nj;
  std::uint32_t tile;  // T
  std::uint32_t halo;  // H
  std::uint32_t tiles_x, tiles_y;
  std::uint32_t n_fields;
  std::uint64_t grid_key;
  std::uint64_t slot_bytes;
  std::uint64_t dir_off;
  std::uint64_t tables_off;
  std::uint64_t total_size;
  std::uint32_t crc_meta;    // directory + tile tables
  std::uint32_t crc_header;  // over every byte before this field
};
static_assert(sizeof(FieldHeader) == 136, "FieldHeader layout");

struct FieldDirEntry
{
  std::uint16_t param_id;
  std::uint8_t level_type;
  std::uint8_t packing;  // 0 = float32 (1 = float16, reserved)
  std::uint8_t flags;
  std::uint8_t pad[3];
  float level_value;
  float min;
  float max;
  std::uint32_t pad2;
  std::uint64_t tile_table_off;
};
static_assert(sizeof(FieldDirEntry) == 32, "FieldDirEntry layout");

struct FieldTileEntry
{
  std::uint64_t offset;
  std::uint32_t crc;
  std::uint32_t pad;
};
static_assert(sizeof(FieldTileEntry) == 16, "FieldTileEntry layout");

}  // namespace fieldfile

// Writes one field file.  Tiles go to a temporary file next to the final
// path as each field is added; seal() writes the directory, tables and
// header and publishes the file (temp + fsync + link + dir fsync).
class FieldFileWriter
{
 public:
  static constexpr std::uint32_t kDefaultTile = 256;
  static constexpr std::uint32_t kDefaultHalo = 1;

  // `model` follows the source-class name rule ([a-z][a-z0-9_-]*, <= 31).
  // tile >= 2, 1 <= halo < tile.  std::invalid_argument otherwise.
  FieldFileWriter(
    const std::filesystem::path& path,
    const Grid& grid,
    std::string_view model,
    std::int64_t run_s,
    std::uint32_t fhr,
    std::uint32_t tile = kDefaultTile,
    std::uint32_t halo = kDefaultHalo);
  ~FieldFileWriter();
  FieldFileWriter(const FieldFileWriter&) = delete;
  FieldFileWriter& operator=(const FieldFileWriter&) = delete;

  // `values` has ni * nj entries in grid order (index = j*ni + i).  NaN =
  // missing.  std::invalid_argument for a wrong size or a field already
  // added (same param_id, level_type, level_value); `info.min/max` are
  // ignored and computed.  Returns the field's index.
  std::size_t add_field(FieldInfo info, std::span<const float> values);

  std::size_t size() const { return fields_.size(); }

  // Publishes the file; std::runtime_error if the path exists or on I/O
  // failure.  A writer seals once.
  void seal();

 private:
  std::filesystem::path path_;
  std::unique_ptr<detail::TempFile> file_;
  fieldfile::FieldHeader h_{};
  bool periodic_ = false;
  bool sealed_ = false;
  std::vector<FieldInfo> fields_;
  std::vector<fieldfile::FieldTileEntry> tiles_;  // all fields, in order
};

// A sealed field file, mmap'd read-only.  Thread-safe.
class FieldFile
{
 public:
  // "fields/<model>/<run YYYYmmddHH>/f<fhr:03>.fld" (relative to the root).
  static std::string
  relative_path(std::string_view model, std::int64_t run_s, std::uint32_t fhr);

  // Maps the file and checks header, directory and tables (CRCs and
  // structure).  std::runtime_error if missing, truncated or corrupt.
  static std::shared_ptr<const FieldFile> open(
    const std::filesystem::path& path);
  ~FieldFile();

  const std::filesystem::path& path() const { return path_; }
  std::string_view model() const { return h_.model; }
  std::int64_t run_s() const { return h_.run_s; }
  std::uint32_t fhr() const { return h_.fhr; }
  GridKey grid_key() const { return h_.grid_key; }
  std::uint32_t ni() const { return h_.ni; }
  std::uint32_t nj() const { return h_.nj; }
  bool periodic_i() const { return (h_.flags & 1u) != 0; }
  std::uint32_t tile_size() const { return h_.tile; }
  std::uint32_t halo() const { return h_.halo; }
  std::uint32_t tiles_x() const { return h_.tiles_x; }
  std::uint32_t tiles_y() const { return h_.tiles_y; }
  std::uint32_t header_crc() const { return h_.crc_header; }

  std::size_t size() const { return fields_.size(); }
  const FieldInfo& field(std::size_t k) const { return fields_[k]; }
  std::optional<std::size_t> find(
    std::uint16_t param_id,
    std::uint8_t level_type,
    float level_value) const;

  // std::runtime_error unless `grid` is the file's grid (key, ni, nj,
  // periodic_i).
  void require_grid(const Grid& grid) const;

  // (T+2H)^2 values of a tile, its CRC checked on first touch
  // (std::runtime_error if it does not match).  std::out_of_range for a
  // bad field or tile index.
  std::span<const float>
  tile(std::size_t field, std::uint32_t tx, std::uint32_t ty) const;

  // The value at grid point (i, j).
  float value(std::size_t field, std::uint32_t i, std::uint32_t j) const;

  // Bilinear interpolation inside `cell` (from Grid::locate on this file's
  // grid).  NaN if any corner is missing.  std::out_of_range if the cell
  // does not exist on the grid.
  float sample(std::size_t field, const Grid::Cell& cell) const;

 private:
  FieldFile() = default;

  // Tile values with the CRC check done.
  const float* tile_ptr(std::size_t field, std::size_t t) const;

  std::filesystem::path path_;
  std::shared_ptr<const void> map_;
  const unsigned char* base_ = nullptr;
  fieldfile::FieldHeader h_{};
  std::vector<FieldInfo> fields_;
  const fieldfile::FieldTileEntry* tiles_ = nullptr;
  std::unique_ptr<std::atomic<std::uint8_t>[]> verified_;
};

}  // namespace atmo
