#include "atmo/products.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "atmo/query.h"

namespace atmo
{

namespace
{

using json = nlohmann::json;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr double kPi = 3.14159265358979323846;

std::string lower(std::string_view s)
{
  std::string out(s);
  for (auto& c : out)
  {
    if (c >= 'A' && c <= 'Z')
    {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

std::vector<IoDecl> io_from(const json& j, const char* what)
{
  if (!j.is_array())
  {
    throw std::invalid_argument(std::string("interface ") + what + ": array");
  }
  std::vector<IoDecl> out;
  for (const auto& e : j)
  {
    if (!e.is_object() || !e.contains("role") || !e["role"].is_string())
    {
      throw std::invalid_argument(
        std::string("interface ") + what + ": {role, ...} objects");
    }
    IoDecl d;
    d.role = e["role"].get<std::string>();
    if (e.contains("units") && e["units"].is_string())
    {
      d.units = e["units"].get<std::string>();
    }
    d.needs_alpha = e.value("needs_alpha", false);
    out.push_back(std::move(d));
  }
  return out;
}

std::optional<double> opt_num(const json& e, const char* k)
{
  if (!e.contains(k) || e[k].is_null())
  {
    return std::nullopt;
  }
  if (!e[k].is_number())
  {
    throw std::invalid_argument(std::string("interface param ") + k);
  }
  return e[k].get<double>();
}

bool same_level(const GribLevel& msg, const GribLevel& want)
{
  return msg.type == want.type &&
         static_cast<float>(msg.value) == static_cast<float>(want.value);
}

// ---- built-in processors
// ------------------------------------------------------

void wind_speed(const ProcessorArgs& a)
{
  const float* u = a.inputs[0];
  const float* v = a.inputs[1];
  float* out = a.outputs[0];
  for (std::size_t k = 0; k < a.cells(); ++k)
  {
    out[k] = std::hypot(u[k], v[k]);  // NaN in either -> NaN
  }
}

// Direction the wind blows FROM, degrees true (0 = from north, 90 = from
// east); speeds below `calm_ms` give 0 (calm, as METARs code it).
void wind_direction(const ProcessorArgs& a)
{
  const float* u = a.inputs[0];
  const float* v = a.inputs[1];
  float* out = a.outputs[0];
  const double calm = a.params.empty() ? 0.0 : a.params[0];
  for (std::size_t k = 0; k < a.cells(); ++k)
  {
    if (std::isnan(u[k]) || std::isnan(v[k]))
    {
      out[k] = kNaN;
      continue;
    }
    double east = u[k];
    double north = v[k];
    if (a.alpha)
    {
      rotate_to_earth(a.alpha[k], u[k], v[k], east, north);
    }
    if (std::hypot(east, north) < calm)
    {
      out[k] = 0.0f;
      continue;
    }
    double deg = std::atan2(-east, -north) * 180.0 / kPi;
    if (deg <= 0.0)
    {
      deg += 360.0;  // 360 = from north (0 is kept for calm)
    }
    out[k] = static_cast<float>(deg);
  }
}

std::vector<BuiltinProcessor> make_builtins()
{
  std::vector<BuiltinProcessor> b;
  {
    BuiltinProcessor p;
    p.name = "wind_speed";
    p.version = 1;
    p.description =
      "Wind speed from u and v (grid- or earth-relative: the magnitude is "
      "the same).";
    p.iface.inputs = {{"u", "m/s", false}, {"v", "m/s", false}};
    p.iface.outputs = {{"speed", "m/s", false}};
    p.fn = wind_speed;
    b.push_back(std::move(p));
  }
  {
    BuiltinProcessor p;
    p.name = "wind_direction";
    p.version = 1;
    p.description =
      "Direction the wind blows from, degrees true (360 = north, 0 = calm); "
      "grid-relative winds are rotated to earth first.";
    p.iface.inputs = {{"u", "m/s", true}, {"v", "m/s", false}};
    p.iface.outputs = {{"dir", "deg", false}};
    ParamDecl calm;
    calm.name = "calm_ms";
    calm.def = 0.5;
    calm.min = 0.0;
    calm.max = 5.0;
    p.iface.params = {calm};
    p.fn = wind_direction;
    b.push_back(std::move(p));
  }
  return b;
}

}  // namespace

// ---- definitions
// --------------------------------------------------------------

std::optional<OffsetRule> offset_rule_from(std::string_view s)
{
  if (s == "same")
  {
    return OffsetRule::same;
  }
  if (s == "previous_step")
  {
    return OffsetRule::previous_step;
  }
  if (s == "own_previous")
  {
    return OffsetRule::own_previous;
  }
  return std::nullopt;
}

ProductInterface ProductInterface::from_json(
  const std::string& inputs,
  const std::string& outputs,
  const std::string& params)
{
  ProductInterface f;
  try
  {
    f.inputs = io_from(json::parse(inputs), "inputs");
    f.outputs = io_from(json::parse(outputs), "outputs");
    const json p = json::parse(params);
    if (!p.is_array())
    {
      throw std::invalid_argument("interface params: array");
    }
    for (const auto& e : p)
    {
      if (!e.is_object() || !e.contains("name") || !e["name"].is_string())
      {
        throw std::invalid_argument("interface params: {name, ...} objects");
      }
      ParamDecl d;
      d.name = e["name"].get<std::string>();
      d.is_int = e.value("type", std::string("float")) == "int";
      d.def = opt_num(e, "default");
      d.min = opt_num(e, "min");
      d.max = opt_num(e, "max");
      f.params.push_back(std::move(d));
    }
  }
  catch (const json::exception& e)
  {
    throw std::invalid_argument(std::string("interface: ") + e.what());
  }
  return f;
}

void ProductInterface::to_json(
  std::string& in,
  std::string& out,
  std::string& par) const
{
  auto io = [](const std::vector<IoDecl>& v)
  {
    json a = json::array();
    for (const auto& d : v)
    {
      json e = {{"role", d.role}, {"units", d.units}};
      if (d.needs_alpha)
      {
        e["needs_alpha"] = true;
      }
      a.push_back(std::move(e));
    }
    return a.dump();
  };
  in = io(inputs);
  out = io(outputs);
  json a = json::array();
  for (const auto& d : params)
  {
    json e = {{"name", d.name}, {"type", d.is_int ? "int" : "float"}};
    if (d.def)
    {
      e["default"] = *d.def;
    }
    if (d.min)
    {
      e["min"] = *d.min;
    }
    if (d.max)
    {
      e["max"] = *d.max;
    }
    a.push_back(std::move(e));
  }
  par = a.dump();
}

bool ProductInterface::needs_alpha() const
{
  return std::any_of(
    inputs.begin(), inputs.end(),
    [](const IoDecl& d) { return d.needs_alpha; });
}

const ProductOutput* Product::output(std::string_view role) const
{
  for (const auto& o : outputs)
  {
    if (o.role == role)
    {
      return &o;
    }
  }
  return nullptr;
}

const std::vector<BuiltinProcessor>& builtin_processors()
{
  static const std::vector<BuiltinProcessor> b = make_builtins();
  return b;
}

const BuiltinProcessor* find_builtin(std::string_view name)
{
  for (const auto& p : builtin_processors())
  {
    if (p.name == name)
    {
      return &p;
    }
  }
  return nullptr;
}

std::vector<double> param_values(
  const ProductInterface& iface,
  const std::map<std::string, double>& given)
{
  std::vector<double> out;
  for (const auto& d : iface.params)
  {
    auto it = given.find(d.name);
    if (it != given.end())
    {
      out.push_back(d.is_int ? std::round(it->second) : it->second);
    }
    else if (d.def)
    {
      out.push_back(*d.def);
    }
    else
    {
      throw std::invalid_argument("param " + d.name + " has no value");
    }
  }
  return out;
}

KernelExecutor::~KernelExecutor() = default;

bool JobPlan::same_job(const JobPlan& o) const
{
  return product_id == o.product_id && run_id == o.run_id &&
         offset_end_s == o.offset_end_s &&
         product_revision == o.product_revision &&
         processor_version == o.processor_version &&
         kernel_revision == o.kernel_revision;
}

// ---- matching
// -----------------------------------------------------------------

ProductMatcher::ProductMatcher(
  std::shared_ptr<const CatalogTables> tables,
  const CatalogFieldStore& names,
  CatalogQuery& catalog,
  std::vector<Product> products,
  Versions versions)
    : t_(std::move(tables)), names_(names), catalog_(catalog)
{
  for (const auto& p : products)
  {
    slugs_[lower(p.slug)] = p.process_id;  // disabled ones too: still outputs
  }
  for (auto& p : products)
  {
    if (!p.enabled)
    {
      continue;
    }
    Entry e;
    e.p = std::move(p);
    auto v = versions.find(e.p.id);
    if (v != versions.end())
    {
      e.versions = v->second;
    }
    for (std::size_t k = 0; k < e.p.inputs.size(); ++k)
    {
      e.in.push_back(resolve(e.p.inputs[k]));
      if (e.primary < 0 && e.p.inputs[k].rule != OffsetRule::own_previous)
      {
        e.primary = static_cast<int>(k);
      }
    }
    entries_.push_back(std::move(e));
  }
}

std::set<std::uint64_t> ProductMatcher::product_grids(
  std::uint32_t process_id) const
{
  std::set<std::uint64_t> out;
  for (const auto& m : t_->models)
  {
    if (
      m.center_code == 254 && m.subcenter_code == 0 &&
      m.process_id == process_id)
    {
      for (const auto& mg : t_->model_grids)
      {
        if (mg.model_id == m.id)
        {
          out.insert(mg.id);
        }
      }
    }
  }
  return out;
}

std::set<std::uint64_t> ProductMatcher::output_grids(
  std::uint64_t product_id) const
{
  const Entry* e = entry(product_id);
  return e ? product_grids(e->p.process_id) : std::set<std::uint64_t>{};
}

ResolvedInput ProductMatcher::resolve(const ProductInput& in) const
{
  ResolvedInput r;
  if (in.rule == OffsetRule::own_previous)
  {
    return r;  // resolved per job against the product's own outputs
  }
  // Model: a product slug (its outputs), else an exact api_name.
  const std::string want = lower(in.model);
  auto slug = slugs_.find(want);
  if (slug != slugs_.end())
  {
    r.model_grids = product_grids(slug->second);
  }
  else
  {
    for (const auto& mg : t_->model_grids)
    {
      if (lower(mg.api_name) == want)
      {
        r.model_grids.insert(mg.id);
      }
    }
  }
  if (r.model_grids.empty())
  {
    r.problem = "model \"" + in.model + "\" is not in the catalog (yet)";
  }
  // Field: exact name and a fixed level.
  FieldSpec spec;
  try
  {
    spec = FieldSpec::parse(in.field);
  }
  catch (const std::exception& e)
  {
    r.problem = "field \"" + in.field + "\": " + e.what();
    return r;
  }
  if (spec.level != FieldSpec::Level::fixed)
  {
    r.problem = "field \"" + in.field + "\" has no @level";
    return r;
  }
  r.level.type = spec.level_type;
  r.level.value = spec.level_value;
  try
  {
    const ResolvedName n = names_.resolve_field(spec.name);
    if (n.guess)
    {
      r.problem = "field \"" + spec.name +
                  "\" is not an exact name (best guess \"" + n.name +
                  "\" is not used for products)";
      return r;
    }
    for (auto s : names_.signatures_of(n.param))
    {
      r.signatures.insert(s);
    }
  }
  catch (const std::exception& e)
  {
    if (r.problem.empty())
    {
      r.problem = "field \"" + spec.name + "\": " + e.what();
    }
  }
  return r;
}

const ProductMatcher::Entry* ProductMatcher::entry(
  std::uint64_t product_id) const
{
  for (const auto& e : entries_)
  {
    if (e.p.id == product_id)
    {
      return &e;
    }
  }
  return nullptr;
}

std::map<std::uint64_t, std::vector<std::string>> ProductMatcher::problems()
  const
{
  std::map<std::uint64_t, std::vector<std::string>> out;
  for (const auto& e : entries_)
  {
    for (std::size_t k = 0; k < e.in.size(); ++k)
    {
      if (!e.in[k].problem.empty())
      {
        out[e.p.id].push_back(
          "input " + e.p.inputs[k].role + ": " + e.in[k].problem);
      }
    }
  }
  return out;
}

const CatRun* ProductMatcher::run_like(
  std::uint64_t model_grid_id,
  std::int64_t basetime_s,
  int member) const
{
  for (const auto& r : t_->runs)
  {
    if (
      r.model_grid_id == model_grid_id && r.basetime_s == basetime_s &&
      r.member == member)
    {
      return &r;
    }
  }
  return nullptr;
}

std::vector<CatMessage>& ProductMatcher::at(
  std::uint64_t run_id,
  std::int64_t offset)
{
  const auto key = std::make_pair(run_id, offset);
  auto it = memo_.find(key);
  if (it == memo_.end())
  {
    it = memo_.emplace(key, catalog_.messages_at(*t_, run_id, offset)).first;
  }
  return it->second;
}

std::vector<CatMessage> ProductMatcher::find(
  std::uint64_t run_id,
  std::int64_t offset,
  const std::set<std::uint64_t>& sigs,
  const GribLevel& level)
{
  std::vector<CatMessage> out;
  if (offset < 0)
  {
    return out;
  }
  for (const auto& m : at(run_id, offset))
  {
    if (
      sigs.count(m.signature_id) && same_level(m.level1, level) &&
      m.offset_start_s == m.offset_end_s)
    {
      out.push_back(m);
    }
  }
  if (!out.empty())
  {
    return out;
  }
  // Statistical fields (start < end) only when nothing instantaneous fits.
  for (const auto& m : at(run_id, offset))
  {
    if (sigs.count(m.signature_id) && same_level(m.level1, level))
    {
      out.push_back(m);
    }
  }
  return out;
}

std::optional<std::uint64_t> ProductMatcher::own_previous(
  const Entry& e,
  const ProductInput& in,
  const CatRun& run,
  std::int64_t offset,
  bool& wait,
  std::string* why)
{
  wait = false;
  const ProductOutput* out = e.p.output(in.own_output);
  if (!out)
  {
    wait = true;
    if (why)
    {
      *why = "own_previous names no output of the product";
    }
    return std::nullopt;
  }
  const std::set<std::uint64_t> grids = product_grids(e.p.process_id);
  std::set<std::uint64_t> sigs;
  for (const auto& s : t_->signatures)
  {
    if (
      s.discipline == out->discipline && s.category == out->category &&
      s.number == out->number && s.pdt == 0)
    {
      sigs.insert(s.id);
    }
  }
  const std::int64_t prev = offset - in.step_s;
  const std::int64_t want_valid = run.basetime_s + prev;

  // Within the run: if the primary input exists one step back, the
  // product's output for that step must come first.
  const ResolvedInput& pr = e.in[e.primary];
  if (prev >= 0 && !find(run.id, prev, pr.signatures, pr.level).empty())
  {
    for (auto g : grids)
    {
      const CatRun* own = run_like(g, run.basetime_s, run.member);
      if (own)
      {
        auto m = find(own->id, prev, sigs, out->level1);
        if (!m.empty())
        {
          return m.front().id;
        }
      }
    }
    wait = true;
    if (why)
    {
      *why = "own output at the previous step not made yet";
    }
    return std::nullopt;
  }
  // The run's first step: the newest earlier run's output valid then.
  const CatRun* best = nullptr;
  std::optional<std::uint64_t> best_msg;
  for (const auto& r : t_->runs)
  {
    if (
      !grids.count(r.model_grid_id) || r.member != run.member ||
      r.basetime_s >= run.basetime_s || r.basetime_s > want_valid)
    {
      continue;
    }
    if (best && best->basetime_s >= r.basetime_s)
    {
      continue;
    }
    auto m = find(r.id, want_valid - r.basetime_s, sigs, out->level1);
    if (!m.empty())
    {
      best = &r;
      best_msg = m.front().id;
    }
  }
  return best_msg;  // nullopt and !wait: chain start
}

std::optional<JobPlan> ProductMatcher::ready(
  std::uint64_t product_id,
  std::uint64_t run_id,
  std::int64_t offset,
  std::string* why)
{
  const Entry* e = entry(product_id);
  const CatRun* run = t_->run(run_id);
  if (!e || !run || e->primary < 0 || offset < 0)
  {
    if (why)
    {
      *why = "no such product, run or offset";
    }
    return std::nullopt;
  }
  JobPlan plan;
  plan.product_id = e->p.id;
  plan.product_revision = e->p.revision;
  plan.processor_version = e->versions.first;
  plan.kernel_revision = e->versions.second;
  plan.run_id = run_id;
  plan.offset_end_s = offset;
  for (std::size_t k = 0; k < e->p.inputs.size(); ++k)
  {
    const ProductInput& in = e->p.inputs[k];
    const ResolvedInput& r = e->in[k];
    if (in.rule == OffsetRule::own_previous)
    {
      bool wait = false;
      auto id = own_previous(*e, in, *run, offset, wait, why);
      if (id)
      {
        plan.inputs[in.role] = *id;
      }
      else if (wait)
      {
        return std::nullopt;
      }
      else
      {
        plan.chain_start = true;
      }
      continue;
    }
    if (!r.problem.empty())
    {
      if (why)
      {
        *why = "input " + in.role + ": " + r.problem;
      }
      return std::nullopt;
    }
    const std::int64_t at_off =
      in.rule == OffsetRule::previous_step ? offset - in.step_s : offset;
    std::vector<CatMessage> hits;
    for (auto g : r.model_grids)
    {
      const CatRun* rr = g == run->model_grid_id
                           ? run
                           : run_like(g, run->basetime_s, run->member);
      if (!rr)
      {
        continue;
      }
      auto m = find(rr->id, at_off, r.signatures, r.level);
      hits.insert(hits.end(), m.begin(), m.end());
    }
    if (hits.size() > 1)
    {
      if (why)
      {
        *why = "input " + in.role + ": " + std::to_string(hits.size()) +
               " messages match (ambiguous)";
      }
      return std::nullopt;
    }
    if (hits.empty())
    {
      if (in.required)
      {
        if (why)
        {
          *why = "input " + in.role + " not there yet";
        }
        return std::nullopt;
      }
      continue;
    }
    plan.inputs[in.role] = hits.front().id;
  }
  // The source run must be the primary input's: a product whose primary
  // input lives on another model grid is planned under that grid's run.
  return plan;
}

std::vector<JobPlan> ProductMatcher::plan(
  const std::vector<ArrivedMessage>& arrived)
{
  memo_.clear();
  // Candidate (product, source run, offset) triples.
  std::set<std::tuple<std::uint64_t, std::uint64_t, std::int64_t>> cand;
  for (const auto& m : arrived)
  {
    const CatRun* run = t_->run(m.run_id);
    if (!run)
    {
      continue;
    }
    for (const auto& e : entries_)
    {
      if (e.primary < 0)
      {
        continue;
      }
      const ResolvedInput& pr = e.in[e.primary];
      // Primary runs with this message's base time and member.
      auto primary_runs = [&]()
      {
        std::vector<const CatRun*> out;
        for (auto g : pr.model_grids)
        {
          if (const CatRun* r = run_like(g, run->basetime_s, run->member))
          {
            out.push_back(r);
          }
        }
        return out;
      };
      for (std::size_t k = 0; k < e.p.inputs.size(); ++k)
      {
        const ProductInput& in = e.p.inputs[k];
        const ResolvedInput& r = e.in[k];
        if (in.rule == OffsetRule::own_previous)
        {
          // One of our own outputs: the next step of the same base time,
          // and the first step of later runs that start from it.
          const auto grids = product_grids(e.p.process_id);
          if (!grids.count(run->model_grid_id))
          {
            continue;
          }
          const std::int64_t next = m.offset_end_s + in.step_s;
          for (const CatRun* pr_run : primary_runs())
          {
            cand.emplace(e.p.id, pr_run->id, next);
          }
          const std::int64_t valid = run->basetime_s + next;
          for (const auto& later : t_->runs)
          {
            if (
              pr.model_grids.count(later.model_grid_id) &&
              later.member == run->member &&
              later.basetime_s > run->basetime_s && later.basetime_s <= valid)
            {
              cand.emplace(e.p.id, later.id, valid - later.basetime_s);
            }
          }
          continue;
        }
        if (
          !r.model_grids.count(run->model_grid_id) ||
          !r.signatures.count(m.signature_id) || !same_level(m.level1, r.level))
        {
          continue;
        }
        const std::int64_t off = in.rule == OffsetRule::previous_step
                                   ? m.offset_end_s + in.step_s
                                   : m.offset_end_s;
        for (const CatRun* pr_run : primary_runs())
        {
          cand.emplace(e.p.id, pr_run->id, off);
        }
      }
    }
  }
  std::vector<JobPlan> out;
  for (const auto& [pid, rid, off] : cand)
  {
    if (auto p = ready(pid, rid, off))
    {
      out.push_back(std::move(*p));
    }
  }
  return out;
}

std::vector<JobPlan> ProductMatcher::plan_all(std::uint64_t product_id)
{
  memo_.clear();
  const Entry* e = entry(product_id);
  std::vector<JobPlan> out;
  if (!e || e->primary < 0)
  {
    return out;
  }
  const ProductInput& pin = e->p.inputs[e->primary];
  const ResolvedInput& pr = e->in[e->primary];
  // Oldest runs first, so own_previous chains build up in order.
  std::vector<const CatRun*> runs;
  for (const auto& r : t_->runs)
  {
    if (pr.model_grids.count(r.model_grid_id))
    {
      runs.push_back(&r);
    }
  }
  std::sort(
    runs.begin(), runs.end(),
    [](const CatRun* a, const CatRun* b)
    {
      return a->basetime_s != b->basetime_s ? a->basetime_s < b->basetime_s
                                            : a->id < b->id;
    });
  for (const CatRun* r : runs)
  {
    std::set<std::int64_t> offs;
    for (auto o : catalog_.run_offsets(*t_, r->id))
    {
      offs.insert(pin.rule == OffsetRule::previous_step ? o + pin.step_s : o);
    }
    for (auto o : offs)
    {
      if (auto p = ready(e->p.id, r->id, o))
      {
        out.push_back(std::move(*p));
      }
    }
  }
  return out;
}

}  // namespace atmo
