#include "atmo/snapshot.h"

#include "atmo/crc32.h"
#include "atmo/gridstore.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace atmo
{
namespace
{

namespace fs = std::filesystem;

[[noreturn]] void mismatch(const std::string& path, const char* what)
{
  throw std::runtime_error(
    "snapshot: " + path + ": file does not match its manifest entry (" + what +
    ")");
}

}  // namespace

Snapshot::Snapshot(const fs::path& root, Manifest m, const Snapshot* previous)
    : root_(root), manifest_(std::move(m))
{
  // Stations.
  if (manifest_.stations)
  {
    const StationsEntry& e = *manifest_.stations;
    if (previous && previous->manifest_.stations == e && previous->stations_)
    {
      stations_ = previous->stations_;
    }
    else
    {
      stations_ = StationTable::open(root / e.path, StationTable::Verify::full);
      ++opened_;
      if (stations_->generation() != e.generation)
      {
        mismatch(e.path, "generation");
      }
      if (stations_->header_crc() != e.header_crc)
      {
        mismatch(e.path, "header CRC");
      }
    }
  }

  // Grids.  GridStore is only constructed when needed: its constructor
  // creates the directory, which a read-only query daemon cannot do.
  std::map<GridKey, const GridEntry*> prev_grids;
  if (previous)
  {
    for (const auto& g : previous->manifest_.grids)
    {
      prev_grids[g.key] = &g;
    }
  }
  std::unique_ptr<GridStore> store;
  for (const auto& e : manifest_.grids)
  {
    auto it = prev_grids.find(e.key);
    if (it != prev_grids.end() && *it->second == e)
    {
      grids_[e.key] = previous->grids_.at(e.key);
      continue;
    }
    if (!store)
    {
      store = std::make_unique<GridStore>(root / "grids");
    }
    if (store->header_crc(e.key) != e.header_crc)
    {
      mismatch(e.path, "header CRC");
    }
    auto g = store->get(e.key);
    if (!g)
    {
      throw std::runtime_error("snapshot: " + e.path + ": missing");
    }
    grids_[e.key] = std::move(g);
    ++opened_;
  }

  // Field files, checked against their entry and their grid.
  std::map<std::string, std::pair<const FieldEntry*, FieldPtr>> prev_fields;
  if (previous)
  {
    for (const auto& f : previous->manifest_.fields)
    {
      prev_fields[f.path] = {
        &f, previous->fields_.at({f.model, f.run_s, f.fhr})};
    }
  }
  for (const auto& e : manifest_.fields)
  {
    FieldPtr ff;
    auto it = prev_fields.find(e.path);
    if (
      it != prev_fields.end() && *it->second.first == e &&
      grids_.at(e.grid_key) == previous->grids_.at(e.grid_key))
    {
      ff = it->second.second;
    }
    else
    {
      ff = FieldFile::open(root / e.path);
      ++opened_;
      if (FieldEntry::from_file(root, *ff) != e)
      {
        mismatch(e.path, "model, run, fhr, grid or header CRC");
      }
      ff->require_grid(*grids_.at(e.grid_key));
    }
    fields_[{e.model, e.run_s, e.fhr}] = std::move(ff);
  }

  // GRIB indexes (T10c): the GRIB files themselves are read on demand.
  std::map<std::string, std::pair<const GribEntry*, GribIndexPtr>> prev_gribs;
  if (previous)
  {
    for (std::size_t i = 0; i < previous->manifest_.gribs.size(); ++i)
    {
      prev_gribs[previous->manifest_.gribs[i].path] = {
        &previous->manifest_.gribs[i], previous->grib_indexes_[i]};
    }
  }
  for (const auto& e : manifest_.gribs)
  {
    auto it = prev_gribs.find(e.path);
    if (it != prev_gribs.end() && *it->second.first == e)
    {
      grib_indexes_.push_back(it->second.second);
      continue;
    }
    std::string text;
    {
      std::ifstream in(root / e.index_path, std::ios::binary);
      if (!in)
      {
        throw std::runtime_error("snapshot: " + e.index_path + ": missing");
      }
      std::ostringstream ss;
      ss << in.rdbuf();
      text = ss.str();
    }
    ++opened_;
    if (crc32(text.data(), text.size()) != e.index_crc)
    {
      mismatch(e.index_path, "index CRC");
    }
    GribIndexPtr idx;
    try
    {
      idx = std::make_shared<const GribIndex>(GribIndex::from_text(text));
    }
    catch (const std::invalid_argument& ex)
    {
      throw std::runtime_error("snapshot: " + e.index_path + ": " + ex.what());
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(root / e.path, ec);
    if (ec || size != e.size || idx->size != e.size)
    {
      mismatch(e.path, "GRIB file size");
    }
    if (idx->model != e.model || idx->run_s != e.run_s || idx->grib != e.path)
    {
      mismatch(e.index_path, "model, run or file name");
    }
    for (const auto& msg : idx->entries)
    {
      if (std::find(e.grids.begin(), e.grids.end(), msg.grid) == e.grids.end())
      {
        mismatch(e.index_path, "message on a grid the entry does not list");
      }
    }
    grib_indexes_.push_back(std::move(idx));
  }

  // Segments.
  std::map<std::string, std::pair<const SegmentEntry*, SegPtr>> prev_segs;
  if (previous)
  {
    const auto& pm = previous->manifest_.segments;
    for (std::size_t i = 0; i < pm.size(); ++i)
    {
      prev_segs[pm[i].path] = {&pm[i], previous->segments_[i]};
    }
  }
  segments_.reserve(manifest_.segments.size());
  for (const auto& e : manifest_.segments)
  {
    auto it = prev_segs.find(e.path);
    if (it != prev_segs.end() && *it->second.first == e)
    {
      segments_.push_back(it->second.second);
      continue;
    }
    SegPtr seg = ObsSegment::open(root / e.path, ObsSegment::Verify::full);
    ++opened_;
    if (SegmentEntry::from_segment(root, *seg) != e)
    {
      mismatch(e.path, "source, bucket, receipt seqs, count or header CRC");
    }
    segments_.push_back(std::move(seg));
  }

  try
  {
    obs_ = std::make_unique<ObsIndex>(manifest_.sources, segments_);
  }
  catch (const std::invalid_argument& e)
  {
    throw std::runtime_error(
      "snapshot " + std::to_string(manifest_.seq) + ": " + e.what());
  }
}

Snapshot::~Snapshot() = default;

Snapshot::FieldPtr Snapshot::field_file(
  std::string_view model,
  std::int64_t run_s,
  std::uint32_t fhr) const
{
  auto it = fields_.find({std::string(model), run_s, fhr});
  return it == fields_.end() ? nullptr : it->second;
}

Snapshot::GridPtr Snapshot::grid(GridKey key) const
{
  auto it = grids_.find(key);
  return it == grids_.end() ? nullptr : it->second;
}

SnapshotLoader::SnapshotLoader(fs::path root) : root_(std::move(root))
{
}

bool SnapshotLoader::refresh()
{
  std::lock_guard<std::mutex> lk(refresh_mu_);
  std::shared_ptr<const Snapshot> cur = cur_.load();
  const auto seq = read_current_seq(root_);
  if (!seq || (cur && *seq <= cur->seq()))
  {
    return false;
  }
  auto m = read_current_manifest(root_);
  if (!m || (cur && m->seq <= cur->seq()))
  {
    return false;
  }
  cur_.store(std::make_shared<const Snapshot>(root_, std::move(*m), cur.get()));
  return true;
}

std::shared_ptr<const Snapshot> SnapshotLoader::current() const
{
  return cur_.load();
}

}  // namespace atmo
