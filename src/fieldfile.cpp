#include "atmo/fieldfile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>

#include "atmo/crc32.h"
#include "atmo/source.h"
#include "fileio.h"

namespace atmo
{
namespace
{

namespace fs = std::filesystem;
using namespace fieldfile;

constexpr const char* kWho = "FieldFile";
constexpr std::uint8_t kKnownFieldFlags = kFieldGridRelative;
constexpr std::uint64_t kMaxU64 = std::numeric_limits<std::uint64_t>::max();

std::uint64_t width(std::uint32_t tile, std::uint32_t halo)
{
  return static_cast<std::uint64_t>(tile) + 2u * halo;
}

std::uint64_t slot_bytes_for(std::uint32_t tile, std::uint32_t halo)
{
  const std::uint64_t w = width(tile, halo);
  return detail::align_up(w * w * sizeof(float), kPage);
}

std::uint32_t tiles_for(std::uint32_t n, std::uint32_t tile)
{
  return (n + tile - 1) / tile;
}

// a * b, or nullopt on overflow.
std::optional<std::uint64_t> mul(std::uint64_t a, std::uint64_t b)
{
  if (a != 0 && b > kMaxU64 / a)
  {
    return std::nullopt;
  }
  return a * b;
}

}  // namespace

bool FieldInfo::same_field(const FieldInfo& o) const
{
  return param_id == o.param_id && level_type == o.level_type &&
         level_value == o.level_value;
}

// ---------------------------------------------------------------------------
// FieldFileWriter
// ---------------------------------------------------------------------------

FieldFileWriter::FieldFileWriter(
  const fs::path& path,
  const Grid& grid,
  std::string_view model,
  std::int64_t run_s,
  std::uint32_t fhr,
  std::uint32_t tile,
  std::uint32_t halo)
    : path_(path), periodic_(grid.periodic_i())
{
  if (!SourceClass::valid_name(model))
  {
    throw std::invalid_argument(
      std::string(kWho) + ": bad model name \"" + std::string(model) + "\"");
  }
  if (tile < 2 || tile > 65536 || halo < 1 || halo >= tile)
  {
    throw std::invalid_argument(
      std::string(kWho) + ": need 2 <= tile <= 65536 and 1 <= halo < tile");
  }
  std::memcpy(h_.magic, kMagic, sizeof h_.magic);
  h_.version = kVersion;
  h_.flags = periodic_ ? 1u : 0u;
  std::memcpy(h_.model, model.data(), model.size());
  h_.run_s = run_s;
  h_.fhr = fhr;
  h_.ni = grid.ni();
  h_.nj = grid.nj();
  h_.tile = tile;
  h_.halo = halo;
  h_.tiles_x = tiles_for(h_.ni, tile);
  h_.tiles_y = tiles_for(h_.nj, tile);
  h_.grid_key = grid.key();
  h_.slot_bytes = slot_bytes_for(tile, halo);
  file_ = std::make_unique<detail::TempFile>(path_, kWho);
}

FieldFileWriter::~FieldFileWriter() = default;

std::size_t FieldFileWriter::add_field(
  FieldInfo info,
  std::span<const float> values)
{
  if (sealed_)
  {
    throw std::logic_error(std::string(kWho) + ": writer already sealed");
  }
  const std::size_t ni = h_.ni, nj = h_.nj;
  if (values.size() != ni * nj)
  {
    throw std::invalid_argument(
      std::string(kWho) + ": field has " + std::to_string(values.size()) +
      " values, grid has " + std::to_string(ni * nj));
  }
  if ((info.flags & ~kKnownFieldFlags) != 0)
  {
    throw std::invalid_argument(std::string(kWho) + ": unknown field flags");
  }
  if (!std::isfinite(info.level_value))
  {
    throw std::invalid_argument(std::string(kWho) + ": level not finite");
  }
  for (const auto& f : fields_)
  {
    if (f.same_field(info))
    {
      throw std::invalid_argument(
        std::string(kWho) + ": field " + std::to_string(info.param_id) +
        " at that level added twice");
    }
  }
  info.min = std::numeric_limits<float>::quiet_NaN();
  info.max = std::numeric_limits<float>::quiet_NaN();
  for (float v : values)
  {
    if (std::isfinite(v))
    {
      info.min = std::isnan(info.min) ? v : std::min(info.min, v);
      info.max = std::isnan(info.max) ? v : std::max(info.max, v);
    }
  }

  const std::int64_t tile = h_.tile, halo = h_.halo;
  const std::size_t w = static_cast<std::size_t>(width(h_.tile, h_.halo));
  const std::size_t ntiles = static_cast<std::size_t>(h_.tiles_x) * h_.tiles_y;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  std::vector<float> buf(w * w);
  const std::size_t f = fields_.size();
  for (std::uint32_t ty = 0; ty < h_.tiles_y; ++ty)
  {
    for (std::uint32_t tx = 0; tx < h_.tiles_x; ++tx)
    {
      for (std::size_t lj = 0; lj < w; ++lj)
      {
        const std::int64_t j = ty * tile + static_cast<std::int64_t>(lj) - halo;
        for (std::size_t li = 0; li < w; ++li)
        {
          std::int64_t i = tx * tile + static_cast<std::int64_t>(li) - halo;
          float v = nan;
          if (j >= 0 && j < static_cast<std::int64_t>(nj))
          {
            if (periodic_)
            {
              const std::int64_t n = static_cast<std::int64_t>(ni);
              i = ((i % n) + n) % n;
            }
            if (i >= 0 && i < static_cast<std::int64_t>(ni))
            {
              v = values
                [static_cast<std::size_t>(j) * ni +
                 static_cast<std::size_t>(i)];
            }
          }
          buf[lj * w + li] = v;
        }
      }
      const std::size_t t = static_cast<std::size_t>(ty) * h_.tiles_x + tx;
      const std::uint64_t off = kPage + (f * ntiles + t) * h_.slot_bytes;
      const std::size_t n = buf.size() * sizeof(float);
      file_->write(buf.data(), n, static_cast<std::size_t>(off));
      tiles_.push_back(FieldTileEntry{off, crc32(buf.data(), n), 0});
    }
  }
  fields_.push_back(info);
  return f;
}

void FieldFileWriter::seal()
{
  if (sealed_)
  {
    throw std::logic_error(std::string(kWho) + ": writer already sealed");
  }
  const std::uint64_t n = fields_.size();
  const std::uint64_t ntiles =
    static_cast<std::uint64_t>(h_.tiles_x) * h_.tiles_y;
  h_.n_fields = static_cast<std::uint32_t>(n);
  h_.dir_off = kPage + n * ntiles * h_.slot_bytes;
  h_.tables_off = h_.dir_off + n * sizeof(FieldDirEntry);
  h_.total_size = h_.tables_off + n * ntiles * sizeof(FieldTileEntry);

  std::vector<unsigned char> meta(h_.total_size - h_.dir_off);
  for (std::size_t f = 0; f < n; ++f)
  {
    FieldDirEntry d{};
    d.param_id = fields_[f].param_id;
    d.level_type = fields_[f].level_type;
    d.packing = 0;
    d.flags = fields_[f].flags;
    d.level_value = fields_[f].level_value;
    d.min = fields_[f].min;
    d.max = fields_[f].max;
    d.tile_table_off = h_.tables_off + f * ntiles * sizeof(FieldTileEntry);
    std::memcpy(meta.data() + f * sizeof d, &d, sizeof d);
  }
  if (!tiles_.empty())
  {
    std::memcpy(
      meta.data() + n * sizeof(FieldDirEntry), tiles_.data(),
      tiles_.size() * sizeof(FieldTileEntry));
  }
  h_.crc_meta = crc32(meta.data(), meta.size());
  h_.crc_header = crc32(&h_, offsetof(FieldHeader, crc_header));

  std::vector<unsigned char> page(kPage, 0);
  std::memcpy(page.data(), &h_, sizeof h_);
  if (!meta.empty())
  {
    file_->write(
      meta.data(), meta.size(), static_cast<std::size_t>(h_.dir_off));
  }
  file_->write(page.data(), page.size(), 0);
  file_->publish();
  sealed_ = true;
  file_.reset();
  tiles_.clear();
}

// ---------------------------------------------------------------------------
// FieldFile
// ---------------------------------------------------------------------------

std::string FieldFile::relative_path(
  std::string_view model,
  std::int64_t run_s,
  std::uint32_t fhr)
{
  const std::time_t t = static_cast<std::time_t>(run_s);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char run[32];
  std::strftime(run, sizeof run, "%Y%m%d%H", &tm);
  char f[16];
  std::snprintf(f, sizeof f, "f%03u.fld", fhr);
  return "fields/" + std::string(model) + "/" + run + "/" + f;
}

std::shared_ptr<const FieldFile> FieldFile::open(const fs::path& path)
{
  auto bad = [&](const std::string& why)
  {
    return std::runtime_error(
      std::string(kWho) + ": " + path.string() + ": " + why);
  };

  auto map = detail::MappedFile::open(path, kPage, kWho);
  std::shared_ptr<FieldFile> ff(new FieldFile());
  ff->path_ = path;
  ff->base_ = map->data();
  FieldHeader& h = ff->h_;
  std::memcpy(&h, map->data(), sizeof h);
  if (std::memcmp(h.magic, kMagic, sizeof h.magic) != 0)
  {
    throw bad("bad magic");
  }
  if (crc32(&h, offsetof(FieldHeader, crc_header)) != h.crc_header)
  {
    throw bad("header checksum mismatch");
  }
  if (h.version != kVersion)
  {
    throw bad("unsupported version");
  }
  if (
    std::memchr(h.model, 0, sizeof h.model) == nullptr ||
    !SourceClass::valid_name(h.model))
  {
    throw bad("bad model name");
  }
  if (
    (h.flags & ~1u) != 0 || h.ni == 0 || h.nj == 0 || h.tile < 2 ||
    h.tile > 65536 || h.halo < 1 || h.halo >= h.tile ||
    h.tiles_x != tiles_for(h.ni, h.tile) ||
    h.tiles_y != tiles_for(h.nj, h.tile) ||
    h.slot_bytes != slot_bytes_for(h.tile, h.halo))
  {
    throw bad("bad grid or tile geometry");
  }

  // Every offset recomputed from the counts, with overflow checks.
  const std::uint64_t n = h.n_fields;
  const std::uint64_t ntiles =
    static_cast<std::uint64_t>(h.tiles_x) * h.tiles_y;
  const auto slots = mul(n, ntiles);
  const auto slot_area = slots ? mul(*slots, h.slot_bytes) : std::nullopt;
  if (!slot_area || *slot_area > map->size())
  {
    throw bad("field count does not fit the file (truncated?)");
  }
  const std::uint64_t dir_off = kPage + *slot_area;
  const std::uint64_t tables_off = dir_off + n * sizeof(FieldDirEntry);
  const std::uint64_t total = tables_off + *slots * sizeof(FieldTileEntry);
  if (
    h.dir_off != dir_off || h.tables_off != tables_off || h.total_size != total)
  {
    throw bad("bad section offsets");
  }
  if (map->size() != total)
  {
    throw bad("file size does not match header (truncated?)");
  }
  if (crc32(map->data() + dir_off, total - dir_off) != h.crc_meta)
  {
    throw bad("directory checksum mismatch");
  }

  for (std::uint64_t f = 0; f < n; ++f)
  {
    FieldDirEntry d;
    std::memcpy(&d, map->data() + dir_off + f * sizeof d, sizeof d);
    if (
      d.packing != 0 || (d.flags & ~kKnownFieldFlags) != 0 ||
      d.tile_table_off != tables_off + f * ntiles * sizeof(FieldTileEntry) ||
      !std::isfinite(d.level_value))
    {
      throw bad("bad field directory entry " + std::to_string(f));
    }
    FieldInfo fi;
    fi.param_id = d.param_id;
    fi.level_type = d.level_type;
    fi.flags = d.flags;
    fi.level_value = d.level_value;
    fi.min = d.min;
    fi.max = d.max;
    for (const auto& o : ff->fields_)
    {
      if (o.same_field(fi))
      {
        throw bad("field listed twice");
      }
    }
    ff->fields_.push_back(fi);
  }
  ff->tiles_ =
    reinterpret_cast<const FieldTileEntry*>(map->data() + tables_off);
  for (std::uint64_t k = 0; k < *slots; ++k)
  {
    if (
      ff->tiles_[k].offset != kPage + k * h.slot_bytes ||
      ff->tiles_[k].pad != 0)
    {
      throw bad("bad tile table entry");
    }
  }
  ff->verified_ = std::make_unique<std::atomic<std::uint8_t>[]>(
    static_cast<std::size_t>(*slots));
  ff->map_ = std::move(map);
  return ff;
}

FieldFile::~FieldFile() = default;

std::optional<std::size_t> FieldFile::find(
  std::uint16_t param_id,
  std::uint8_t level_type,
  float level_value) const
{
  FieldInfo want;
  want.param_id = param_id;
  want.level_type = level_type;
  want.level_value = level_value;
  for (std::size_t k = 0; k < fields_.size(); ++k)
  {
    if (fields_[k].same_field(want))
    {
      return k;
    }
  }
  return std::nullopt;
}

void FieldFile::require_grid(const Grid& grid) const
{
  if (
    grid.key() != h_.grid_key || grid.ni() != h_.ni || grid.nj() != h_.nj ||
    grid.periodic_i() != periodic_i())
  {
    throw std::runtime_error(
      std::string(kWho) + ": " + path_.string() + ": written for grid " +
      key_hex(h_.grid_key) + ", not " + key_hex(grid.key()));
  }
}

const float* FieldFile::tile_ptr(std::size_t field, std::size_t t) const
{
  const std::size_t ntiles = static_cast<std::size_t>(h_.tiles_x) * h_.tiles_y;
  const std::size_t k = field * ntiles + t;
  const FieldTileEntry& e = tiles_[k];
  if (verified_[k].load(std::memory_order_acquire) == 0)
  {
    const std::uint64_t w = width(h_.tile, h_.halo);
    if (crc32(base_ + e.offset, w * w * sizeof(float)) != e.crc)
    {
      throw std::runtime_error(
        std::string(kWho) + ": " + path_.string() +
        ": tile checksum mismatch (field " + std::to_string(field) + ", tile " +
        std::to_string(t) + ")");
    }
    verified_[k].store(1, std::memory_order_release);
  }
  return reinterpret_cast<const float*>(base_ + e.offset);
}

std::span<const float>
FieldFile::tile(std::size_t field, std::uint32_t tx, std::uint32_t ty) const
{
  if (field >= fields_.size() || tx >= h_.tiles_x || ty >= h_.tiles_y)
  {
    throw std::out_of_range(std::string(kWho) + ": bad field or tile index");
  }
  const std::size_t w = static_cast<std::size_t>(width(h_.tile, h_.halo));
  return {
    tile_ptr(field, static_cast<std::size_t>(ty) * h_.tiles_x + tx), w * w};
}

float FieldFile::value(std::size_t field, std::uint32_t i, std::uint32_t j)
  const
{
  if (field >= fields_.size() || i >= h_.ni || j >= h_.nj)
  {
    throw std::out_of_range(std::string(kWho) + ": bad field or point");
  }
  const std::uint32_t tx = i / h_.tile, ty = j / h_.tile;
  const std::size_t w = static_cast<std::size_t>(width(h_.tile, h_.halo));
  const float* p =
    tile_ptr(field, static_cast<std::size_t>(ty) * h_.tiles_x + tx);
  const std::size_t li = i - tx * h_.tile + h_.halo;
  const std::size_t lj = j - ty * h_.tile + h_.halo;
  return p[lj * w + li];
}

float FieldFile::sample(std::size_t field, const Grid::Cell& c) const
{
  const bool has_right = periodic_i() ? c.i < h_.ni : c.i + 1 < h_.ni;
  if (field >= fields_.size() || !has_right || c.j + 1 >= h_.nj)
  {
    throw std::out_of_range(std::string(kWho) + ": bad field or cell");
  }
  const std::uint32_t tx = c.i / h_.tile, ty = c.j / h_.tile;
  const std::size_t w = static_cast<std::size_t>(width(h_.tile, h_.halo));
  const float* p =
    tile_ptr(field, static_cast<std::size_t>(ty) * h_.tiles_x + tx);
  const std::size_t li = c.i - tx * h_.tile + h_.halo;
  const std::size_t lj = c.j - ty * h_.tile + h_.halo;
  const double v00 = p[lj * w + li], v10 = p[lj * w + li + 1];
  const double v01 = p[(lj + 1) * w + li], v11 = p[(lj + 1) * w + li + 1];
  const double fx = c.fx, fy = c.fy;
  return static_cast<float>(
    (1.0 - fy) * ((1.0 - fx) * v00 + fx * v10) +
    fy * ((1.0 - fx) * v01 + fx * v11));
}

}  // namespace atmo
