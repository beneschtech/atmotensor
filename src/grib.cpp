#include "atmo/grib.h"

#include <eccodes.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "atmo/crc32.h"
#include "atmo/gridstore.h"
#include "atmo/source.h"
#include "atmo/variable.h"
#include "atmo/variables.h"
#include "fileio.h"

namespace atmo
{
namespace
{

namespace fs = std::filesystem;

constexpr double kMissing = 1.0e30;  // sentinel for bitmapped points

struct HandleDeleter
{
  void operator()(codes_handle* h) const { codes_handle_delete(h); }
};
using Handle = std::unique_ptr<codes_handle, HandleDeleter>;

[[noreturn]] void
corrupt(const std::string& file, std::size_t msg, const std::string& what)
{
  throw std::runtime_error(
    "grib: " + file + ": message " + std::to_string(msg) + ": " + what);
}

std::optional<long> get_long(codes_handle* h, const char* key)
{
  long v = 0;
  if (codes_get_long(h, key, &v) != 0)
  {
    return std::nullopt;
  }
  return v;
}

std::optional<double> get_double(codes_handle* h, const char* key)
{
  double v = 0;
  if (codes_get_double(h, key, &v) != 0)
  {
    return std::nullopt;
  }
  return v;
}

std::string get_string(codes_handle* h, const char* key)
{
  char buf[256];
  std::size_t n = sizeof buf;
  if (codes_get_string(h, key, buf, &n) != 0)
  {
    return {};
  }
  return std::string(buf);
}

bool is_missing(codes_handle* h, const char* key)
{
  int err = 0;
  const int m = codes_is_missing(h, key, &err);
  return err != 0 || m != 0;
}

bool supported_grid(const std::string& t)
{
  for (const char* ok :
       {"regular_ll", "regular_gg", "rotated_ll", "lambert",
        "polar_stereographic", "mercator", "lambert_azimuthal_equal_area"})
  {
    if (t == ok)
    {
      return true;
    }
  }
  return false;
}

// Destination index (j*ni + i) of each point in message order.
std::vector<std::uint32_t> scan_permutation(
  std::uint32_t ni,
  std::uint32_t nj,
  bool i_neg,
  bool j_pos,
  bool j_consecutive,
  bool alternate)
{
  std::vector<std::uint32_t> perm(static_cast<std::size_t>(ni) * nj);
  for (std::size_t k = 0; k < perm.size(); ++k)
  {
    std::size_t col, row;  // along i, along j, in message terms
    if (!j_consecutive)
    {
      row = k / ni;
      col = k % ni;
      if (alternate && (row % 2) == 1)
      {
        col = ni - 1 - col;
      }
    }
    else
    {
      col = k / nj;
      row = k % nj;
      if (alternate && (col % 2) == 1)
      {
        row = nj - 1 - row;
      }
    }
    const std::size_t i = i_neg ? ni - 1 - col : col;
    const std::size_t j = j_pos ? row : nj - 1 - row;
    perm[k] = static_cast<std::uint32_t>(j * ni + i);
  }
  return perm;
}

std::int64_t utc_seconds(long date, long hhmm)
{
  std::tm tm{};
  tm.tm_year = static_cast<int>(date / 10000) - 1900;
  tm.tm_mon = static_cast<int>((date / 100) % 100) - 1;
  tm.tm_mday = static_cast<int>(date % 100);
  tm.tm_hour = static_cast<int>(hhmm / 100);
  tm.tm_min = static_cast<int>(hhmm % 100);
  return static_cast<std::int64_t>(::timegm(&tm));
}

// What a message is, before its data is decoded.
struct Meta
{
  const VariableInfo* var = nullptr;
  FieldInfo info;
  std::int64_t run_s = 0;
  std::uint32_t fhr = 0;
  GridIdentity id;  // canon = section 3, ni, nj, periodic_i
  bool i_neg = false, j_pos = false, j_cons = false, alternate = false;
};

// Usable -> Meta; unusable -> nullopt with the reason in `skip`; damaged ->
// std::runtime_error.
std::optional<Meta> classify(
  codes_handle* h,
  const std::set<std::uint16_t>& select,
  const std::string& file,
  std::size_t msg,
  std::string& skip)
{
  if (get_long(h, "edition").value_or(0) != 2)
  {
    skip = "not GRIB edition 2";
    return std::nullopt;
  }
  const std::string grid_type = get_string(h, "gridType");
  if (!supported_grid(grid_type))
  {
    skip = "unsupported grid type '" + grid_type + "'";
    return std::nullopt;
  }
  const long pdt = get_long(h, "productDefinitionTemplateNumber").value_or(-1);
  if (pdt != 0 && pdt != 8)
  {
    skip = "unsupported product definition template 4." + std::to_string(pdt);
    return std::nullopt;
  }

  // Parameter.
  const auto disc = get_long(h, "discipline");
  const auto cat = get_long(h, "parameterCategory");
  const auto num = get_long(h, "parameterNumber");
  Meta m;
  m.var = disc && cat && num ? find_grib2_variable(
                                 static_cast<int>(*disc),
                                 static_cast<int>(*cat), static_cast<int>(*num))
                             : nullptr;
  if (!m.var)
  {
    skip = "parameter " + std::to_string(disc.value_or(-1)) + "." +
           std::to_string(cat.value_or(-1)) + "." +
           std::to_string(num.value_or(-1)) + " not in the variable registry";
    return std::nullopt;
  }
  if (!select.empty() && !select.count(m.var->code))
  {
    skip = std::string("variable ") + m.var->name + " not selected";
    return std::nullopt;
  }

  // Time.
  const auto date = get_long(h, "dataDate");
  const auto time = get_long(h, "dataTime");
  std::size_t len = 1;
  if (!date || !time || codes_set_string(h, "stepUnits", "m", &len) != 0)
  {
    corrupt(file, msg, "no reference time or step");
  }
  const auto start = get_long(h, "startStep");
  const auto end = get_long(h, "endStep");
  if (!start || !end || *end < 0)
  {
    corrupt(file, msg, "cannot read the forecast step");
  }
  if (pdt == 8 && *start != 0)
  {
    skip = "statistical interval " +
           std::to_string(static_cast<double>(*start) / 60.0) + "-" +
           std::to_string(static_cast<double>(*end) / 60.0) +
           " h does not start at the run";
    return std::nullopt;
  }
  if (*end % 60 != 0)
  {
    skip = "forecast step of " + std::to_string(*end) + " min is sub-hourly";
    return std::nullopt;
  }
  m.run_s = utc_seconds(*date, *time);
  m.fhr = static_cast<std::uint32_t>(*end / 60);

  // Level.
  m.info.param_id = m.var->code;
  const long lt = get_long(h, "typeOfFirstFixedSurface").value_or(255);
  if (lt < 0 || lt > 255)
  {
    corrupt(file, msg, "bad level type");
  }
  m.info.level_type = static_cast<std::uint8_t>(lt);
  if (
    !is_missing(h, "scaledValueOfFirstFixedSurface") &&
    !is_missing(h, "scaleFactorOfFirstFixedSurface"))
  {
    const long sv = get_long(h, "scaledValueOfFirstFixedSurface").value_or(0);
    const long sf = get_long(h, "scaleFactorOfFirstFixedSurface").value_or(0);
    m.info.level_value =
      static_cast<float>(static_cast<double>(sv) * std::pow(10.0, -sf));
  }
  const bool vector_component =
    m.var->code == var::kWindU || m.var->code == var::kWindV;
  if (vector_component && get_long(h, "uvRelativeToGrid").value_or(0) != 0)
  {
    m.info.flags = kFieldGridRelative;
  }

  // Grid identity: section 3 bytes + dimensions.
  const auto off3 = get_long(h, "offsetSection3");
  const auto len3 = get_long(h, "section3Length");
  const void* raw = nullptr;
  std::size_t raw_len = 0;
  if (
    !off3 || !len3 || codes_get_message(h, &raw, &raw_len) != 0 || *off3 < 0 ||
    *len3 <= 0 || static_cast<std::size_t>(*off3 + *len3) > raw_len)
  {
    corrupt(file, msg, "cannot locate section 3");
  }
  const auto* sec3 = static_cast<const std::uint8_t*>(raw) + *off3;
  m.id.canon.assign(sec3, sec3 + *len3);
  const auto ni = get_long(h, "Ni");
  const auto nj = get_long(h, "Nj");
  if (!ni || !nj || *ni <= 0 || *nj <= 0)
  {
    corrupt(file, msg, "bad grid dimensions");
  }
  m.id.ni = static_cast<std::uint32_t>(*ni);
  m.id.nj = static_cast<std::uint32_t>(*nj);
  if (grid_type == "regular_ll" || grid_type == "regular_gg")
  {
    const double inc =
      get_double(h, "iDirectionIncrementInDegrees").value_or(0.0);
    m.id.periodic_i =
      inc > 0.0 &&
      std::fabs(static_cast<double>(*ni) * inc - 360.0) < 0.01 * inc;
  }
  try
  {
    m.id.validate();
  }
  catch (const std::exception& e)
  {
    corrupt(file, msg, std::string("grid: ") + e.what());
  }
  m.i_neg = get_long(h, "iScansNegatively").value_or(0) != 0;
  m.j_pos = get_long(h, "jScansPositively").value_or(0) != 0;
  m.j_cons = get_long(h, "jPointsAreConsecutive").value_or(0) != 0;
  m.alternate = get_long(h, "alternativeRowScanning").value_or(0) != 0;
  return m;
}

std::vector<std::uint32_t> permutation(const Meta& m)
{
  return scan_permutation(
    m.id.ni, m.id.nj, m.i_neg, m.j_pos, m.j_cons, m.alternate);
}

// Normalised coordinates of the message's grid.
std::shared_ptr<GribGrid> decode_grid(
  codes_handle* h,
  const Meta& m,
  const std::vector<std::uint32_t>& perm,
  const std::string& file,
  std::size_t msg)
{
  const std::size_t n = perm.size();
  auto g = std::make_shared<GribGrid>();
  g->id = m.id;
  std::vector<double> lat(n), lon(n), val(n);
  if (codes_grib_get_data(h, lat.data(), lon.data(), val.data()) != 0)
  {
    corrupt(file, msg, "cannot compute grid coordinates");
  }
  g->data.lat_deg.resize(n);
  g->data.lon_deg.resize(n);
  for (std::size_t k = 0; k < n; ++k)
  {
    g->data.lat_deg[perm[k]] = lat[k];
    g->data.lon_deg[perm[k]] = lon[k];
  }
  return g;
}

// Values in normalised order and registry units, NaN where missing.
template <class T>
void decode_values(
  codes_handle* h,
  const VariableInfo& var,
  const std::vector<std::uint32_t>& perm,
  std::vector<T>& out,
  const std::string& file,
  std::size_t msg)
{
  const std::size_t n = perm.size();
  codes_set_double(h, "missingValue", kMissing);
  std::size_t nv = 0;
  if (codes_get_size(h, "values", &nv) != 0 || nv != n)
  {
    corrupt(file, msg, "value count does not match the grid");
  }
  std::vector<double> val(n);
  if (codes_get_double_array(h, "values", val.data(), &nv) != 0)
  {
    corrupt(file, msg, "cannot decode values");
  }
  const bool bitmap =
    get_long(h, "bitmapPresent").value_or(0) != 0 ||
    get_long(h, "missingValueManagementUsed").value_or(0) != 0;
  out.resize(n);
  for (std::size_t k = 0; k < n; ++k)
  {
    const double v = val[k];
    out[perm[k]] = bitmap && v == kMissing
                     ? std::numeric_limits<T>::quiet_NaN()
                     : static_cast<T>(v * var.grib2_scale + var.grib2_offset);
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// GribReader
// ---------------------------------------------------------------------------

struct GribReader::Impl
{
  std::string file;
  FILE* f = nullptr;
  std::set<std::uint16_t> select;
  std::shared_ptr<const GribGrid> last_grid;
  std::vector<std::uint32_t> last_perm;

  ~Impl()
  {
    if (f)
    {
      std::fclose(f);
    }
  }
};

GribReader::GribReader(const fs::path& path) : impl_(std::make_unique<Impl>())
{
  impl_->file = path.string();
  impl_->f = std::fopen(path.c_str(), "rb");
  if (!impl_->f)
  {
    throw std::runtime_error("grib: cannot open " + path.string());
  }
}

GribReader::~GribReader() = default;

void GribReader::select(std::set<std::uint16_t> variables)
{
  impl_->select = std::move(variables);
}

std::optional<GribMessage> GribReader::next()
{
  Impl& im = *impl_;
  for (;;)
  {
    int err = 0;
    Handle h(codes_handle_new_from_file(nullptr, im.f, PRODUCT_GRIB, &err));
    if (err != 0)
    {
      corrupt(im.file, index_, codes_get_error_message(err));
    }
    if (!h)
    {
      return std::nullopt;  // end of file
    }
    const std::size_t msg = index_++;
    std::string why;
    auto m = classify(h.get(), im.select, im.file, msg, why);
    if (!m)
    {
      skipped_.push_back(GribSkip{msg, im.file, why});
      continue;
    }
    // Grid: decoded once per distinct section 3.
    if (!im.last_grid || im.last_grid->id != m->id)
    {
      im.last_perm = permutation(*m);
      im.last_grid = decode_grid(h.get(), *m, im.last_perm, im.file, msg);
    }
    GribMessage out;
    out.grid = im.last_grid;
    decode_values(h.get(), *m->var, im.last_perm, out.values, im.file, msg);
    out.info = m->info;
    out.run_s = m->run_s;
    out.fhr = m->fhr;
    return out;
  }
}

// ---------------------------------------------------------------------------
// Ingest
// ---------------------------------------------------------------------------

GribIngestResult ingest_grib(
  const fs::path& root,
  const std::vector<fs::path>& files,
  const GribIngestOptions& opt)
{
  if (!SourceClass::valid_name(opt.model))
  {
    throw std::invalid_argument("grib: bad model name \"" + opt.model + "\"");
  }
  for (std::uint16_t code : opt.variables)
  {
    if (!find_variable(code))
    {
      throw std::invalid_argument(
        "grib: variable code " + std::to_string(code) + " is not registered");
    }
  }
  GribIngestResult r;
  GridStore store(root / "grids");
  std::shared_ptr<const Grid> grid;
  std::shared_ptr<const GribGrid> first_grid;
  std::unique_ptr<FieldFileWriter> writer;
  std::vector<FieldInfo> taken;
  fs::path field_path;

  for (const auto& file : files)
  {
    GribReader rd(file);
    rd.select(opt.variables);
    std::vector<GribSkip> extra;
    while (auto m = rd.next())
    {
      const std::size_t msg = rd.messages_read() - 1;
      if (!writer)
      {
        first_grid = m->grid;
        grid = store.put(m->grid->id, m->grid->data);
        r.field.model = opt.model;
        r.field.run_s = m->run_s;
        r.field.fhr = m->fhr;
        field_path =
          root / FieldFile::relative_path(opt.model, m->run_s, m->fhr);
        if (fs::exists(field_path))
        {
          throw std::runtime_error(
            "grib: " + field_path.string() + " already exists");
        }
        writer = std::make_unique<FieldFileWriter>(
          field_path, *grid, opt.model, m->run_s, m->fhr, opt.tile, opt.halo);
      }
      if (m->run_s != r.field.run_s || m->fhr != r.field.fhr)
      {
        corrupt(
          file.string(), msg,
          "run/forecast hour differs from the first message (one field file "
          "per run and hour)");
      }
      if (m->grid != first_grid && m->grid->id != first_grid->id)
      {
        extra.push_back(
          GribSkip{msg, file.string(), "grid differs from the first message"});
        continue;
      }
      bool dup = false;
      for (const auto& t : taken)
      {
        dup = dup || t.same_field(m->info);
      }
      if (dup)
      {
        extra.push_back(
          GribSkip{
            msg, file.string(),
            "field " + std::to_string(m->info.param_id) +
              " at this level already taken"});
        continue;
      }
      writer->add_field(m->info, m->values);
      taken.push_back(m->info);
    }
    r.messages += rd.messages_read();
    r.skipped.insert(r.skipped.end(), rd.skipped().begin(), rd.skipped().end());
    r.skipped.insert(r.skipped.end(), extra.begin(), extra.end());
  }
  if (!writer || taken.empty())
  {
    throw std::runtime_error("grib: no usable messages");
  }
  writer->seal();
  r.fields = taken.size();
  auto ff = FieldFile::open(field_path);
  r.field = FieldEntry::from_file(root, *ff);
  r.grid = GridEntry{
    grid->key(), "grids/" + key_hex(grid->key()) + ".grid",
    store.header_crc(grid->key())};
  return r;
}

void add_to_manifest(Manifest& m, const GribIngestResult& r)
{
  for (const auto& f : m.fields)
  {
    if (f.path == r.field.path)
    {
      throw std::invalid_argument(
        "grib: manifest already lists " + r.field.path);
    }
  }
  bool have_grid = false;
  for (const auto& g : m.grids)
  {
    have_grid = have_grid || g.key == r.grid.key;
  }
  if (!have_grid)
  {
    m.grids.push_back(r.grid);
  }
  m.fields.push_back(r.field);
}

}  // namespace atmo

// ---------------------------------------------------------------------------
// GRIB-native store
// ---------------------------------------------------------------------------

namespace atmo
{
namespace
{

std::string run_dir_name(std::int64_t run_s)
{
  const std::time_t t = static_cast<std::time_t>(run_s);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y%m%d%H", &tm);
  return buf;
}

// Copies `src` to `dst` (temp + fsync + link) when a hard link is not
// possible.
void copy_file_durably(const fs::path& src, const fs::path& dst)
{
  std::FILE* in = std::fopen(src.c_str(), "rb");
  if (!in)
  {
    throw std::runtime_error("grib: cannot read " + src.string());
  }
  detail::TempFile out(dst, "grib");
  std::vector<char> buf(1 << 20);
  std::size_t off = 0, n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
  {
    out.write(buf.data(), n, off);
    off += n;
  }
  const bool err = std::ferror(in) != 0;
  std::fclose(in);
  if (err)
  {
    throw std::runtime_error("grib: error reading " + src.string());
  }
  out.publish();
}

}  // namespace

GribNativeResult index_grib(
  const fs::path& root,
  const fs::path& source,
  const GribIngestOptions& opt)
{
  if (!SourceClass::valid_name(opt.model))
  {
    throw std::invalid_argument("grib: bad model name \"" + opt.model + "\"");
  }
  for (std::uint16_t code : opt.variables)
  {
    if (!find_variable(code))
    {
      throw std::invalid_argument(
        "grib: variable code " + std::to_string(code) + " is not registered");
    }
  }
  const std::string file = source.string();
  std::FILE* f = std::fopen(source.c_str(), "rb");
  if (!f)
  {
    throw std::runtime_error("grib: cannot open " + file);
  }
  struct Closer
  {
    std::FILE* f;
    ~Closer() { std::fclose(f); }
  } closer{f};

  GribNativeResult r;
  GribIndex& idx = r.index;
  idx.model = opt.model;
  GridStore store(root / "grids");
  std::map<GridKey, bool> grid_done;
  std::map<std::uint32_t, GridKey> grid_of_fhr;
  std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint8_t, float>> taken;
  std::optional<std::int64_t> run;
  std::size_t msg = 0;
  for (;; ++msg)
  {
    int err = 0;
    Handle h(codes_handle_new_from_file(nullptr, f, PRODUCT_GRIB, &err));
    if (err != 0)
    {
      corrupt(file, msg, codes_get_error_message(err));
    }
    if (!h)
    {
      break;
    }
    // Hash the bytes as they are on disk, before classify() sets keys
    // (setting stepUnits can make eccodes re-encode the message).
    const void* raw = nullptr;
    std::size_t raw_len = 0;
    const auto offset = get_long(h.get(), "offset");
    if (
      codes_get_message(h.get(), &raw, &raw_len) != 0 || !offset || *offset < 0)
    {
      corrupt(file, msg, "cannot read the message bytes");
    }
    const std::uint64_t hash = fnv1a64(raw, raw_len);
    const std::uint64_t length = raw_len;
    std::string why;
    auto m = classify(h.get(), opt.variables, file, msg, why);
    if (!m)
    {
      idx.skipped.push_back({msg, why});
      continue;
    }
    if (run && *run != m->run_s)
    {
      corrupt(file, msg, "messages from different runs in one file");
    }
    run = m->run_s;
    const GridKey key = m->id.key();
    auto [git, fresh_hour] = grid_of_fhr.insert({m->fhr, key});
    if (!fresh_hour && git->second != key)
    {
      idx.skipped.push_back({msg, "grid differs from the first message"});
      continue;
    }
    if (!taken
           .insert(
             {m->fhr, m->info.param_id, m->info.level_type,
              m->info.level_value})
           .second)
    {
      idx.skipped.push_back(
        {msg, "field " + std::to_string(m->info.param_id) +
                " at this level already taken"});
      continue;
    }
    if (!grid_done.count(key))
    {
      const auto perm = permutation(*m);
      auto g = decode_grid(h.get(), *m, perm, file, msg);
      auto grid = store.put(g->id, std::move(g->data));
      r.grids.push_back(
        GridEntry{
          key, "grids/" + key_hex(key) + ".grid", store.header_crc(key)});
      grid_done[key] = true;
    }
    GribIndexEntry e;
    e.offset = static_cast<std::uint64_t>(*offset);
    e.length = length;
    e.hash = hash;
    e.info = m->info;
    e.fhr = m->fhr;
    e.grid = key;
    idx.entries.push_back(e);
  }
  idx.messages_total = msg;
  if (idx.entries.empty())
  {
    throw std::runtime_error("grib: " + file + ": no usable messages");
  }
  idx.run_s = *run;
  idx.size = static_cast<std::uint64_t>(fs::file_size(source));

  const std::string rel = "grib/" + opt.model + "/" + run_dir_name(*run) + "/" +
                          source.filename().string();
  idx.grib = rel;
  idx.validate();
  const fs::path target = root / rel;
  if (fs::exists(target))
  {
    throw std::runtime_error("grib: " + target.string() + " already exists");
  }
  fs::create_directories(target.parent_path());
  if (::link(source.c_str(), target.c_str()) != 0)
  {
    if (errno != EXDEV)
    {
      throw std::runtime_error(
        "grib: cannot link " + file + " to " + target.string() + ": " +
        std::strerror(errno));
    }
    copy_file_durably(source, target);
  }
  detail::fsync_dir(target.parent_path(), "grib");
  const std::string text = idx.to_text();
  detail::publish_file(
    root / (rel + ".idx.json"),
    std::vector<unsigned char>(text.begin(), text.end()), "grib");

  r.entry.model = opt.model;
  r.entry.run_s = *run;
  r.entry.path = rel;
  r.entry.index_path = rel + ".idx.json";
  r.entry.index_crc = crc32(text.data(), text.size());
  r.entry.size = idx.size;
  for (const auto& g : r.grids)
  {
    r.entry.grids.push_back(g.key);
  }
  return r;
}

void add_to_manifest(Manifest& m, const GribNativeResult& r)
{
  for (const auto& g : m.gribs)
  {
    if (g.path == r.entry.path)
    {
      throw std::invalid_argument("grib: manifest already lists " + g.path);
    }
  }
  for (const auto& ng : r.grids)
  {
    bool have = false;
    for (const auto& g : m.grids)
    {
      have = have || g.key == ng.key;
    }
    if (!have)
    {
      m.grids.push_back(ng);
    }
  }
  m.gribs.push_back(r.entry);
}

GribDecoder grib_decoder()
{
  return [](
           const fs::path& file, const GribIndexEntry& e,
           std::shared_ptr<const Grid> grid) -> std::shared_ptr<GribMessageData>
  {
    const std::string name = file.string();
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
      throw std::runtime_error("grib: cannot open " + name);
    }
    std::vector<unsigned char> buf(e.length);
    std::size_t got = 0;
    while (got < buf.size())
    {
      const ssize_t n = ::pread(
        fd, buf.data() + got, buf.size() - got,
        static_cast<off_t>(e.offset + got));
      if (n <= 0)
      {
        break;
      }
      got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (got != buf.size() || fnv1a64(buf.data(), buf.size()) != e.hash)
    {
      throw std::runtime_error(
        "grib: " + name + ": message at offset " + std::to_string(e.offset) +
        " does not match the index");
    }
    Handle h(codes_handle_new_from_message(nullptr, buf.data(), buf.size()));
    if (!h)
    {
      throw std::runtime_error("grib: " + name + ": cannot parse message");
    }
    std::string why;
    auto m = classify(h.get(), {}, name, 0, why);
    if (
      !m || m->id.key() != e.grid || !grid || grid->key() != e.grid ||
      !m->info.same_field(e.info) || m->fhr != e.fhr)
    {
      throw std::runtime_error(
        "grib: " + name + ": message does not match the index");
    }
    auto d = std::make_shared<GribMessageData>();
    d->grid = std::move(grid);
    decode_values(h.get(), *m->var, permutation(*m), d->values, name, 0);
    return d;
  };
}

}  // namespace atmo
