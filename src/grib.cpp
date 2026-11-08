#include "atmo/grib.h"

#include <eccodes.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "atmo/gridstore.h"
#include "atmo/source.h"
#include "atmo/variable.h"

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

}  // namespace

// ---------------------------------------------------------------------------
// GribReader
// ---------------------------------------------------------------------------

struct GribReader::Impl
{
  std::string file;
  FILE* f = nullptr;
  std::set<std::uint16_t> select;
  std::vector<std::uint8_t> last_canon;
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
    auto skip = [&](const std::string& why)
    { skipped_.push_back(GribSkip{msg, im.file, why}); };

    if (get_long(h.get(), "edition").value_or(0) != 2)
    {
      skip("not GRIB edition 2");
      continue;
    }
    const std::string grid_type = get_string(h.get(), "gridType");
    if (!supported_grid(grid_type))
    {
      skip("unsupported grid type '" + grid_type + "'");
      continue;
    }
    const long pdt =
      get_long(h.get(), "productDefinitionTemplateNumber").value_or(-1);
    if (pdt != 0 && pdt != 8)
    {
      skip("unsupported product definition template 4." + std::to_string(pdt));
      continue;
    }

    // Parameter.
    const auto disc = get_long(h.get(), "discipline");
    const auto cat = get_long(h.get(), "parameterCategory");
    const auto num = get_long(h.get(), "parameterNumber");
    const VariableInfo* var =
      disc && cat && num ? find_grib2_variable(
                             static_cast<int>(*disc), static_cast<int>(*cat),
                             static_cast<int>(*num))
                         : nullptr;
    if (!var)
    {
      skip(
        "parameter " + std::to_string(disc.value_or(-1)) + "." +
        std::to_string(cat.value_or(-1)) + "." +
        std::to_string(num.value_or(-1)) + " not in the variable registry");
      continue;
    }
    if (!im.select.empty() && !im.select.count(var->code))
    {
      skip(std::string("variable ") + var->name + " not selected");
      continue;
    }

    // Time.
    const auto date = get_long(h.get(), "dataDate");
    const auto time = get_long(h.get(), "dataTime");
    std::size_t len = 1;
    if (
      !date || !time || codes_set_string(h.get(), "stepUnits", "m", &len) != 0)
    {
      corrupt(im.file, msg, "no reference time or step");
    }
    const auto start = get_long(h.get(), "startStep");
    const auto end = get_long(h.get(), "endStep");
    if (!start || !end || *end < 0)
    {
      corrupt(im.file, msg, "cannot read the forecast step");
    }
    if (pdt == 8 && *start != 0)
    {
      skip(
        "statistical interval " +
        std::to_string(static_cast<double>(*start) / 60.0) + "-" +
        std::to_string(static_cast<double>(*end) / 60.0) +
        " h does not start at the run");
      continue;
    }
    if (*end % 60 != 0)
    {
      skip("forecast step of " + std::to_string(*end) + " min is sub-hourly");
      continue;
    }

    // Level.
    FieldInfo info;
    info.param_id = var->code;
    const long lt = get_long(h.get(), "typeOfFirstFixedSurface").value_or(255);
    if (lt < 0 || lt > 255)
    {
      corrupt(im.file, msg, "bad level type");
    }
    info.level_type = static_cast<std::uint8_t>(lt);
    if (
      !is_missing(h.get(), "scaledValueOfFirstFixedSurface") &&
      !is_missing(h.get(), "scaleFactorOfFirstFixedSurface"))
    {
      const long sv =
        get_long(h.get(), "scaledValueOfFirstFixedSurface").value_or(0);
      const long sf =
        get_long(h.get(), "scaleFactorOfFirstFixedSurface").value_or(0);
      info.level_value =
        static_cast<float>(static_cast<double>(sv) * std::pow(10.0, -sf));
    }
    const bool vector_component = var->code == find_variable("wind_u")->code ||
                                  var->code == find_variable("wind_v")->code;
    if (
      vector_component &&
      get_long(h.get(), "uvRelativeToGrid").value_or(0) != 0)
    {
      info.flags = kFieldGridRelative;
    }

    // Grid: decoded once per distinct section 3.
    const auto off3 = get_long(h.get(), "offsetSection3");
    const auto len3 = get_long(h.get(), "section3Length");
    const void* raw = nullptr;
    std::size_t raw_len = 0;
    if (
      !off3 || !len3 || codes_get_message(h.get(), &raw, &raw_len) != 0 ||
      *off3 < 0 || *len3 <= 0 ||
      static_cast<std::size_t>(*off3 + *len3) > raw_len)
    {
      corrupt(im.file, msg, "cannot locate section 3");
    }
    const auto* sec3 = static_cast<const std::uint8_t*>(raw) + *off3;
    std::vector<std::uint8_t> canon(sec3, sec3 + *len3);
    const auto ni_l = get_long(h.get(), "Ni");
    const auto nj_l = get_long(h.get(), "Nj");
    if (!ni_l || !nj_l || *ni_l <= 0 || *nj_l <= 0)
    {
      corrupt(im.file, msg, "bad grid dimensions");
    }
    const auto ni = static_cast<std::uint32_t>(*ni_l);
    const auto nj = static_cast<std::uint32_t>(*nj_l);
    const std::size_t n = static_cast<std::size_t>(ni) * nj;
    if (canon != im.last_canon || !im.last_grid)
    {
      auto g = std::make_shared<GribGrid>();
      g->id.canon = canon;
      g->id.ni = ni;
      g->id.nj = nj;
      if (grid_type == "regular_ll" || grid_type == "regular_gg")
      {
        const double inc =
          get_double(h.get(), "iDirectionIncrementInDegrees").value_or(0.0);
        g->id.periodic_i =
          inc > 0.0 && std::fabs(ni * inc - 360.0) < 0.01 * inc;
      }
      try
      {
        g->id.validate();
      }
      catch (const std::exception& e)
      {
        corrupt(im.file, msg, std::string("grid: ") + e.what());
      }
      im.last_perm = scan_permutation(
        ni, nj, get_long(h.get(), "iScansNegatively").value_or(0) != 0,
        get_long(h.get(), "jScansPositively").value_or(0) != 0,
        get_long(h.get(), "jPointsAreConsecutive").value_or(0) != 0,
        get_long(h.get(), "alternativeRowScanning").value_or(0) != 0);
      std::vector<double> lat(n), lon(n), val(n);
      if (codes_grib_get_data(h.get(), lat.data(), lon.data(), val.data()) != 0)
      {
        corrupt(im.file, msg, "cannot compute grid coordinates");
      }
      g->data.lat_deg.resize(n);
      g->data.lon_deg.resize(n);
      for (std::size_t k = 0; k < n; ++k)
      {
        g->data.lat_deg[im.last_perm[k]] = lat[k];
        g->data.lon_deg[im.last_perm[k]] = lon[k];
      }
      im.last_grid = std::move(g);
      im.last_canon = std::move(canon);
    }

    // Values, in message order -> normalised, registry units.
    codes_set_double(h.get(), "missingValue", kMissing);
    std::size_t nv = 0;
    if (codes_get_size(h.get(), "values", &nv) != 0 || nv != n)
    {
      corrupt(im.file, msg, "value count does not match the grid");
    }
    std::vector<double> val(n);
    if (codes_get_double_array(h.get(), "values", val.data(), &nv) != 0)
    {
      corrupt(im.file, msg, "cannot decode values");
    }
    const bool bitmap =
      get_long(h.get(), "bitmapPresent").value_or(0) != 0 ||
      get_long(h.get(), "missingValueManagementUsed").value_or(0) != 0;
    GribMessage out;
    out.grid = im.last_grid;
    out.values.resize(n);
    for (std::size_t k = 0; k < n; ++k)
    {
      const double v = val[k];
      out.values[im.last_perm[k]] =
        bitmap && v == kMissing
          ? std::numeric_limits<float>::quiet_NaN()
          : static_cast<float>(v * var->grib2_scale + var->grib2_offset);
    }
    out.info = info;
    out.run_s = utc_seconds(*date, *time);
    out.fhr = static_cast<std::uint32_t>(*end / 60);
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
