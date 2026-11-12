// /v1/grib/* over the grid catalog (T17).  See include/atmo/grib_api.h and
// docs/api/query-api-v1.md "Grid catalog".
#include "atmo/grib_api.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

#include "atmo/catalog.h"
#include "atmo/colormap.h"
#include "atmo/gridstore.h"
#include "atmo/query.h"
#include "atmo/tiles.h"
#include "atmo/variable.h"

namespace atmo
{

namespace
{

using nlohmann::json;

struct ApiError : std::runtime_error
{
  int status;
  std::string code;
  ApiError(int s, std::string c, const std::string& msg)
      : std::runtime_error(msg), status(s), code(std::move(c))
  {
  }
};

template <class T>
const T* by_id(const std::vector<T>& v, std::uint64_t id)
{
  auto it = std::lower_bound(
    v.begin(), v.end(), id,
    [](const T& x, std::uint64_t k) { return x.id < k; });
  return it != v.end() && it->id == id ? &*it : nullptr;
}

// Shortest decimal that reads back as the same float (API convention).
json tidy(double v)
{
  if (!std::isfinite(v))
  {
    return nullptr;
  }
  const float f = static_cast<float>(v);
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, f);
  double out = 0.0;
  std::from_chars(buf, r.ptr, out);
  return out;
}

std::uint64_t parse_id(const std::string& s, const char* what)
{
  std::uint64_t n = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
  if (
    s.empty() || r.ec != std::errc() || r.ptr != s.data() + s.size() || n == 0)
  {
    throw ApiError(400, "bad_request", std::string("bad ") + what);
  }
  return n;
}

std::optional<std::uint64_t> opt_id(
  const std::map<std::string, std::string>& q,
  const char* key)
{
  auto it = q.find(key);
  if (it == q.end())
  {
    return std::nullopt;
  }
  return parse_id(it->second, key);
}

std::uint64_t req_id(
  const std::map<std::string, std::string>& q,
  const char* key)
{
  auto v = opt_id(q, key);
  if (!v)
  {
    throw ApiError(400, "bad_request", std::string("missing ") + key);
  }
  return *v;
}

std::uint64_t parse_hash(const std::string& s)
{
  std::uint64_t n = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), n, 16);
  if (s.size() != 16 || r.ec != std::errc() || r.ptr != s.data() + s.size())
  {
    throw ApiError(400, "bad_request", "bad signature");
  }
  return n;
}

GribLevel parse_level(const std::string& s, const char* what)
{
  const auto colon = s.find(':');
  GribLevel l;
  if (colon == std::string::npos)
  {
    throw ApiError(400, "bad_request", std::string("bad ") + what);
  }
  std::uint32_t t = 0;
  const auto a = std::from_chars(s.data(), s.data() + colon, t);
  double v = 0.0;
  const auto b = std::from_chars(s.data() + colon + 1, s.data() + s.size(), v);
  if (
    a.ec != std::errc() || a.ptr != s.data() + colon || t > 255 ||
    b.ec != std::errc() || b.ptr != s.data() + s.size() || !std::isfinite(v))
  {
    throw ApiError(400, "bad_request", std::string("bad ") + what);
  }
  l.type = t;
  l.value = v;
  return l;
}

json level_json(const GribLevel& l)
{
  if (l.type == 255)
  {
    return nullptr;
  }
  return json{{"type", l.type}, {"value", l.value}};
}

json grid_json(const CatGrid& g)
{
  return json{
    {"key", key_hex(g.key)},
    {"type", g.type},
    {"ni", g.ni},
    {"nj", g.nj},
    {"periodic_i", g.periodic_i},
    {"bbox", {g.west, g.south, g.east, g.north}},
    {"viewable", g.viewable}};
}

json parameter_json(const CatSignature& s)
{
  return json{
    {"signature", key_hex(s.hash)},
    {"name", s.name},
    {"short_name", s.short_name},
    {"units", s.units},
    {"status", s.status},
    {"stat_interval_s",
     s.stat_interval_s ? json(*s.stat_interval_s) : json(nullptr)},
    {"base_signature", key_hex(s.base_hash)},
    {"discipline", s.discipline},
    {"category", s.category},
    {"number", s.number},
    {"pdt", s.pdt}};
}

json flags_json(std::uint32_t f)
{
  json a = json::array();
  if (f & kCatalogUndecodable)
  {
    a.push_back("undecodable");
  }
  if (f & kCatalogGridUnsupported)
  {
    a.push_back("grid_unsupported");
  }
  return a;
}

fcgi::Response respond(int status, const json& body, std::uint64_t gen)
{
  fcgi::Response r;
  r.status = status;
  r.content_type = "application/json";
  r.body = body.dump();
  r.headers.push_back({"Cache-Control", "no-store"});
  r.headers.push_back({"X-Atmo-Generation", std::to_string(gen)});
  return r;
}

// The bytes of one catalogued message, checked against its content hash.
std::string read_message(
  const std::filesystem::path& file,
  std::uint64_t offset,
  std::uint64_t length,
  std::uint64_t hash)
{
  std::ifstream in(file, std::ios::binary);
  std::string buf(length, '\0');
  in.seekg(static_cast<std::streamoff>(offset));
  in.read(buf.data(), static_cast<std::streamsize>(length));
  if (
    !in || static_cast<std::uint64_t>(in.gcount()) != length ||
    fnv1a64(buf.data(), buf.size()) != hash)
  {
    throw std::runtime_error(
      "message at " + file.string() + ":" + std::to_string(offset) +
      " does not match the catalog");
  }
  return buf;
}

std::vector<std::string> split_path(std::string_view p)
{
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i <= p.size())
  {
    std::size_t e = p.find('/', i);
    if (e == std::string_view::npos)
    {
      e = p.size();
    }
    out.emplace_back(p.substr(i, e - i));
    i = e + 1;
  }
  return out;
}

// Native values of one decoded message, as a FieldView.
class MessageView : public FieldView
{
 public:
  MessageView(std::shared_ptr<const GribMessageData> d, FieldInfo info)
      : FieldView(d->grid, info), d_(std::move(d))
  {
  }
  double at(std::uint32_t i, std::uint32_t j) const override
  {
    return d_->values[static_cast<std::size_t>(j) * grid().ni() + i];
  }

 private:
  std::shared_ptr<const GribMessageData> d_;
};

// One message as a FieldStore, so render_tile() can draw it.  The values
// are already in the parameter's units (see sampleable()).
class OneMessageStore : public FieldStore
{
 public:
  OneMessageStore(std::shared_ptr<const FieldView> v) : v_(std::move(v)) {}
  std::vector<std::string> models() const override { return {"m"}; }
  std::vector<ModelTime> times(std::string_view) const override
  {
    return {ModelTime{}};
  }
  std::shared_ptr<const Grid> grid(std::string_view, const ModelTime&)
    const override
  {
    return std::shared_ptr<const Grid>(v_, &v_->grid());
  }
  std::vector<FieldInfo> fields(std::string_view, const ModelTime&)
    const override
  {
    return {v_->info()};
  }
  float sample(
    std::string_view,
    const ModelTime&,
    const FieldKey&,
    const Grid::Cell& cell) const override
  {
    return static_cast<float>(v_->sample(cell));
  }
  std::shared_ptr<const FieldView>
  view(std::string_view, const ModelTime&, const FieldKey&) const override
  {
    return v_;
  }
  ParamInfo param(std::uint16_t) const override
  {
    return {"message", "", ParamRole::other};
  }

 private:
  std::shared_ptr<const FieldView> v_;
};

// Values in a decoded copy, scaled to the parameter's units.
class ScaledView : public FieldView
{
 public:
  ScaledView(
    std::shared_ptr<const GribMessageData> d,
    FieldInfo info,
    double scale,
    double offset)
      : FieldView(d->grid, info),
        d_(std::move(d)),
        scale_(scale),
        offset_(offset)
  {
  }
  double at(std::uint32_t i, std::uint32_t j) const override
  {
    return d_->values[static_cast<std::size_t>(j) * grid().ni() + i] * scale_ +
           offset_;
  }

 private:
  std::shared_ptr<const GribMessageData> d_;
  double scale_, offset_;
};

}  // namespace

// ---- CatalogTables ---------------------------------------------------------

const CatCenter* CatalogTables::center(std::uint64_t id) const
{
  return by_id(centers, id);
}
const CatModel* CatalogTables::model(std::uint64_t id) const
{
  return by_id(models, id);
}
const CatModelGrid* CatalogTables::model_grid(std::uint64_t id) const
{
  return by_id(model_grids, id);
}
const CatGrid* CatalogTables::grid(std::uint64_t id) const
{
  return by_id(grids, id);
}
const CatRun* CatalogTables::run(std::uint64_t id) const
{
  return by_id(runs, id);
}
const CatSignature* CatalogTables::signature(std::uint64_t id) const
{
  return by_id(signatures, id);
}
const CatSignature* CatalogTables::signature_by_hash(std::uint64_t h) const
{
  for (const auto& s : signatures)
  {
    if (s.hash == h)
    {
      return &s;
    }
  }
  return nullptr;
}

void CatalogTables::sort()
{
  auto by = [](auto& v)
  {
    std::sort(
      v.begin(), v.end(),
      [](const auto& a, const auto& b) { return a.id < b.id; });
  };
  by(centers);
  by(models);
  by(model_grids);
  by(grids);
  by(runs);
  by(signatures);
}

CatalogQuery::~CatalogQuery() = default;

// ---- GribApi ---------------------------------------------------------------

GribApi::GribApi(
  std::filesystem::path root,
  CatalogQuery& catalog,
  GribCache* cache,
  GribValuesDecoder values)
    : root_(std::move(root)),
      catalog_(catalog),
      cache_(cache),
      values_(std::move(values))
{
}

std::shared_ptr<const FieldView> GribApi::decode(
  const CatMessage& msg,
  const CatGrid& grid,
  const CatSignature& sig,
  FieldInfo info) const
{
  if (!cache_)
  {
    return nullptr;
  }
  const auto g = load_grid(grid.key);
  if (!g)
  {
    return nullptr;
  }
  GribIndexEntry e;
  e.offset = msg.byte_offset;
  e.length = msg.byte_length;
  e.hash = msg.content_hash;
  e.info.param_id = kNativeParam;
  e.grid = grid.key;
  const auto data = cache_->get(root_ / msg.file_path, e, g);
  if (data->uv_relative_to_grid)
  {
    info.flags |= kFieldGridRelative;
  }
  return std::make_shared<ScaledView>(data, info, sig.si_scale, sig.si_offset);
}

std::optional<bool> GribApi::uv_relative_to_grid(
  const CatMessage& msg,
  const CatGrid& grid,
  const CatSignature& sig) const
{
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = uv_relative_.find(grid.key);
    if (it != uv_relative_.end())
    {
      return it->second;
    }
  }
  const auto v = decode(msg, grid, sig, FieldInfo{});
  if (!v)
  {
    return std::nullopt;
  }
  const bool rel = (v->info().flags & kFieldGridRelative) != 0;
  std::lock_guard<std::mutex> lk(mu_);
  uv_relative_[grid.key] = rel;
  return rel;
}

std::shared_ptr<const CatalogFieldStore> GribApi::field_store() const
{
  const auto t = catalog_.tables();
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (store_ && store_->generation() == t->generation)
    {
      return store_;
    }
  }
  auto s = std::make_shared<const CatalogFieldStore>(t, catalog_, *this);
  std::lock_guard<std::mutex> lk(mu_);
  store_ = s;
  return s;
}

GribApi::~GribApi() = default;

bool GribApi::handles(std::string_view path)
{
  return path.rfind("/v1/grib/", 0) == 0;
}

std::shared_ptr<const Grid> GribApi::load_grid(GridKey key) const
{
  std::lock_guard<std::mutex> lk(mu_);
  auto it = grids_.find(key);
  if (it != grids_.end())
  {
    return it->second;
  }
  auto g = GridStore(root_ / "grids").get(key);
  grids_[key] = g;
  return g;
}

fcgi::Response GribApi::handle(
  const std::string& path,
  const std::map<std::string, std::string>& q,
  const fcgi::Request& req) const
{
  std::uint64_t gen = 0;
  try
  {
    const auto tp = catalog_.tables();
    const CatalogTables& t = *tp;
    gen = t.generation;
    const std::vector<std::string> seg =
      split_path(std::string_view(path).substr(9));  // after /v1/grib/
    const std::string& what = seg[0];
    json body;
    body["generation"] = gen;

    if (seg.size() == 1 && what == "centers")
    {
      json a = json::array();
      std::vector<const CatCenter*> cs;
      for (const auto& c : t.centers)
      {
        cs.push_back(&c);
      }
      std::sort(
        cs.begin(), cs.end(),
        [](auto* x, auto* y) { return x->code < y->code; });
      for (const auto* c : cs)
      {
        json subs = json::array();
        for (const auto& s : c->subcenters)
        {
          subs.push_back({{"id", s.id}, {"code", s.code}, {"name", s.name}});
        }
        a.push_back(
          {{"id", c->id},
           {"code", c->code},
           {"name", c->name},
           {"subcenters", subs}});
      }
      body["centers"] = a;
      return respond(200, body, gen);
    }
    if (seg.size() == 1 && what == "models")
    {
      const auto center = opt_id(q, "center");
      const auto sub = opt_id(q, "subcenter");
      std::vector<const CatModel*> ms;
      for (const auto& m : t.models)
      {
        if (
          (!center || m.center_id == *center) &&
          (!sub || m.subcenter_id == *sub))
        {
          ms.push_back(&m);
        }
      }
      std::sort(
        ms.begin(), ms.end(),
        [](auto* a, auto* b)
        {
          return std::tie(a->center_code, a->subcenter_code, a->process_id) <
                 std::tie(b->center_code, b->subcenter_code, b->process_id);
        });
      json a = json::array();
      for (const auto* m : ms)
      {
        a.push_back(
          {{"id", m->id},
           {"center", m->center_code},
           {"subcenter", m->subcenter_code},
           {"process_id", m->process_id},
           {"slug", m->slug},
           {"name", m->name}});
      }
      body["models"] = a;
      return respond(200, body, gen);
    }
    if (seg.size() == 1 && what == "grids")
    {
      const std::uint64_t model = req_id(q, "model");
      if (!t.model(model))
      {
        throw ApiError(404, "not_found", "unknown model");
      }
      std::vector<const CatModelGrid*> mgs;
      for (const auto& mg : t.model_grids)
      {
        if (mg.model_id == model)
        {
          mgs.push_back(&mg);
        }
      }
      std::sort(
        mgs.begin(), mgs.end(), [](auto* a, auto* b)
        { return std::tie(a->name, a->id) < std::tie(b->name, b->id); });
      json a = json::array();
      for (const auto* mg : mgs)
      {
        const CatGrid* g = t.grid(mg->grid_id);
        a.push_back(
          {{"id", mg->id},
           {"name", mg->name},
           {"api_name", mg->api_name},
           {"grid", g ? grid_json(*g) : json(nullptr)}});
      }
      body["grids"] = a;
      return respond(200, body, gen);
    }
    if (seg.size() == 1 && what == "runs")
    {
      const std::uint64_t mg = req_id(q, "model_grid");
      if (!t.model_grid(mg))
      {
        throw ApiError(404, "not_found", "unknown model_grid");
      }
      std::vector<const CatRun*> rs;
      for (const auto& r : t.runs)
      {
        if (r.model_grid_id == mg)
        {
          rs.push_back(&r);
        }
      }
      std::sort(
        rs.begin(), rs.end(),
        [](auto* a, auto* b)
        {
          return std::make_tuple(-a->basetime_s, a->member) <
                 std::make_tuple(-b->basetime_s, b->member);
        });
      json a = json::array();
      for (const auto* r : rs)
      {
        a.push_back(
          {{"id", r->id},
           {"basetime", iso8601(r->basetime_s)},
           {"member", r->member},
           {"member_type",
            r->member_type ? json(*r->member_type) : json(nullptr)}});
      }
      body["runs"] = a;
      return respond(200, body, gen);
    }

    // The rest need a run (and a signature).
    auto need_run = [&]() -> const CatRun&
    {
      const CatRun* r = t.run(req_id(q, "run"));
      if (!r)
      {
        throw ApiError(404, "not_found", "unknown run");
      }
      return *r;
    };
    auto need_sig = [&]() -> const CatSignature&
    {
      auto it = q.find("signature");
      if (it == q.end())
      {
        throw ApiError(400, "bad_request", "missing signature");
      }
      const CatSignature* s = t.signature_by_hash(parse_hash(it->second));
      if (!s)
      {
        throw ApiError(404, "not_found", "unknown signature");
      }
      return *s;
    };

    if (seg.size() == 1 && what == "parameters")
    {
      const CatRun& run = need_run();
      std::vector<const CatSignature*> ss;
      for (std::uint64_t id : catalog_.run_signatures(t, run.id))
      {
        if (const CatSignature* s = t.signature(id))
        {
          ss.push_back(s);
        }
      }
      std::sort(
        ss.begin(), ss.end(), [](auto* a, auto* b)
        { return std::tie(a->name, a->hash) < std::tie(b->name, b->hash); });
      json a = json::array();
      for (const auto* s : ss)
      {
        a.push_back(parameter_json(*s));
      }
      body["parameters"] = a;
      return respond(200, body, gen);
    }
    if (seg.size() == 1 && what == "levels")
    {
      const CatRun& run = need_run();
      const CatSignature& sig = need_sig();
      std::set<std::tuple<std::uint32_t, double, std::uint32_t, double>> ls;
      for (const auto& m : catalog_.messages(t, run.id, sig.id))
      {
        ls.insert(
          {m.level1.type, m.level1.value, m.level2.type, m.level2.value});
      }
      json a = json::array();
      for (const auto& [t1, v1, t2, v2] : ls)
      {
        a.push_back(
          {{"level1", level_json({t1, v1})}, {"level2", level_json({t2, v2})}});
      }
      body["levels"] = a;
      return respond(200, body, gen);
    }
    if (seg.size() == 1 && what == "offsets")
    {
      const CatRun& run = need_run();
      const CatSignature& sig = need_sig();
      if (!q.count("level"))
      {
        throw ApiError(400, "bad_request", "missing level");
      }
      const GribLevel l1 = parse_level(q.at("level"), "level");
      const GribLevel l2 =
        q.count("level2") ? parse_level(q.at("level2"), "level2") : GribLevel{};
      struct Off
      {
        const CatMessage* m;
        bool repaired;
      };
      std::vector<CatMessage> own = catalog_.messages(t, run.id, sig.id);
      std::vector<CatMessage> extra;
      // First-bucket repair: the run-total sibling's 0..N message.
      if (sig.stat_interval_s && *sig.stat_interval_s > 0)
      {
        for (const auto& s : t.signatures)
        {
          if (
            s.base_hash == sig.base_hash && s.stat_interval_s &&
            *s.stat_interval_s == 0)
          {
            for (auto& m : catalog_.messages(t, run.id, s.id))
            {
              if (
                m.offset_start_s == 0 &&
                m.offset_end_s ==
                  static_cast<std::int64_t>(*sig.stat_interval_s))
              {
                extra.push_back(std::move(m));
              }
            }
          }
        }
      }
      std::vector<Off> offs;
      for (const auto& m : own)
      {
        if (m.level1 == l1 && m.level2 == l2)
        {
          offs.push_back({&m, false});
        }
      }
      for (const auto& m : extra)
      {
        if (m.level1 == l1 && m.level2 == l2)
        {
          offs.push_back({&m, true});
        }
      }
      std::sort(
        offs.begin(), offs.end(),
        [](const Off& a, const Off& b)
        {
          return std::make_tuple(
                   a.m->offset_end_s, a.m->offset_start_s, a.repaired,
                   a.m->id) <
                 std::make_tuple(
                   b.m->offset_end_s, b.m->offset_start_s, b.repaired, b.m->id);
        });
      json a = json::array();
      for (const auto& o : offs)
      {
        a.push_back(
          {{"message", o.m->id},
           {"offset_start_s", o.m->offset_start_s},
           {"offset_end_s", o.m->offset_end_s},
           {"valid_time", iso8601(o.m->valid_time_s)},
           {"from_run_total", o.repaired}});
      }
      body["offsets"] = a;
      return respond(200, body, gen);
    }

    if (seg.size() == 2 && what == "grids")
    {
      const CatGrid* g = t.grid(parse_id(seg[1], "grid id"));
      if (!g)
      {
        throw ApiError(404, "not_found", "unknown grid");
      }
      json gj = grid_json(*g);
      gj["id"] = g->id;
      gj["summary"] = json::parse(g->summary.empty() ? "{}" : g->summary);
      body["grid"] = gj;
      return respond(200, body, gen);
    }

    if (seg.size() == 1 && what == "tree")
    {
      static const std::vector<std::string> depths = {
        "centers", "models", "grids", "runs"};
      const std::string depth = q.count("depth") ? q.at("depth") : "runs";
      const auto d = std::find(depths.begin(), depths.end(), depth);
      if (d == depths.end())
      {
        throw ApiError(400, "bad_request", "depth: centers|models|grids|runs");
      }
      const int level = static_cast<int>(d - depths.begin());
      const bool with_params =
        q.count("parameters") && q.at("parameters") == "1";
      std::shared_ptr<const CatalogFieldStore> names;
      if (with_params)
      {
        names = std::make_shared<const CatalogFieldStore>(tp, catalog_, *this);
      }
      auto run_json = [&](const CatRun& r)
      {
        const auto offs = catalog_.run_offsets(t, r.id);
        const auto sigs = catalog_.run_signatures(t, r.id);
        json j{
          {"id", r.id},
          {"basetime", iso8601(r.basetime_s)},
          {"member", r.member},
          {"member_type", r.member_type ? json(*r.member_type) : json(nullptr)},
          {"parameters", sigs.size()},
          {"first_offset_s", offs.empty() ? json(nullptr) : json(offs.front())},
          {"last_offset_s", offs.empty() ? json(nullptr) : json(offs.back())}};
        if (with_params)
        {
          std::vector<const CatSignature*> ss;
          for (const auto id : sigs)
          {
            if (const CatSignature* sg = t.signature(id))
            {
              ss.push_back(sg);
            }
          }
          std::sort(
            ss.begin(), ss.end(),
            [](auto* a, auto* b)
            {
              return std::tie(a->name, a->hash) < std::tie(b->name, b->hash);
            });
          json pl = json::array();
          for (const auto* sg : ss)
          {
            pl.push_back(
              {{"signature", key_hex(sg->hash)},
               {"name", sg->name},
               {"field", names->field_name(sg->id)},
               {"common_names", names->common_names(sg->id)}});
          }
          j["parameter_list"] = pl;
        }
        return j;
      };
      std::vector<const CatCenter*> cs;
      for (const auto& c : t.centers)
      {
        cs.push_back(&c);
      }
      std::sort(
        cs.begin(), cs.end(),
        [](auto* x, auto* y) { return x->code < y->code; });
      json centers = json::array();
      for (const auto* c : cs)
      {
        json subs = json::array();
        for (const auto& sc : c->subcenters)
        {
          json sj{{"id", sc.id}, {"code", sc.code}, {"name", sc.name}};
          if (level >= 1)
          {
            std::vector<const CatModel*> ms;
            for (const auto& m : t.models)
            {
              if (m.subcenter_id == sc.id)
              {
                ms.push_back(&m);
              }
            }
            std::sort(
              ms.begin(), ms.end(),
              [](auto* a, auto* b) { return a->process_id < b->process_id; });
            json mj = json::array();
            for (const auto* m : ms)
            {
              json one{
                {"id", m->id},
                {"process_id", m->process_id},
                {"slug", m->slug},
                {"name", m->name}};
              if (level >= 2)
              {
                std::vector<const CatModelGrid*> gs;
                for (const auto& mg : t.model_grids)
                {
                  if (mg.model_id == m->id)
                  {
                    gs.push_back(&mg);
                  }
                }
                std::sort(
                  gs.begin(), gs.end(),
                  [](auto* a, auto* b)
                  {
                    return std::tie(a->name, a->id) < std::tie(b->name, b->id);
                  });
                json gj = json::array();
                for (const auto* mg : gs)
                {
                  const CatGrid* g = t.grid(mg->grid_id);
                  json oneg{
                    {"id", mg->id},
                    {"name", mg->name},
                    {"api_name", mg->api_name},
                    {"viewable", g && g->viewable},
                    {"grid_id", mg->grid_id}};
                  if (level >= 3)
                  {
                    std::vector<const CatRun*> rs;
                    for (const auto& r : t.runs)
                    {
                      if (r.model_grid_id == mg->id)
                      {
                        rs.push_back(&r);
                      }
                    }
                    std::sort(
                      rs.begin(), rs.end(),
                      [](auto* a, auto* b)
                      {
                        return std::make_tuple(-a->basetime_s, a->member) <
                               std::make_tuple(-b->basetime_s, b->member);
                      });
                    json rj = json::array();
                    for (const auto* r : rs)
                    {
                      rj.push_back(run_json(*r));
                    }
                    oneg["runs"] = rj;
                  }
                  gj.push_back(oneg);
                }
                one["grids"] = gj;
              }
              mj.push_back(one);
            }
            sj["models"] = mj;
          }
          subs.push_back(sj);
        }
        centers.push_back(
          {{"id", c->id},
           {"code", c->code},
           {"name", c->name},
           {"subcenters", subs}});
      }
      body["centers"] = centers;
      return respond(200, body, gen);
    }

    if (what != "messages" || seg.size() < 2)
    {
      throw ApiError(404, "not_found", "no such endpoint: " + path);
    }
    const std::uint64_t id = parse_id(seg[1], "message id");
    const auto msg = catalog_.message(t, id);
    if (!msg)
    {
      if (const auto nid = catalog_.replaced_by(t, id))
      {
        // A newer copy with other bytes has its own id (Q16b).
        json err{
          {"code", "replaced"},
          {"message", "message " + std::to_string(id) + " was replaced by " +
                        std::to_string(*nid)},
          {"replaced_by", *nid}};
        return respond(410, json{{"error", err}}, gen);
      }
      throw ApiError(404, "not_found", "unknown message");
    }
    const CatRun* run = t.run(msg->run_id);
    const CatSignature* sig = t.signature(msg->signature_id);
    const CatModelGrid* mg = run ? t.model_grid(run->model_grid_id) : nullptr;
    const CatModel* model = mg ? t.model(mg->model_id) : nullptr;
    const CatGrid* grid = mg ? t.grid(mg->grid_id) : nullptr;
    if (!run || !sig || !mg || !model || !grid)
    {
      throw ApiError(500, "internal", "catalog tables are inconsistent");
    }
    const bool sampleable =
      msg->grid_id.has_value() && grid->viewable &&
      (msg->flags & (kCatalogUndecodable | kCatalogGridUnsupported)) == 0;

    if (seg.size() == 3 && (seg[2] == "raw" || seg[2] == "values"))
    {
      const std::string etag = "\"" + key_hex(msg->content_hash) + "\"";
      if (q.count("v") && q.at("v") != key_hex(msg->content_hash))
      {
        throw ApiError(404, "not_found", "message has changed (v)");
      }
      fcgi::Response r;
      r.headers.push_back({"X-Atmo-Generation", std::to_string(gen)});
      r.headers.push_back(
        {"Cache-Control",
         q.count("v") ? "public, max-age=31536000, immutable" : "no-cache"});
      const bool raw = seg[2] == "raw";
      if (!raw && (msg->flags & kCatalogUndecodable))
      {
        throw ApiError(
          409, "not_sampleable", "eccodes cannot decode this message");
      }
      if (!raw && !values_)
      {
        throw ApiError(503, "no_decoder", "built without GRIB decoding");
      }
      // Different representations of one message: different ETags.
      r.headers.push_back({"ETag", raw ? etag : "\"v" + etag.substr(1)});
      if (
        req.param("HTTP_IF_NONE_MATCH") ==
        (raw ? etag : "\"v" + etag.substr(1)))
      {
        r.status = 304;
        return r;
      }
      const std::string bytes = read_message(
        root_ / msg->file_path, msg->byte_offset, msg->byte_length,
        msg->content_hash);
      if (raw)
      {
        r.content_type = "application/x-grib2";
        r.headers.push_back(
          {"Content-Disposition",
           "attachment; filename=\"" + std::to_string(msg->id) + ".grib2\""});
        r.body = bytes;
      }
      else
      {
        std::vector<double> v;
        try
        {
          v = values_(bytes.data(), bytes.size());
        }
        catch (const std::runtime_error& ex)
        {
          throw ApiError(409, "not_sampleable", ex.what());
        }
        r.content_type = "application/octet-stream";
        r.body.resize(v.size() * 4);
        for (std::size_t k = 0; k < v.size(); ++k)
        {
          const float f =
            static_cast<float>(v[k] * sig->si_scale + sig->si_offset);
          std::uint32_t u = 0;
          std::memcpy(&u, &f, 4);
          for (int b = 0; b < 4; ++b)  // little-endian on any host
          {
            r.body[k * 4 + b] = static_cast<char>((u >> (8 * b)) & 0xff);
          }
        }
        r.headers.push_back({"X-Atmo-Count", std::to_string(v.size())});
        r.headers.push_back({"X-Atmo-Units", sig->units});
        r.headers.push_back({"X-Atmo-Grid", std::to_string(grid->id)});
      }
      if (req.param("REQUEST_METHOD", "GET") == "HEAD")
      {
        r.body.clear();
      }
      return r;
    }

    if (seg.size() == 2)
    {
      const CatCenter* c = t.center(model->center_id);
      std::string sub_name;
      if (c)
      {
        for (const auto& s : c->subcenters)
        {
          if (s.id == model->subcenter_id)
          {
            sub_name = s.name;
          }
        }
      }
      json g = grid_json(*grid);
      g["name"] = mg->name;
      g["api_name"] = mg->api_name;
      json p = parameter_json(*sig);
      p["canonical"] = sig->canonical;
      body["message"] = {
        {"id", msg->id},
        {"run",
         {{"id", run->id},
          {"basetime", iso8601(run->basetime_s)},
          {"member", run->member},
          {"member_type",
           run->member_type ? json(*run->member_type) : json(nullptr)}}},
        {"model",
         {{"id", model->id},
          {"slug", model->slug},
          {"name", model->name},
          {"process_id", model->process_id}}},
        {"center", {{"code", model->center_code}, {"name", c ? c->name : ""}}},
        {"subcenter", {{"code", model->subcenter_code}, {"name", sub_name}}},
        {"grid", g},
        {"process_id", msg->process_id},
        {"parameter", p},
        {"level1", level_json(msg->level1)},
        {"level2", level_json(msg->level2)},
        {"offset_start_s", msg->offset_start_s},
        {"offset_end_s", msg->offset_end_s},
        {"valid_time", iso8601(msg->valid_time_s)},
        {"file",
         {{"path", msg->file_path},
          {"byte_offset", msg->byte_offset},
          {"byte_length", msg->byte_length}}},
        {"content_hash", key_hex(msg->content_hash)},
        {"flags", flags_json(msg->flags)},
        {"sampleable", sampleable}};
      return respond(200, body, gen);
    }

    // point / tiles: decode through the cache.
    const bool is_point = seg.size() == 3 && seg[2] == "point";
    const bool is_tile = seg.size() == 6 && seg[2] == "tiles";
    if (!is_point && !is_tile)
    {
      throw ApiError(404, "not_found", "no such endpoint: " + path);
    }
    if (is_tile && q.count("v") && q.at("v") != key_hex(msg->content_hash))
    {
      throw ApiError(404, "not_found", "message has changed (v)");
    }
    if (!sampleable)
    {
      throw ApiError(
        409, "not_sampleable",
        "this message cannot be sampled (grid or template)");
    }
    if (!cache_)
    {
      throw ApiError(503, "no_decoder", "built without GRIB decoding");
    }
    const auto g = load_grid(grid->key);
    GribIndexEntry e;
    e.offset = msg->byte_offset;
    e.length = msg->byte_length;
    e.hash = msg->content_hash;
    e.info.param_id = kNativeParam;
    e.grid = grid->key;
    const auto data = cache_->get(root_ / msg->file_path, e, g);
    FieldInfo info;
    info.param_id = kNativeParam;
    info.level_type = static_cast<std::uint8_t>(
      msg->level1.type > 255 ? 255 : msg->level1.type);
    info.level_value = static_cast<float>(msg->level1.value);
    auto view =
      std::make_shared<ScaledView>(data, info, sig->si_scale, sig->si_offset);

    if (is_point)
    {
      double lat = 0.0, lon = 0.0;
      for (auto [k, out] : {std::pair{"lat", &lat}, std::pair{"lon", &lon}})
      {
        auto it = q.find(k);
        const std::string s = it == q.end() ? "" : it->second;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), *out);
        if (
          s.empty() || r.ec != std::errc() || r.ptr != s.data() + s.size() ||
          !std::isfinite(*out))
        {
          throw ApiError(
            400, "bad_request",
            std::string(it == q.end() ? "missing " : "bad ") + k);
        }
      }
      if (lat < -90.0 || lat > 90.0)
      {
        throw ApiError(400, "bad_request", "bad lat");
      }
      const auto cell = g->locate(lat, lon);
      body["message"] = msg->id;
      body["lat"] = lat;
      body["lon"] = lon;
      body["units"] = sig->units;
      body["status"] = cell ? "ok" : "outside_grid";
      body["value"] = cell ? tidy(view->sample(*cell)) : json(nullptr);
      body["uv_relative_to_grid"] = data->uv_relative_to_grid;
      return respond(200, body, gen);
    }

    // Tile.
    auto number = [](const std::string& s, const char* w)
    {
      std::uint32_t n = 0;
      const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
      if (s.empty() || r.ec != std::errc() || r.ptr != s.data() + s.size())
      {
        throw ApiError(400, "bad_request", std::string("bad ") + w);
      }
      return n;
    };
    std::string ys = seg[5];
    if (ys.size() < 5 || ys.compare(ys.size() - 4, 4, ".png") != 0)
    {
      throw ApiError(404, "not_found", "expected .../tiles/{z}/{x}/{y}.png");
    }
    ys.resize(ys.size() - 4);
    TileRequest tr;
    tr.model = "m";
    tr.field.text = "message";
    tr.field.param = 1;  // OneMessageStore's only parameter
    tr.field.level = FieldSpec::Level::fixed;
    tr.field.level_type = info.level_type;
    tr.field.level_value = info.level_value;
    tr.z = number(seg[3], "z");
    tr.x = number(seg[4], "x");
    tr.y = number(ys, "y");
    if (tr.z > 24 || tr.x >= (1u << tr.z) || tr.y >= (1u << tr.z))
    {
      throw ApiError(400, "bad_request", "bad z/x/y");
    }
    if (!q.count("cmap"))
    {
      throw ApiError(400, "bad_request", "missing cmap");
    }
    Colormap cmap;
    try
    {
      cmap = Colormap::parse(
        q.at("cmap"), q.count("nodata")
                        ? std::optional<std::string_view>(q.at("nodata"))
                        : std::nullopt);
    }
    catch (const std::invalid_argument& ex)
    {
      throw ApiError(400, "bad_request", ex.what());
    }
    const OneMessageStore store(view);
    const TileResult res = render_tile(store, tr, cmap);
    fcgi::Response r;
    r.content_type = "image/png";
    const std::string etag =
      "\"" + key_hex(fnv1a64(res.png.data(), res.png.size())) + "\"";
    r.headers.push_back({"X-Atmo-Generation", std::to_string(gen)});
    r.headers.push_back(
      {"Cache-Control",
       q.count("v") ? "public, max-age=31536000, immutable" : "no-cache"});
    r.headers.push_back({"ETag", etag});
    if (req.param("HTTP_IF_NONE_MATCH") == etag)
    {
      r.status = 304;
      return r;
    }
    if (req.param("REQUEST_METHOD", "GET") != "HEAD")
    {
      r.body = res.png;
    }
    return r;
  }
  catch (const ApiError& e)
  {
    return respond(
      e.status, json{{"error", {{"code", e.code}, {"message", e.what()}}}},
      gen);
  }
  catch (const std::exception& e)
  {
    return respond(
      500, json{{"error", {{"code", "internal"}, {"message", e.what()}}}}, gen);
  }
}

// ---- CatalogFieldStore -----------------------------------------------------

namespace
{

std::string lower(std::string_view s)
{
  std::string out(s);
  for (char& c : out)
  {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string normalise(std::string_view name)
{
  std::string out = lower(name);
  std::replace(out.begin(), out.end(), ' ', '_');
  return out;
}

ParamRole role_of(const CatSignature& s)
{
  if (s.stat_interval_s || s.discipline != 0)
  {
    return ParamRole::other;
  }
  if (s.category == 2 && s.number == 2)
  {
    return ParamRole::wind_u;
  }
  if (s.category == 2 && s.number == 3)
  {
    return ParamRole::wind_v;
  }
  if (s.category == 3 && s.number == 5)
  {
    return ParamRole::geopotential_height;
  }
  return ParamRole::other;
}

constexpr std::size_t kMaxCandidates = 20;

}  // namespace

CatalogFieldStore::CatalogFieldStore(
  std::shared_ptr<const CatalogTables> tables,
  CatalogQuery& catalog,
  const GribApi& api)
    : tables_(std::move(tables)), catalog_(catalog), api_(api)
{
  const CatalogTables& t = *tables_;
  std::map<std::uint64_t, std::uint16_t> by_parameter;
  for (const auto& s : t.signatures)
  {
    std::uint16_t id = 0;
    if (s.parameter_id)
    {
      auto it = by_parameter.find(*s.parameter_id);
      if (it != by_parameter.end())
      {
        id = it->second;
      }
    }
    if (id == 0)
    {
      if (params_.size() >= 0xffff)
      {
        throw std::runtime_error("catalog: more than 65535 parameters");
      }
      Param p;
      p.parameter_id = s.parameter_id;
      p.info.name = s.name;
      p.info.units = s.units;
      p.normalised = normalise(s.name);
      params_.push_back(std::move(p));
      id = static_cast<std::uint16_t>(params_.size());
      if (s.parameter_id)
      {
        by_parameter[*s.parameter_id] = id;
      }
    }
    sig_param_[s.id] = id;
    if (s.stat_interval_s && *s.stat_interval_s > 0)
    {
      fixed_by_base_[s.base_hash].push_back(&s);
    }
    Param& p = params_[id - 1];
    const ParamRole r = role_of(s);
    if (r != ParamRole::other)
    {
      p.info.role = r;
    }
  }
  for (const auto& c : t.common_names)
  {
    auto it = by_parameter.find(c.parameter_id);
    if (it != by_parameter.end())
    {
      params_[it->second - 1].common.push_back(lower(c.name));
    }
  }
  for (auto& p : params_)
  {
    std::sort(p.common.begin(), p.common.end());
    p.info.name = p.common.empty() ? p.normalised : p.common.front();
  }
  for (const auto& mg : t.model_grids)
  {
    const CatGrid* g = t.grid(mg.grid_id);
    if (!g || !g->viewable || mg.api_name.empty())
    {
      continue;
    }
    for (const auto& r : t.runs)
    {
      if (r.model_grid_id == mg.id && r.member == -1)
      {
        models_[lower(mg.api_name)] = &mg;
        break;
      }
    }
  }
}

CatalogFieldStore::~CatalogFieldStore() = default;

std::vector<std::string> CatalogFieldStore::models() const
{
  std::vector<std::string> out;
  for (const auto& [name, mg] : models_)
  {
    out.push_back(name);
  }
  return out;
}

const CatRun* CatalogFieldStore::run_of(
  std::string_view model,
  std::int64_t run_s) const
{
  auto it = models_.find(std::string(model));
  if (it == models_.end())
  {
    return nullptr;
  }
  for (const auto& r : tables_->runs)
  {
    if (
      r.model_grid_id == it->second->id && r.member == -1 &&
      r.basetime_s == run_s)
    {
      return &r;
    }
  }
  return nullptr;
}

std::vector<ModelTime> CatalogFieldStore::times(std::string_view model) const
{
  std::vector<ModelTime> out;
  auto it = models_.find(std::string(model));
  if (it == models_.end())
  {
    return out;
  }
  for (const auto& r : tables_->runs)
  {
    if (r.model_grid_id != it->second->id || r.member != -1)
    {
      continue;
    }
    for (const std::int64_t off : catalog_.run_offsets(*tables_, r.id))
    {
      if (off >= 0 && off % 3600 == 0)
      {
        out.push_back({r.basetime_s, static_cast<std::uint32_t>(off / 3600)});
      }
    }
  }
  std::sort(
    out.begin(), out.end(), [](const ModelTime& a, const ModelTime& b)
    { return std::tie(a.run_s, a.fhr) < std::tie(b.run_s, b.fhr); });
  return out;
}

std::shared_ptr<const Grid> CatalogFieldStore::grid(
  std::string_view model,
  const ModelTime& t) const
{
  if (!run_of(model, t.run_s))
  {
    return nullptr;
  }
  const CatGrid* g = tables_->grid(models_.at(std::string(model))->grid_id);
  return g ? api_.load_grid(g->key) : nullptr;
}

std::vector<CatalogFieldStore::Slot> CatalogFieldStore::slots(
  std::string_view model,
  const ModelTime& t,
  std::vector<CatMessage>& keep) const
{
  std::vector<Slot> out;
  const CatRun* run = run_of(model, t.run_s);
  if (!run)
  {
    return out;
  }
  keep = catalog_.messages_at(
    *tables_, run->id, static_cast<std::int64_t>(t.fhr) * 3600);
  for (const auto& m : keep)
  {
    const CatSignature* sig = tables_->signature(m.signature_id);
    if (
      !sig || !m.grid_id ||
      (m.flags & (kCatalogUndecodable | kCatalogGridUnsupported)) != 0)
    {
      continue;
    }
    Slot s;
    s.info.param_id = sig_param_.at(sig->id);
    s.info.level_type =
      static_cast<std::uint8_t>(m.level1.type > 255 ? 255 : m.level1.type);
    s.info.level_value = static_cast<float>(m.level1.value);
    s.info.min = s.info.max = std::numeric_limits<float>::quiet_NaN();
    s.msg = &m;
    s.sig = sig;
    bool dup = false;
    for (auto& o : out)
    {
      if (o.info.same_field(s.info))
      {
        o.info.flags |= kFieldAmbiguous;
        dup = true;
      }
    }
    if (!dup)
    {
      out.push_back(s);
    }
  }
  // First-bucket repair (Q22c): at offset N the run-total message 0..N is
  // also the N-second bucket of its fixed-length siblings, unless they
  // have a message of their own there.
  const std::int64_t off = static_cast<std::int64_t>(t.fhr) * 3600;
  const std::size_t real = out.size();
  for (std::size_t k = 0; k < real; ++k)
  {
    const Slot total = out[k];
    if (
      !total.sig->stat_interval_s || *total.sig->stat_interval_s != 0 ||
      total.msg->offset_start_s != 0 || off <= 0)
    {
      continue;
    }
    auto it = fixed_by_base_.find(total.sig->base_hash);
    if (it == fixed_by_base_.end())
    {
      continue;
    }
    for (const CatSignature* fixed : it->second)
    {
      if (static_cast<std::int64_t>(*fixed->stat_interval_s) != off)
      {
        continue;
      }
      Slot s = total;
      s.info.param_id = sig_param_.at(fixed->id);
      s.info.flags = 0;
      s.sig = fixed;  // its units / scale
      bool have = false;
      for (const auto& o : out)
      {
        have = have || o.info.same_field(s.info);
      }
      if (!have)
      {
        out.push_back(s);
      }
    }
  }
  return out;
}

std::vector<FieldInfo> CatalogFieldStore::fields(
  std::string_view model,
  const ModelTime& t) const
{
  std::vector<CatMessage> keep;
  std::vector<FieldInfo> out;
  const CatGrid* g = nullptr;
  for (auto& s : slots(model, t, keep))
  {
    const ParamRole r = params_[s.info.param_id - 1].info.role;
    if (
      (r == ParamRole::wind_u || r == ParamRole::wind_v) &&
      !(s.info.flags & kFieldAmbiguous))
    {
      // Grid-relative or not is a property of the grid (GRIB2 section 3
      // resolution and component flags are part of its identity): one
      // decode per grid, ever, not one per wind message.
      g = g ? g : tables_->grid(*s.msg->grid_id);
      if (g)
      {
        if (const auto rel = api_.uv_relative_to_grid(*s.msg, *g, *s.sig))
        {
          if (*rel)
          {
            s.info.flags |= kFieldGridRelative;
          }
        }
      }
    }
    out.push_back(s.info);
  }
  return out;
}

std::shared_ptr<const FieldView> CatalogFieldStore::view(
  std::string_view model,
  const ModelTime& t,
  const FieldKey& key) const
{
  std::vector<CatMessage> keep;
  for (const auto& s : slots(model, t, keep))
  {
    if (
      s.info.param_id == key.param_id && s.info.level_type == key.level_type &&
      s.info.level_value == key.level_value)
    {
      if (s.info.flags & kFieldAmbiguous)
      {
        // Listed so callers can say why; no values.
        struct Empty : FieldView
        {
          Empty(std::shared_ptr<const Grid> g, FieldInfo i)
              : FieldView(std::move(g), i)
          {
          }
          double at(std::uint32_t, std::uint32_t) const override
          {
            return std::numeric_limits<double>::quiet_NaN();
          }
        };
        auto g = grid(model, t);
        return g ? std::make_shared<Empty>(g, s.info) : nullptr;
      }
      const CatGrid* g = tables_->grid(*s.msg->grid_id);
      return g ? api_.decode(*s.msg, *g, *s.sig, s.info) : nullptr;
    }
  }
  return nullptr;
}

float CatalogFieldStore::sample(
  std::string_view model,
  const ModelTime& t,
  const FieldKey& key,
  const Grid::Cell& cell) const
{
  const auto v = view(model, t, key);
  return v ? static_cast<float>(v->sample(cell))
           : std::numeric_limits<float>::quiet_NaN();
}

ParamInfo CatalogFieldStore::param(std::uint16_t id) const
{
  if (id == 0 || id > params_.size())
  {
    return {"param" + std::to_string(id), "", ParamRole::other};
  }
  return params_[id - 1].info;
}

std::string CatalogFieldStore::field_name(std::uint64_t signature_id) const
{
  auto it = sig_param_.find(signature_id);
  return it == sig_param_.end() ? "" : params_[it->second - 1].info.name;
}

std::vector<std::string> CatalogFieldStore::common_names(
  std::uint64_t signature_id) const
{
  auto it = sig_param_.find(signature_id);
  return it == sig_param_.end() ? std::vector<std::string>{}
                                : params_[it->second - 1].common;
}

ResolvedName CatalogFieldStore::resolve_field(std::string_view name) const
{
  const std::string want = lower(name);
  auto found = [&](std::uint16_t id, bool guess)
  { return ResolvedName{params_[id - 1].info.name, id, guess}; };
  // 1. exact common name, then exact parameter name.
  for (std::size_t k = 0; k < params_.size(); ++k)
  {
    const auto& c = params_[k].common;
    if (std::find(c.begin(), c.end(), want) != c.end())
    {
      return found(static_cast<std::uint16_t>(k + 1), false);
    }
  }
  std::vector<std::uint16_t> hit;
  for (std::size_t k = 0; k < params_.size(); ++k)
  {
    if (params_[k].normalised == want)
    {
      hit.push_back(static_cast<std::uint16_t>(k + 1));
    }
  }
  if (hit.size() == 1)
  {
    return found(hit[0], false);
  }
  if (hit.size() > 1)
  {
    throw NameError(
      400, "ambiguous_field",
      "field \"" + std::string(name) + "\" names " +
        std::to_string(hit.size()) +
        " parameters; give one of them a common name in the web app");
  }
  // 2. the only name containing the text: common names, then parameters.
  std::set<std::uint16_t> part;
  std::set<std::string> names;
  for (int pass = 0; pass < 2 && part.empty(); ++pass)
  {
    for (std::size_t k = 0; k < params_.size(); ++k)
    {
      const auto& p = params_[k];
      const std::vector<std::string> cands =
        pass == 0 ? p.common : std::vector<std::string>{p.normalised};
      for (const auto& c : cands)
      {
        if (c.find(want) != std::string::npos)
        {
          part.insert(static_cast<std::uint16_t>(k + 1));
          names.insert(p.info.name);
        }
      }
    }
  }
  if (part.size() == 1)
  {
    return found(*part.begin(), true);
  }
  if (part.empty())
  {
    throw NameError(
      404, "not_found", "no field named like \"" + std::string(name) + "\"");
  }
  std::vector<std::string> c(names.begin(), names.end());
  c.resize(std::min(c.size(), kMaxCandidates));
  throw NameError(
    400, "ambiguous_name",
    "field \"" + std::string(name) + "\" matches several names", c);
}

ResolvedName CatalogFieldStore::resolve_model(std::string_view name) const
{
  const std::string want = lower(name);
  if (models_.count(want))
  {
    return {want, 0, false};
  }
  std::set<std::string> part;
  for (const auto& [n, mg] : models_)
  {
    if (n.find(want) != std::string::npos)
    {
      part.insert(n);
    }
  }
  if (part.empty())
  {
    for (const auto& [n, mg] : models_)
    {
      const CatModel* m = tables_->model(mg->model_id);
      if (m && lower(m->slug).find(want) != std::string::npos)
      {
        part.insert(n);
      }
    }
  }
  if (part.size() == 1)
  {
    return {*part.begin(), 0, true};
  }
  if (part.empty())
  {
    throw NameError(
      404, "not_found", "no model named like \"" + std::string(name) + "\"");
  }
  std::vector<std::string> c(part.begin(), part.end());
  c.resize(std::min(c.size(), kMaxCandidates));
  throw NameError(
    400, "ambiguous_name",
    "model \"" + std::string(name) + "\" matches several names", c);
}

}  // namespace atmo
