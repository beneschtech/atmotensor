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

#include <nlohmann/json.hpp>

#include "atmo/crc32.h"
#include "atmo/gribsig.h"
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

// Where a message is: its grid identity and scanning mode.
struct GridMeta
{
  GridIdentity id;  // canon = section 3, ni, nj, periodic_i
  bool i_neg = false, j_pos = false, j_cons = false, alternate = false;
};

// What a message is, before its data is decoded.
struct Meta : GridMeta
{
  const VariableInfo* var = nullptr;
  FieldInfo info;
  std::int64_t run_s = 0;
  std::uint32_t fhr = 0;
};

// Supported grid -> GridMeta; unsupported -> nullopt with the reason in
// `skip`; damaged -> std::runtime_error.
std::optional<GridMeta> grid_meta(
  codes_handle* h,
  const std::string& file,
  std::size_t msg,
  std::string& skip)
{
  const std::string grid_type = get_string(h, "gridType");
  if (!supported_grid(grid_type))
  {
    skip = "unsupported grid type '" + grid_type + "'";
    return std::nullopt;
  }
  GridMeta m;
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

  std::string why;
  auto g = grid_meta(h, file, msg, why);
  if (!g)
  {
    skip = why;
    return std::nullopt;
  }
  static_cast<GridMeta&>(m) = std::move(*g);
  return m;
}

std::vector<std::uint32_t> permutation(const GridMeta& m)
{
  return scan_permutation(
    m.id.ni, m.id.nj, m.i_neg, m.j_pos, m.j_cons, m.alternate);
}

// Normalised coordinates of the message's grid.
std::shared_ptr<GribGrid> decode_grid(
  codes_handle* h,
  const GridMeta& m,
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
// Keeping files as published (grid catalog)
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

namespace
{

// A grib_grid row for a grid that cannot be sampled: keyed by the raw
// section 3 (whole section, as GridIdentity::canon elsewhere).
CatalogGrid placeholder_grid(
  const std::uint8_t* msg,
  std::size_t len,
  std::uint32_t ni,
  std::uint32_t nj,
  const std::string& type)
{
  std::size_t i = 16;
  GridIdentity id;
  while (i + 5 <= len && std::memcmp(msg + i, "7777", 4) != 0)
  {
    const std::size_t sl = (std::size_t(msg[i]) << 24) |
                           (std::size_t(msg[i + 1]) << 16) |
                           (std::size_t(msg[i + 2]) << 8) | msg[i + 3];
    if (sl < 5 || i + sl > len)
    {
      break;
    }
    if (msg[i + 4] == 3)
    {
      id.canon.assign(msg + i, msg + i + sl);
      break;
    }
    i += sl;
  }
  id.ni = ni;
  id.nj = nj;
  CatalogGrid g;
  g.key = id.key();
  g.viewable = false;
  g.ni = ni;
  g.nj = nj;
  if (id.canon.size() >= 14)
  {
    g.grid_type =
      static_cast<std::uint32_t>((id.canon[12] << 8) | id.canon[13]);
  }
  nlohmann::json j;
  j["gridType"] = type.empty() ? "unknown" : type;
  g.summary = j.dump();
  std::string up = type.empty() ? std::string("UNKNOWN") : type;
  for (auto& c : up)
  {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  g.name = "UNSUPPORTED " + up + " GRID " + std::to_string(g.grid_type);
  return g;
}

std::string fmt_g(double v)
{
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

// Row fields of a supported grid from its handle and coordinates.
CatalogGrid
describe_grid(codes_handle* h, const GridMeta& m, const GridData& data)
{
  CatalogGrid g;
  g.key = m.id.key();
  g.viewable = true;
  g.grid_type = static_cast<std::uint32_t>(
    get_long(h, "gridDefinitionTemplateNumber").value_or(0));
  g.ni = m.id.ni;
  g.nj = m.id.nj;
  g.periodic_i = m.id.periodic_i;
  double s = 90.0, n = -90.0;
  double w1 = 1e9, e1 = -1e9, w2 = 1e9, e2 = -1e9;  // [-180,180), [0,360)
  for (std::size_t k = 0; k < data.lat_deg.size(); ++k)
  {
    s = std::min(s, data.lat_deg[k]);
    n = std::max(n, data.lat_deg[k]);
    double a = std::fmod(data.lon_deg[k], 360.0);
    if (a < 0.0)
    {
      a += 360.0;
    }
    const double b = a >= 180.0 ? a - 360.0 : a;
    w1 = std::min(w1, b);
    e1 = std::max(e1, b);
    w2 = std::min(w2, a);
    e2 = std::max(e2, a);
  }
  g.south = s;
  g.north = n;
  if (e2 - w2 < e1 - w1)
  {
    g.west = w2 >= 180.0 ? w2 - 360.0 : w2;
    g.east = e2 >= 180.0 ? e2 - 360.0 : e2;
  }
  else
  {
    g.west = w1;
    g.east = e1;
  }
  const std::string type = get_string(h, "gridType");
  nlohmann::json j;
  j["gridType"] = type;
  for (const char* k :
       {"iDirectionIncrementInDegrees", "jDirectionIncrementInDegrees",
        "DxInMetres", "DyInMetres", "LoVInDegrees", "LaDInDegrees",
        "Latin1InDegrees", "Latin2InDegrees", "shapeOfTheEarth"})
  {
    if (auto v = get_double(h, k))
    {
      j[k] = *v;
    }
  }
  g.summary = j.dump();
  const std::string dims = std::to_string(g.ni) + "x" + std::to_string(g.nj);
  if (type == "regular_ll")
  {
    const double inc =
      get_double(h, "iDirectionIncrementInDegrees").value_or(0.0);
    g.name = fmt_g(inc) + "DEG LL " +
             (g.periodic_i && n - s >= 179.0 ? std::string("WORLDWIDE") : dims);
  }
  else if (type == "lambert")
  {
    g.name = "LAMBERT " +
             fmt_g(get_double(h, "DxInMetres").value_or(0.0) / 1000.0) + "KM " +
             dims;
  }
  else
  {
    std::string up = type;
    for (auto& c : up)
    {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    g.name = up + " " + dims;
  }
  return g;
}

// Hard-links (or copies) `source` to `target`; an identical file already
// there is fine.
void place_file(
  const fs::path& source,
  const fs::path& target,
  const detail::MappedFile& src)
{
  if (fs::exists(target))
  {
    std::error_code ec;
    if (fs::equivalent(source, target, ec))
    {
      return;
    }
    auto t = detail::MappedFile::open(target, 0, "grib");
    if (
      t->size() != src.size() ||
      std::memcmp(t->data(), src.data(), src.size()) != 0)
    {
      throw std::runtime_error(
        "grib: " + target.string() + " exists with different content");
    }
    return;
  }
  fs::create_directories(target.parent_path());
  if (::link(source.c_str(), target.c_str()) != 0)
  {
    if (errno != EXDEV)
    {
      throw std::runtime_error(
        "grib: cannot link " + source.string() + " to " + target.string() +
        ": " + std::strerror(errno));
    }
    copy_file_durably(source, target);
  }
  detail::fsync_dir(target.parent_path(), "grib");
}

}  // namespace

CatalogFile catalog_grib(
  const fs::path& root,
  const fs::path& source,
  const std::string& model)
{
  if (!SourceClass::valid_name(model))
  {
    throw std::invalid_argument("grib: bad model name \"" + model + "\"");
  }
  const std::string file = source.string();
  auto mapped = detail::MappedFile::open(source, 0, "grib");
  const auto* base = mapped->data();
  const auto spans = grib_message_spans(base, mapped->size());

  CatalogFile out;
  out.messages_total = static_cast<std::uint32_t>(spans.size());
  GridStore store(root / "grids");
  std::map<GridKey, std::size_t> grid_index;
  auto add_grid = [&](CatalogGrid g)
  {
    if (!grid_index.count(g.key))
    {
      grid_index[g.key] = out.grids.size();
      out.grids.push_back(std::move(g));
    }
  };
  std::optional<std::int64_t> earliest;
  for (std::size_t i = 0; i < spans.size(); ++i)
  {
    const GribSpan& sp = spans[i];
    if (sp.edition != 2)
    {
      out.skipped.push_back(
        {i,
         "GRIB edition " + std::to_string(sp.edition) + " is not catalogued"});
      continue;
    }
    const auto* p = base + sp.offset;
    const std::size_t len = static_cast<std::size_t>(sp.length);
    CatalogMessage cm;
    cm.byte_offset = sp.offset;
    cm.byte_length = sp.length;
    cm.content_hash = fnv1a64(p, len);
    try
    {
      cm.header = describe_grib(p, len);
    }
    catch (const std::runtime_error& e)
    {
      corrupt(file, i, e.what());
    }
    earliest =
      std::min(earliest.value_or(cm.header.base_time_s), cm.header.base_time_s);
    if (cm.header.sig.undecodable)
    {
      cm.flags |= kCatalogUndecodable;
      CatalogGrid g = placeholder_grid(p, len, 0, 0, "");
      cm.grid = g.key;
      add_grid(std::move(g));
      out.messages.push_back(std::move(cm));
      continue;
    }
    Handle h(codes_handle_new_from_message(nullptr, p, len));
    if (!h)
    {
      corrupt(file, i, "cannot parse message");
    }
    std::string why;
    auto gm = grid_meta(h.get(), file, i, why);
    if (!gm)
    {
      cm.flags |= kCatalogGridUnsupported;
      const long ni = get_long(h.get(), "Ni").value_or(0);
      const long nj = get_long(h.get(), "Nj").value_or(0);
      CatalogGrid g = placeholder_grid(
        p, len,
        ni > 0 && ni != CODES_MISSING_LONG ? static_cast<std::uint32_t>(ni)
                                           : 0u,
        nj > 0 && nj != CODES_MISSING_LONG ? static_cast<std::uint32_t>(nj)
                                           : 0u,
        get_string(h.get(), "gridType"));
      cm.grid = g.key;
      add_grid(std::move(g));
      out.messages.push_back(std::move(cm));
      continue;
    }
    cm.grid = gm->id.key();
    if (!grid_index.count(cm.grid))
    {
      auto g = decode_grid(h.get(), *gm, permutation(*gm), file, i);
      CatalogGrid row = describe_grid(h.get(), *gm, g->data);
      store.put(g->id, std::move(g->data));
      add_grid(std::move(row));
    }
    out.messages.push_back(std::move(cm));
  }
  if (out.messages.empty())
  {
    throw std::runtime_error("grib: " + file + ": no GRIB2 messages");
  }
  out.path = "grib/" + model + "/" + run_dir_name(*earliest) + "/" +
             source.filename().string();
  out.size = mapped->size();
  place_file(source, root / out.path, *mapped);
  out.validate();
  return out;
}

std::vector<double> grib_values(const void* msg, std::size_t len)
{
  Handle h(codes_handle_new_from_message(nullptr, msg, len));
  if (!h)
  {
    throw std::runtime_error("grib: cannot parse message");
  }
  codes_set_double(h.get(), "missingValue", kMissing);
  std::size_t n = 0;
  if (codes_get_size(h.get(), "values", &n) != 0)
  {
    throw std::runtime_error("grib: cannot decode values");
  }
  std::vector<double> v(n);
  if (codes_get_double_array(h.get(), "values", v.data(), &n) != 0)
  {
    throw std::runtime_error("grib: cannot decode values");
  }
  v.resize(n);
  const bool bitmap =
    get_long(h.get(), "bitmapPresent").value_or(0) != 0 ||
    get_long(h.get(), "missingValueManagementUsed").value_or(0) != 0;
  if (bitmap)
  {
    for (double& x : v)
    {
      if (x == kMissing)
      {
        x = std::numeric_limits<double>::quiet_NaN();
      }
    }
  }
  return v;
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
    if (e.info.param_id == kNativeParam)
    {
      // A grid catalog message (T17): any parameter, native units.
      static const VariableInfo native = {kNativeParam, "native", "",  -1, -1,
                                          -1,           1.0,      0.0, ""};
      auto gm = grid_meta(h.get(), name, 0, why);
      if (!gm || gm->id.key() != e.grid || !grid || grid->key() != e.grid)
      {
        throw std::runtime_error(
          "grib: " + name + ": message does not match the catalog");
      }
      auto d = std::make_shared<GribMessageData>();
      d->grid = std::move(grid);
      decode_values(h.get(), native, permutation(*gm), d->values, name, 0);
      d->uv_relative_to_grid =
        get_long(h.get(), "uvRelativeToGrid").value_or(0) != 0;
      return d;
    }
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
