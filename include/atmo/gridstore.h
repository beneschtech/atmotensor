// GridStore: one immutable file per grid definition, <dir>/<key>.grid.
//
// File layout (little-endian; POSIX):
//   [0, 4096)   header page: FileHeader at 0, canonical identity bytes at 128
//   lat_off     double[ni*nj]   4096-aligned
//   lon_off     double[ni*nj]   4096-aligned
//   rot_off     float [ni*nj]   4096-aligned
// Every section carries a CRC-32 in the header, and the header has its own.
//
// Files are never modified: put() writes a temp file, fsyncs it, renames it
// into place and fsyncs the directory, so a crash leaves either no file or a
// complete one.  put() of a grid that already exists is a no-op (after
// checking the stored identity and coordinates match).  get() mmaps the file
// read-only and returns a Grid whose arrays point straight into the mapping,
// so every field on the grid shares the same pages.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "atmo/grid.h"

namespace atmo
{

class GridStore
{
 public:
  explicit GridStore(std::filesystem::path dir);

  // Store a grid (idempotent) and return it, backed by the on-disk file.
  std::shared_ptr<const Grid> put(const GridIdentity& id, GridData data);

  // nullptr if absent; throws std::runtime_error if the file is corrupt.
  std::shared_ptr<const Grid> get(GridKey key) const;

  bool contains(GridKey key) const;
  // CRC of the stored file's header (what manifests record).  Reads and
  // checks only the header; std::runtime_error if absent or corrupt.
  std::uint32_t header_crc(GridKey key) const;
  std::vector<GridKey> list() const;
  std::filesystem::path path_for(GridKey key) const;

 private:
  void write_file(
    const std::filesystem::path& path,
    GridKey key,
    const GridIdentity& id,
    const GridData& d) const;
  std::shared_ptr<const Grid> load_locked(GridKey key) const;

  std::filesystem::path dir_;
  mutable std::mutex mu_;
  mutable std::map<GridKey, std::weak_ptr<const Grid>> cache_;
};

}  // namespace atmo
