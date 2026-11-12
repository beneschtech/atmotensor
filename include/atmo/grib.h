// GRIB2 ingest via eccodes (T11; 03-field-tiles.md, CLAUDE.md "Grids").
// Only built when eccodes is found (ATMO_HAVE_ECCODES; CMake option
// ATMO_WITH_ECCODES=AUTO|ON|OFF).  This header does not include eccodes.h.
//
// What a message becomes:
//   * grid: identity = the message's GRIB2 section 3 bytes + Ni, Nj +
//     periodic_i (regular_ll / regular_gg spanning 360 degrees); points and
//     values normalised from any scanning mode to index = j*ni + i with +i
//     along the grid's x axis and +j along its y axis;
//   * field: param_id from the variable registry by (discipline, category,
//     number), values converted to the registry's units (scale, offset),
//     missing (bitmap) -> NaN; level = first fixed surface (code table 4.5
//     type, value in SI); grid-relative u/v (uvRelativeToGrid) flagged
//     kFieldGridRelative;
//   * time: run = reference time (dataDate/dataTime), fhr = end of the
//     forecast step in whole hours.
//
// Messages that cannot be used are skipped with a reason, not errors:
// not GRIB2, unsupported grid type (reduced, spectral, unstructured),
// product template other than 4.0 (instantaneous) and 4.8 (statistically
// processed), a 4.8 interval that does not start at the run time (only
// accumulations/extremes since run start are kept, Q9), sub-hourly steps,
// a parameter that is not in the registry or not selected, a second
// message for a field already taken, a grid other than the first one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "atmo/catalog.h"
#include "atmo/fieldfile.h"
#include "atmo/gribstore.h"
#include "atmo/grid.h"
#include "atmo/manifest.h"

namespace atmo
{

// A grid as decoded from section 3, shared by every message on it.
struct GribGrid
{
  GridIdentity id;
  GridData data;  // normalised order
};

struct GribMessage
{
  std::shared_ptr<const GribGrid> grid;
  std::vector<float> values;  // normalised order, registry units, NaN missing
  FieldInfo info;             // min/max not set
  std::int64_t run_s = 0;
  std::uint32_t fhr = 0;
};

struct GribSkip
{
  std::size_t message = 0;  // 0-based index in its file
  std::string file;
  std::string reason;
};

// Reads the messages of one GRIB file in order.  Grids are decoded once per
// distinct section 3 and shared.
class GribReader
{
 public:
  // std::runtime_error if the file cannot be opened.
  explicit GribReader(const std::filesystem::path& path);
  ~GribReader();
  GribReader(const GribReader&) = delete;
  GribReader& operator=(const GribReader&) = delete;

  // Only these variables (registry codes) are decoded; empty = all.
  void select(std::set<std::uint16_t> variables);

  // The next usable message, or nullopt at the end.  std::runtime_error on
  // a corrupt message.
  std::optional<GribMessage> next();

  // Messages read so far, usable or not.
  std::size_t messages_read() const { return index_; }
  const std::vector<GribSkip>& skipped() const { return skipped_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::size_t index_ = 0;
  std::vector<GribSkip> skipped_;
};

struct GribIngestOptions
{
  std::string model;                  // field file model name
  std::set<std::uint16_t> variables;  // empty = every registered one
  std::uint32_t tile = FieldFileWriter::kDefaultTile;
  std::uint32_t halo = FieldFileWriter::kDefaultHalo;
};

struct GribIngestResult
{
  GridEntry grid;
  FieldEntry field;
  std::size_t messages = 0;  // read, over all files
  std::size_t fields = 0;    // written to the field file
  std::vector<GribSkip> skipped;
};

// Decodes GRIB2 files of ONE model run and forecast hour on one grid (e.g.
// HRRR wrfsfc + wrfprs) into the GridStore under root/grids (idempotent)
// and one field file root/FieldFile::relative_path(model, run, fhr).
// Returns the manifest entries; publishing is the caller's job.
// std::runtime_error if a file is unreadable or corrupt, messages disagree
// on run or forecast hour, nothing usable was found, or the field file
// already exists.  std::invalid_argument for bad options.
GribIngestResult ingest_grib(
  const std::filesystem::path& root,
  const std::vector<std::filesystem::path>& files,
  const GribIngestOptions& options);

// Adds the result's grid (unless listed) and field file to `m`.
// std::invalid_argument if `m` already lists that field file.
void add_to_manifest(Manifest& m, const GribIngestResult& r);

// ---- Grid catalog (T16, D13; catalog.h) ------------------------------------

// Keeps a GRIB2 file as published (hard link into
// root/grib/<model>/<earliest base time YYYYmmddHH>/<name>; an identical
// file already there is reused, a different one is an error), describes
// EVERY message (gribsig.h: signature, time, level, member) and stores
// each supported grid in the GridStore.  Nothing is skipped for want of a
// name: unknown parameters, other templates, sub-hourly steps and
// statistical intervals of any kind are all returned; only non-GRIB2
// messages are listed in `skipped`.  Unsupported grids and undecodable
// templates are returned with flags (catalog.h).  Writes no sidecar and no
// manifest entry: the result goes to Catalog::ingest().
// std::runtime_error if the file is unreadable or truncated, or holds no
// GRIB2 message.  std::invalid_argument for a bad model name.
CatalogFile catalog_grib(
  const std::filesystem::path& root,
  const std::filesystem::path& source,
  const std::string& model);

// Decoder for GribCache: reads the message at its offset, checks it still
// hashes to the indexed value, decodes values into registry units and
// normalised order.
GribDecoder grib_decoder();

// Every value of one GRIB2 message in the message's own point order (no
// scan-mode normalisation, any grid type), native units, NaN where the
// bitmap says missing (/v1/grib/messages/{id}/values, Q21g).
// std::runtime_error if eccodes cannot decode it.
std::vector<double> grib_values(const void* msg, std::size_t len);

}  // namespace atmo
