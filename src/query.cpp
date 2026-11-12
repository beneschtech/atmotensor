#include "atmo/query.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "atmo/snapshot.h"
#include "atmo/variables.h"

namespace atmo
{
namespace
{

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Mean Earth radius for geopotential -> geometric height (Q3).
constexpr double kEarthRadiusM = 6371008.8;

double geometric_from_geopotential(double gh_m)
{
  return kEarthRadiusM * gh_m / (kEarthRadiusM - gh_m);
}

// Shortest decimal that reads back as the same float, as a double, so JSON
// shows 290.94 rather than 290.94000244140625.
double tidy(double v)
{
  if (!std::isfinite(v))
  {
    return v;
  }
  const float f = static_cast<float>(v);
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, f);
  double out = 0.0;
  std::from_chars(buf, r.ptr, out);
  return out;
}

std::string format_number(double v)
{
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<float>(v));
  return std::string(buf, r.ptr);
}

bool parse_number(std::string_view s, double& out)
{
  if (s.empty())
  {
    return false;
  }
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size() &&
         std::isfinite(out);
}

ParamRole registry_role(std::uint16_t code)
{
  switch (code)
  {
    case var::kWindU:
      return ParamRole::wind_u;
    case var::kWindV:
      return ParamRole::wind_v;
    case var::kGeopotentialHeight:
      return ParamRole::geopotential_height;
    default:
      return ParamRole::other;
  }
}

// "@2m", "@850hPa", ...: the level part of a canonical spec.
std::string level_suffix(std::uint8_t type, float value)
{
  switch (type)
  {
    case kLevelSurface:
      return "@sfc";
    case kLevelMsl:
      return "@msl";
    case kLevelHeightAgl:
      return "@" + format_number(value) + "m";
    case kLevelIsobaric:
      return "@" + format_number(value / 100.0) + "hPa";
    default:
      return "@L" + std::to_string(type) + "=" + format_number(value);
  }
}

// One model at one (run, fhr): grid, located cell and field list.
struct TimeSlice
{
  ModelTime t;
  std::shared_ptr<const Grid> grid;
  std::optional<Grid::Cell> cell;
  std::vector<FieldInfo> fields;
};

double alpha_at(const Grid& g, const Grid::Cell& c)
{
  const std::uint32_t i1 = c.i + 1 == g.ni() ? 0 : c.i + 1;
  const std::uint32_t j1 = c.j + 1;
  const float* rot = g.rot_rad();
  const double a[4] = {
    rot[g.index(c.i, c.j)], rot[g.index(i1, c.j)], rot[g.index(c.i, j1)],
    rot[g.index(i1, j1)]};
  const double w[4] = {
    (1 - c.fx) * (1 - c.fy), c.fx * (1 - c.fy), (1 - c.fx) * c.fy, c.fx * c.fy};
  double s = 0.0, co = 0.0;
  for (int k = 0; k < 4; ++k)
  {
    s += w[k] * std::sin(a[k]);
    co += w[k] * std::cos(a[k]);
  }
  return std::atan2(s, co);
}

class Sampler
{
 public:
  Sampler(const FieldStore& store, std::string_view model, const PointQuery& q)
      : store_(store), model_(model), q_(q)
  {
  }

  // Parameters of `role` present in the slice (first seen first).
  std::vector<std::uint16_t> with_role(const TimeSlice& ts, ParamRole role)
    const
  {
    std::vector<std::uint16_t> out;
    for (const auto& f : ts.fields)
    {
      if (
        std::find(out.begin(), out.end(), f.param_id) == out.end() &&
        store_.param(f.param_id).role == role)
      {
        out.push_back(f.param_id);
      }
    }
    return out;
  }

  // Value of `code` at the requested level at one time slice.
  PointValue::Status value(
    const TimeSlice& ts,
    const FieldSpec& spec,
    std::uint16_t code,
    double& out,
    bool& grid_relative) const
  {
    if (spec.level == FieldSpec::Level::fixed)
    {
      if (spec.level_type == kLevelIsobaric)
      {
        return isobaric(ts, code, spec.level_value, out, grid_relative);
      }
      return fixed(
        ts, FieldKey{code, spec.level_type, spec.level_value}, out,
        grid_relative);
    }
    if (q_.level_hpa)
    {
      return isobaric(ts, code, *q_.level_hpa * 100.0, out, grid_relative);
    }
    if (q_.alt_m)
    {
      return altitude(ts, code, *q_.alt_m, out, grid_relative);
    }
    const FieldInfo* only = nullptr;
    int n = 0;
    for (const auto& f : ts.fields)
    {
      if (f.param_id == code)
      {
        only = &f;
        ++n;
      }
    }
    if (n == 0)
    {
      return PointValue::Status::no_field;
    }
    if (n > 1)
    {
      return PointValue::Status::level_required;
    }
    return fixed(
      ts, FieldKey{code, only->level_type, only->level_value}, out,
      grid_relative);
  }

 private:
  const FieldInfo* find(const TimeSlice& ts, const FieldKey& k) const
  {
    for (const auto& f : ts.fields)
    {
      if (
        f.param_id == k.param_id && f.level_type == k.level_type &&
        f.level_value == k.level_value)
      {
        return &f;
      }
    }
    return nullptr;
  }

  PointValue::Status fixed(
    const TimeSlice& ts,
    const FieldKey& k,
    double& out,
    bool& grid_relative) const
  {
    const FieldInfo* f = find(ts, k);
    if (!f)
    {
      return PointValue::Status::no_field;
    }
    if (f->flags & kFieldAmbiguous)
    {
      return PointValue::Status::ambiguous_field;
    }
    grid_relative = grid_relative || (f->flags & kFieldGridRelative) != 0;
    out = store_.sample(model_, ts.t, k, *ts.cell);
    return PointValue::Status::ok;
  }

  // Stored isobaric levels (Pa) of `code`, ascending.
  std::vector<float> isobaric_levels(const TimeSlice& ts, std::uint16_t code)
    const
  {
    std::vector<float> p;
    for (const auto& f : ts.fields)
    {
      if (f.param_id == code && f.level_type == kLevelIsobaric)
      {
        p.push_back(f.level_value);
      }
    }
    std::sort(p.begin(), p.end());
    return p;
  }

  PointValue::Status isobaric(
    const TimeSlice& ts,
    std::uint16_t code,
    double p_pa,
    double& out,
    bool& grid_relative) const
  {
    const std::vector<float> p = isobaric_levels(ts, code);
    if (p.empty())
    {
      return PointValue::Status::no_field;
    }
    for (float lv : p)
    {
      if (static_cast<double>(lv) == p_pa)
      {
        return fixed(
          ts, FieldKey{code, kLevelIsobaric, lv}, out, grid_relative);
      }
    }
    if (p_pa < p.front() || p_pa > p.back())
    {
      return PointValue::Status::out_of_levels;
    }
    const auto hi = std::upper_bound(p.begin(), p.end(), p_pa);
    const float p2 = *hi, p1 = *(hi - 1);
    double v1 = 0.0, v2 = 0.0;
    const auto s1 =
      fixed(ts, FieldKey{code, kLevelIsobaric, p1}, v1, grid_relative);
    const auto s2 =
      fixed(ts, FieldKey{code, kLevelIsobaric, p2}, v2, grid_relative);
    if (s1 != PointValue::Status::ok || s2 != PointValue::Status::ok)
    {
      return s1 != PointValue::Status::ok ? s1 : s2;
    }
    const double w =
      std::log(p_pa / p1) / std::log(static_cast<double>(p2) / p1);
    out = v1 + w * (v2 - v1);
    return PointValue::Status::ok;
  }

  PointValue::Status altitude(
    const TimeSlice& ts,
    std::uint16_t code,
    double alt_m,
    double& out,
    bool& grid_relative) const
  {
    // Isobaric levels that have both the variable and geopotential height.
    const std::vector<float> pv = isobaric_levels(ts, code);
    std::uint16_t gh_id = 0;
    std::vector<float> pz;
    for (const std::uint16_t id : with_role(ts, ParamRole::geopotential_height))
    {
      pz = isobaric_levels(ts, id);
      gh_id = id;
      if (!pz.empty())
      {
        break;
      }
    }
    std::vector<std::pair<double, float>> zs;  // (geometric z, level)
    for (float lv : pv)
    {
      if (!std::binary_search(pz.begin(), pz.end(), lv))
      {
        continue;
      }
      double gh = 0.0;
      bool unused = false;
      fixed(ts, FieldKey{gh_id, kLevelIsobaric, lv}, gh, unused);
      if (std::isfinite(gh))
      {
        zs.push_back({geometric_from_geopotential(gh), lv});
      }
    }
    if (zs.empty())
    {
      return pv.empty() ? PointValue::Status::no_field
                        : PointValue::Status::out_of_levels;
    }
    std::sort(zs.begin(), zs.end());
    if (alt_m < zs.front().first || alt_m > zs.back().first)
    {
      return PointValue::Status::out_of_levels;
    }
    std::size_t k = 0;
    while (k + 1 < zs.size() && zs[k + 1].first < alt_m)
    {
      ++k;
    }
    double v1 = 0.0;
    const auto s1 = fixed(
      ts, FieldKey{code, kLevelIsobaric, zs[k].second}, v1, grid_relative);
    if (s1 != PointValue::Status::ok)
    {
      return s1;
    }
    if (k + 1 == zs.size() || zs[k].first == alt_m)
    {
      out = v1;
      return PointValue::Status::ok;
    }
    double v2 = 0.0;
    const auto s2 = fixed(
      ts, FieldKey{code, kLevelIsobaric, zs[k + 1].second}, v2, grid_relative);
    if (s2 != PointValue::Status::ok)
    {
      return s2;
    }
    const double w = (alt_m - zs[k].first) / (zs[k + 1].first - zs[k].first);
    out = v1 + w * (v2 - v1);
    return PointValue::Status::ok;
  }

  const FieldStore& store_;
  std::string_view model_;
  const PointQuery& q_;
};

// The newest run whose hours bracket t: one or two (time, weight) slices.
std::vector<std::pair<ModelTime, double>> pick_times(
  const std::vector<ModelTime>& times,
  std::int64_t t)
{
  // times are sorted by (run, fhr); walk runs from the newest.
  std::size_t end = times.size();
  while (end > 0)
  {
    std::size_t begin = end - 1;
    while (begin > 0 && times[begin - 1].run_s == times[end - 1].run_s)
    {
      --begin;
    }
    for (std::size_t k = begin; k < end; ++k)
    {
      const std::int64_t v = times[k].valid_s();
      if (v == t)
      {
        return {{times[k], 1.0}};
      }
      if (k + 1 < end && v < t && times[k + 1].valid_s() > t)
      {
        const double w = static_cast<double>(t - v) /
                         static_cast<double>(times[k + 1].valid_s() - v);
        return {{times[k], 1.0 - w}, {times[k + 1], w}};
      }
    }
    end = begin;
  }
  return {};
}

}  // namespace

// ---------------------------------------------------------------------------

std::int64_t ModelTime::valid_s() const
{
  return run_s + static_cast<std::int64_t>(fhr) * 3600;
}

FieldStore::~FieldStore() = default;

NameError::NameError(
  int status_,
  std::string code_,
  const std::string& message,
  std::vector<std::string> candidates_)
    : std::invalid_argument(message),
      status(status_),
      code(std::move(code_)),
      candidates(std::move(candidates_))
{
}

ParamInfo FieldStore::param(std::uint16_t id) const
{
  const VariableInfo* v = find_variable(id);
  if (!v)
  {
    return {"param" + std::to_string(id), "", ParamRole::other};
  }
  return {std::string(v->name), std::string(v->units), registry_role(id)};
}

ResolvedName FieldStore::resolve_field(std::string_view name) const
{
  const VariableInfo* v = find_variable(name);
  if (!v)
  {
    throw NameError(
      400, "bad_request",
      "field \"" + std::string(name) + "\": unknown variable");
  }
  return {std::string(v->name), v->code, false};
}

ResolvedName FieldStore::resolve_model(std::string_view name) const
{
  const std::vector<std::string> all = models();
  if (!std::binary_search(all.begin(), all.end(), name))
  {
    throw NameError(
      400, "bad_request", "unknown model \"" + std::string(name) + "\"");
  }
  return {std::string(name), 0, false};
}

FieldView::FieldView(std::shared_ptr<const Grid> grid, FieldInfo info)
    : grid_(std::move(grid)), info_(info)
{
}

FieldView::~FieldView() = default;

double FieldView::sample(const Grid::Cell& c) const
{
  const Grid& g = *grid_;
  if (c.j + 1 >= g.nj() || (g.periodic_i() ? c.i >= g.ni() : c.i + 1 >= g.ni()))
  {
    return kNaN;
  }
  const std::uint32_t i1 = c.i + 1 == g.ni() ? 0 : c.i + 1;
  const double v00 = at(c.i, c.j), v10 = at(i1, c.j);
  const double v01 = at(c.i, c.j + 1), v11 = at(i1, c.j + 1);
  return (1.0 - c.fy) * ((1.0 - c.fx) * v00 + c.fx * v10) +
         c.fy * ((1.0 - c.fx) * v01 + c.fx * v11);
}

namespace
{

class FieldFileView : public FieldView
{
 public:
  FieldFileView(
    std::shared_ptr<const Grid> g,
    std::shared_ptr<const FieldFile> f,
    std::size_t k)
      : FieldView(std::move(g), f->field(k)), file_(std::move(f)), k_(k)
  {
  }
  double at(std::uint32_t i, std::uint32_t j) const override
  {
    return file_->value(k_, i, j);
  }

 private:
  std::shared_ptr<const FieldFile> file_;
  std::size_t k_;
};

}  // namespace

SnapshotFieldStore::SnapshotFieldStore(std::shared_ptr<const Snapshot> s)
    : snap_(std::move(s))
{
  std::map<std::string, std::set<std::pair<std::int64_t, std::uint32_t>>>
    by_model;
  for (const auto& f : snap_->manifest().fields)
  {
    by_model[f.model].insert({f.run_s, f.fhr});
  }
  for (auto& [m, v] : by_model)
  {
    std::vector<ModelTime> t;
    for (const auto& [run, fhr] : v)
    {
      t.push_back(ModelTime{run, fhr});
    }
    times_.push_back({m, std::move(t)});
  }
}

SnapshotFieldStore::~SnapshotFieldStore() = default;

std::vector<std::string> SnapshotFieldStore::models() const
{
  std::vector<std::string> v;
  for (const auto& [m, t] : times_)
  {
    v.push_back(m);
  }
  return v;
}

std::vector<ModelTime> SnapshotFieldStore::times(std::string_view model) const
{
  for (const auto& [m, t] : times_)
  {
    if (m == model)
    {
      return t;
    }
  }
  return {};
}

std::shared_ptr<const FieldFile> SnapshotFieldStore::file(
  std::string_view model,
  const ModelTime& t) const
{
  return snap_->field_file(model, t.run_s, t.fhr);
}

std::shared_ptr<const Grid> SnapshotFieldStore::grid(
  std::string_view model,
  const ModelTime& t) const
{
  if (auto f = file(model, t))
  {
    return snap_->grid(f->grid_key());
  }
  return nullptr;
}

std::vector<FieldInfo> SnapshotFieldStore::fields(
  std::string_view model,
  const ModelTime& t) const
{
  std::vector<FieldInfo> v;
  if (auto f = file(model, t))
  {
    for (std::size_t k = 0; k < f->size(); ++k)
    {
      v.push_back(f->field(k));
    }
  }
  return v;
}

float SnapshotFieldStore::sample(
  std::string_view model,
  const ModelTime& t,
  const FieldKey& key,
  const Grid::Cell& cell) const
{
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  if (auto f = file(model, t))
  {
    auto k = f->find(key.param_id, key.level_type, key.level_value);
    return k ? f->sample(*k, cell) : nan;
  }
  return nan;
}

std::shared_ptr<const FieldView> SnapshotFieldStore::view(
  std::string_view model,
  const ModelTime& t,
  const FieldKey& key) const
{
  if (auto f = file(model, t))
  {
    auto k = f->find(key.param_id, key.level_type, key.level_value);
    auto g = snap_->grid(f->grid_key());
    if (!k || !g)
    {
      return nullptr;
    }
    return std::make_shared<FieldFileView>(std::move(g), f, *k);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Field specs
// ---------------------------------------------------------------------------

FieldSpec FieldSpec::parse(std::string_view text)
{
  auto bad = [&](const std::string& why)
  {
    return std::invalid_argument("field \"" + std::string(text) + "\": " + why);
  };
  FieldSpec s;
  const auto at = text.find('@');
  const std::string_view name = text.substr(0, at);
  if (name.empty() || name.find(',') != std::string_view::npos)
  {
    throw bad("missing or malformed name");
  }
  s.name = std::string(name);
  s.var = find_variable(name);
  if (at == std::string_view::npos)
  {
    s.text = s.name;
    return s;
  }
  const std::string_view lv = text.substr(at + 1);
  s.level = Level::fixed;
  double v = 0.0;
  if (lv == "sfc")
  {
    s.level_type = kLevelSurface;
  }
  else if (lv == "msl")
  {
    s.level_type = kLevelMsl;
  }
  else if (
    lv.size() > 3 && lv.substr(lv.size() - 3) == "hPa" &&
    parse_number(lv.substr(0, lv.size() - 3), v) && v > 0.0)
  {
    s.level_type = kLevelIsobaric;
    s.level_value = static_cast<float>(v * 100.0);
  }
  else if (
    lv.size() > 1 && lv.back() == 'm' &&
    parse_number(lv.substr(0, lv.size() - 1), v) && v >= 0.0)
  {
    s.level_type = kLevelHeightAgl;
    s.level_value = static_cast<float>(v);
  }
  else if (lv.size() > 3 && lv[0] == 'L' && lv.find('=') != lv.npos)
  {
    const auto eq = lv.find('=');
    double t = 0.0;
    if (
      !parse_number(lv.substr(1, eq - 1), t) || t < 0 || t > 255 ||
      t != std::floor(t) || !parse_number(lv.substr(eq + 1), v))
    {
      throw bad("bad level");
    }
    s.level_type = static_cast<std::uint8_t>(t);
    s.level_value = static_cast<float>(v);
  }
  else
  {
    throw bad("bad level (use sfc, msl, <x>m, <x>hPa or L<type>=<value>)");
  }
  s.text = s.name + level_suffix(s.level_type, s.level_value);
  return s;
}

void FieldSpec::resolve(
  const FieldStore& store,
  std::vector<NameWarning>* warnings,
  const std::string& argument)
{
  const ResolvedName r = store.resolve_field(name);
  param = r.param;
  if (r.guess && warnings)
  {
    warnings->push_back({argument, name, r.name});
  }
  text = r.name +
         (level == Level::fixed ? level_suffix(level_type, level_value) : "");
}

std::string FieldSpec::format(const FieldInfo& f)
{
  const VariableInfo* v = find_variable(f.param_id);
  return (v ? std::string(v->name) : "param" + std::to_string(f.param_id)) +
         level_suffix(f.level_type, f.level_value);
}

std::string FieldSpec::format(const FieldInfo& f, const FieldStore& store)
{
  return store.param(f.param_id).name +
         level_suffix(f.level_type, f.level_value);
}

// ---------------------------------------------------------------------------
// point()
// ---------------------------------------------------------------------------

PointResult point(
  const FieldStore& store,
  const ObsIndex* obs,
  const SourceRegistry& sources,
  const PointQuery& q)
{
  if (
    !std::isfinite(q.lat_deg) || !std::isfinite(q.lon_deg) ||
    q.lat_deg < -90.0 || q.lat_deg > 90.0)
  {
    throw std::invalid_argument("point: bad lat/lon");
  }
  if (q.level_hpa && q.alt_m)
  {
    throw std::invalid_argument("point: give level_hpa or alt_m, not both");
  }
  if (
    (q.level_hpa && !(std::isfinite(*q.level_hpa) && *q.level_hpa > 0)) ||
    (q.alt_m && !std::isfinite(*q.alt_m)))
  {
    throw std::invalid_argument("point: bad vertical coordinate");
  }
  if (q.obs && !obs)
  {
    throw std::invalid_argument("point: no observation index");
  }
  PointResult r;
  std::vector<std::string> models;
  if (q.models.empty())
  {
    models = store.models();
  }
  for (const auto& asked : q.models)
  {
    const ResolvedName m = store.resolve_model(asked);
    if (m.guess)
    {
      r.warnings.push_back({"models", asked, m.name});
    }
    models.push_back(m.name);
  }
  std::vector<FieldSpec> specs = q.fields;
  for (auto& spec : specs)
  {
    spec.resolve(store, &r.warnings);
  }

  for (const auto& m : models)
  {
    const auto picks = pick_times(store.times(m), q.t_s);
    std::vector<TimeSlice> slices;
    bool on_grid = true;
    for (const auto& [t, w] : picks)
    {
      TimeSlice ts;
      ts.t = t;
      ts.grid = store.grid(m, t);
      ts.cell = ts.grid ? ts.grid->locate(q.lat_deg, q.lon_deg) : std::nullopt;
      on_grid = on_grid && ts.cell.has_value();
      ts.fields = store.fields(m, t);
      slices.push_back(std::move(ts));
    }
    Sampler sampler(store, m, q);
    for (const auto& spec : specs)
    {
      const ParamInfo info = store.param(spec.param);
      PointValue pv;
      pv.model = m;
      pv.field = spec.text;
      pv.var = spec.var;
      pv.units = info.units;
      if (picks.empty())
      {
        pv.status = PointValue::Status::no_time;
        r.values.push_back(pv);
        continue;
      }
      if (!on_grid)
      {
        pv.status = PointValue::Status::outside_grid;
        r.values.push_back(pv);
        continue;
      }
      pv.run_s = picks[0].first.run_s;
      double value = 0.0, fhr = 0.0;
      for (std::size_t k = 0;
           k < slices.size() && pv.status == PointValue::Status::ok; ++k)
      {
        const TimeSlice& ts = slices[k];
        double v = 0.0;
        bool grid_rel = false;
        pv.status = sampler.value(ts, spec, spec.param, v, grid_rel);
        const bool is_u = info.role == ParamRole::wind_u;
        if (
          pv.status == PointValue::Status::ok &&
          (is_u || info.role == ParamRole::wind_v) && grid_rel)
        {
          // Rotate the grid-relative pair to east/north: the first
          // partner present at the same level.
          double o = 0.0;
          auto st = PointValue::Status::no_field;
          for (const std::uint16_t other : sampler.with_role(
                 ts, is_u ? ParamRole::wind_v : ParamRole::wind_u))
          {
            bool unused = false;
            st = sampler.value(ts, spec, other, o, unused);
            if (st != PointValue::Status::no_field)
            {
              break;
            }
          }
          pv.status = st;
          const double u = is_u ? v : o;
          const double vv = is_u ? o : v;
          double east = 0.0, north = 0.0;
          rotate_to_earth(
            grid_alpha_at(*ts.grid, *ts.cell), u, vv, east, north);
          v = is_u ? east : north;
        }
        value += picks[k].second * v;
        fhr += picks[k].second * ts.t.fhr;
      }
      pv.value = pv.status == PointValue::Status::ok ? value : kNaN;
      pv.fhr = fhr;
      r.values.push_back(pv);
    }
  }

  if (q.obs)
  {
    ObsQuery oq;
    oq.lat_deg = q.lat_deg;
    oq.lon_deg = q.lon_deg;
    oq.height_m = q.alt_m.value_or(0.0);
    oq.t_s = static_cast<double>(q.t_s);
    oq.as_of = q.as_of;
    for (const auto& s : sources.all())
    {
      for (auto& h : obs->radius(s.name, oq, q.obs_radius_km))
      {
        r.obs.push_back({s.name, h});
      }
    }
  }
  return r;
}

double grid_alpha_at(const Grid& g, const Grid::Cell& c)
{
  return alpha_at(g, c);
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

const char* to_string(PointValue::Status s)
{
  switch (s)
  {
    case PointValue::Status::ok:
      return "ok";
    case PointValue::Status::no_time:
      return "no_time";
    case PointValue::Status::outside_grid:
      return "outside_grid";
    case PointValue::Status::no_field:
      return "no_field";
    case PointValue::Status::level_required:
      return "level_required";
    case PointValue::Status::out_of_levels:
      return "out_of_levels";
    case PointValue::Status::ambiguous_field:
      return "ambiguous_field";
  }
  return "?";
}

std::string iso8601(std::int64_t t_s)
{
  const std::time_t t = static_cast<std::time_t>(t_s);
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::int64_t parse_iso8601(std::string_view text)
{
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  char tail[8] = {};
  const std::string str(text);
  const int n = std::sscanf(
    str.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%7s", &y, &mo, &d, &h, &mi, &s, tail);
  const bool ok_tail = (n == 5 && str.size() == 16) ||
                       (n == 6 && str.size() == 19) ||
                       (n == 7 && std::string(tail) == "Z" && str.size() == 20);
  int n2 = 0;
  if (!ok_tail && str.size() == 17 && str.back() == 'Z')
  {
    n2 = std::sscanf(str.c_str(), "%4d-%2d-%2dT%2d:%2dZ", &y, &mo, &d, &h, &mi);
  }
  if (!ok_tail && n2 != 5)
  {
    throw std::invalid_argument("bad time \"" + str + "\" (ISO 8601 UTC)");
  }
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
  {
    throw std::invalid_argument("bad time \"" + str + "\"");
  }
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  return static_cast<std::int64_t>(::timegm(&tm));
}

nlohmann::json warnings_json(const std::vector<NameWarning>& w)
{
  nlohmann::json a = nlohmann::json::array();
  for (const auto& x : w)
  {
    a.push_back(
      {{"code", "best_guess"},
       {"argument", x.argument},
       {"asked", x.asked},
       {"used", x.used},
       {"message", "\"" + x.asked + "\" is not a name; used \"" + x.used +
                     "\", the only name containing it"}});
  }
  return a;
}

nlohmann::json to_json(const PointResult& r)
{
  using nlohmann::json;
  json values = json::array();
  for (const auto& v : r.values)
  {
    json e{
      {"model", v.model},
      {"field", v.field},
      {"units", v.units},
      {"status", to_string(v.status)}};
    e["value"] = std::isfinite(v.value) && v.status == PointValue::Status::ok
                   ? json(tidy(v.value))
                   : json(nullptr);
    if (
      v.status != PointValue::Status::no_time &&
      v.status != PointValue::Status::outside_grid)
    {
      e["run"] = iso8601(v.run_s);
      e["fhr"] = v.fhr;
    }
    values.push_back(std::move(e));
  }
  json obs = json::array();
  for (const auto& [source, h] : r.obs)
  {
    json vals = json::array();
    const ValuesView& vv = h.report.values;
    for (std::size_t k = 0; k < vv.size(); ++k)
    {
      const VariableInfo* vi = find_variable(vv.var_code(k));
      json e{
        {"var", vi ? json(vi->name) : json(vv.var_code(k))},
        {"value", tidy(vv.value(k))},
        {"qc", vv.qc(k)},
        {"height_m", tidy(vv.height_m(k))}};
      if (vi)
      {
        e["units"] = vi->units;
      }
      vals.push_back(std::move(e));
    }
    obs.push_back(
      {{"source", source},
       {"distance", tidy(std::sqrt(static_cast<double>(h.d2)))},
       {"station_ref", h.report.station_ref},
       {"obs_time", iso8601(h.report.obs_time_s)},
       {"lat", tidy(h.report.lat_deg)},
       {"lon", tidy(h.report.lon_deg)},
       {"height_m", tidy(h.report.height_m)},
       {"values", std::move(vals)},
       {"raw", std::string(h.report.raw)}});
  }
  json out{{"values", std::move(values)}, {"obs", std::move(obs)}};
  if (!r.warnings.empty())
  {
    out["warnings"] = warnings_json(r.warnings);
  }
  return out;
}

nlohmann::json meta_json(const Snapshot& s, const FieldStore& store)
{
  using nlohmann::json;
  json models = json::array();
  for (const auto& m : store.models())
  {
    const auto times = store.times(m);
    json runs = json::array();
    for (std::size_t k = 0; k < times.size();)
    {
      json hours = json::array();
      const std::int64_t run = times[k].run_s;
      for (; k < times.size() && times[k].run_s == run; ++k)
      {
        hours.push_back(times[k].fhr);
      }
      runs.push_back({{"run", iso8601(run)}, {"fhr", std::move(hours)}});
    }
    json fields = json::array();
    if (!times.empty())
    {
      std::vector<std::string> names;
      for (const auto& f : store.fields(m, times.back()))
      {
        names.push_back(FieldSpec::format(f, store));
      }
      std::sort(names.begin(), names.end());
      for (auto& n : names)
      {
        fields.push_back(std::move(n));
      }
    }
    models.push_back(
      {{"name", m}, {"runs", std::move(runs)}, {"fields", std::move(fields)}});
  }
  json sources = json::array();
  for (const auto& c : s.sources().all())
  {
    sources.push_back(
      {{"name", c.name},
       {"retain_seconds", c.retain_seconds},
       {"L_km", c.L_km},
       {"tau_seconds", c.tau_seconds}});
  }
  json out{
    {"snapshot", s.seq()},
    {"created_at", iso8601(s.manifest().created_at_s)},
    {"models", std::move(models)},
    {"obs_sources", std::move(sources)}};
  out["stations_generation"] =
    s.stations() ? json(s.stations()->generation()) : json(nullptr);
  return out;
}

}  // namespace atmo
