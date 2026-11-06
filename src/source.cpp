#include "atmo/source.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace atmo
{
namespace
{

nlohmann::json parse_json(std::string_view text)
{
  try
  {
    return nlohmann::json::parse(text.begin(), text.end());
  }
  catch (const nlohmann::json::exception& e)
  {
    throw std::invalid_argument(
      std::string("config: invalid JSON: ") + e.what());
  }
}

}  // namespace

double SourceClass::lambda() const
{
  return L_km / tau_seconds;
}

std::int64_t SourceClass::bucket_start_s(std::int64_t t_s) const
{
  std::int64_t q = t_s / bucket_seconds;
  if (t_s % bucket_seconds < 0)
  {
    --q;
  }
  return q * bucket_seconds;
}

Embedding SourceClass::make_embedding(std::int64_t start_s) const
{
  if (bucket_start_s(start_s) != start_s)
  {
    throw std::invalid_argument(
      "SourceClass " + name + ": " + std::to_string(start_s) +
      " is not a bucket start");
  }
  Embedding e;
  e.t0_s = static_cast<double>(start_s);
  e.km_per_second = lambda();
  e.vertical_scale = vertical_scale;
  return e;
}

bool SourceClass::valid_name(std::string_view n)
{
  if (n.empty() || n.size() > kMaxName)
  {
    return false;
  }
  if (!(n[0] >= 'a' && n[0] <= 'z'))
  {
    return false;
  }
  for (char c : n)
  {
    const bool ok =
      (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok)
    {
      return false;
    }
  }
  return true;
}

void SourceClass::validate() const
{
  auto bad = [&](const std::string& why)
  { return std::invalid_argument("source class '" + name + "': " + why); };
  if (!valid_name(name))
  {
    throw bad("name must match [a-z][a-z0-9_-]* and be at most 31 characters");
  }
  if (bucket_seconds <= 0)
  {
    throw bad("bucket_seconds must be > 0");
  }
  if (!(std::isfinite(L_km) && L_km > 0))
  {
    throw bad("L_km must be finite and > 0");
  }
  if (!(std::isfinite(tau_seconds) && tau_seconds > 0))
  {
    throw bad("tau_seconds must be finite and > 0");
  }
  if (!(std::isfinite(vertical_scale) && vertical_scale >= 0))
  {
    throw bad("vertical_scale must be finite and >= 0");
  }
  if (retain_seconds <= 0)
  {
    throw bad("retain_seconds must be > 0");
  }
}

nlohmann::json SourceClass::to_json() const
{
  return nlohmann::json{
    {"name", name},
    {"bucket_seconds", bucket_seconds},
    {"L_km", L_km},
    {"tau_seconds", tau_seconds},
    {"vertical_scale", vertical_scale},
    {"retain_seconds", retain_seconds}};
}

SourceClass SourceClass::from_json(const nlohmann::json& j)
{
  if (!j.is_object())
  {
    throw std::invalid_argument("source class must be a JSON object");
  }
  auto name_it = j.find("name");
  if (name_it == j.end() || !name_it->is_string())
  {
    throw std::invalid_argument("source class needs a string \"name\"");
  }
  SourceClass s;
  s.name = name_it->get<std::string>();
  auto bad = [&](const std::string& why)
  { return std::invalid_argument("source class '" + s.name + "': " + why); };
  static const char* const kKeys[] = {"name",           "bucket_seconds",
                                      "L_km",           "tau_seconds",
                                      "vertical_scale", "retain_seconds"};
  for (auto it = j.begin(); it != j.end(); ++it)
  {
    bool known = false;
    for (const char* k : kKeys)
    {
      known = known || it.key() == k;
    }
    if (!known)
    {
      throw bad("unknown key \"" + it.key() + "\"");
    }
  }
  auto get_int = [&](const char* key, std::int64_t& out)
  {
    auto it = j.find(key);
    if (it == j.end())
    {
      throw bad(std::string("missing \"") + key + "\"");
    }
    if (!it->is_number_integer())
    {
      throw bad(std::string("\"") + key + "\" must be an integer");
    }
    if (
      it->is_number_unsigned() &&
      it->get<std::uint64_t>() >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
      throw bad(std::string("\"") + key + "\" out of range");
    }
    out = it->get<std::int64_t>();
  };
  auto get_num = [&](const char* key, double& out)
  {
    auto it = j.find(key);
    if (it == j.end())
    {
      throw bad(std::string("missing \"") + key + "\"");
    }
    if (!it->is_number())
    {
      throw bad(std::string("\"") + key + "\" must be a number");
    }
    out = it->get<double>();
  };
  get_int("bucket_seconds", s.bucket_seconds);
  get_num("L_km", s.L_km);
  get_num("tau_seconds", s.tau_seconds);
  get_num("vertical_scale", s.vertical_scale);
  get_int("retain_seconds", s.retain_seconds);
  s.validate();
  return s;
}

SourceClass SourceClass::from_text(std::string_view json_text)
{
  return from_json(parse_json(json_text));
}

SourceRegistry::SourceRegistry(std::vector<SourceClass> classes)
    : classes_(std::move(classes))
{
  validate();
}

SourceRegistry SourceRegistry::defaults()
{
  auto mk = [](const char* n, std::int64_t bucket, double L, double tau)
  {
    SourceClass s;
    s.name = n;
    s.bucket_seconds = bucket;
    s.L_km = L;
    s.tau_seconds = tau;
    s.vertical_scale = 10.0;
    s.retain_seconds = 259200;
    return s;
  };
  return SourceRegistry(
    {mk("metar", 3600, 100, 3600), mk("synop", 3600, 150, 3 * 3600),
     mk("sounding", 12 * 3600, 250, 6 * 3600), mk("pirep", 3600, 100, 3600),
     mk("aircraft", 3600, 100, 1800)});
}

SourceRegistry SourceRegistry::from_json(const nlohmann::json& j)
{
  if (!j.is_object())
  {
    throw std::invalid_argument("sources export: not a JSON object");
  }
  auto fmt = j.find("format");
  if (fmt == j.end() || !fmt->is_string() || *fmt != kFormat)
  {
    throw std::invalid_argument(
      std::string("sources export: \"format\" must be \"") + kFormat + "\"");
  }
  auto ver = j.find("version");
  if (ver == j.end() || !ver->is_number_integer() || *ver != kVersion)
  {
    throw std::invalid_argument(
      "sources export: unsupported \"version\" (want " +
      std::to_string(kVersion) + ")");
  }
  auto arr = j.find("sources");
  if (arr == j.end() || !arr->is_array())
  {
    throw std::invalid_argument("sources export: \"sources\" must be an array");
  }
  if (arr->empty())
  {
    throw std::invalid_argument("sources export: no source classes");
  }
  std::vector<SourceClass> v;
  for (const auto& e : *arr)
  {
    v.push_back(SourceClass::from_json(e));
  }
  return SourceRegistry(std::move(v));
}

SourceRegistry SourceRegistry::from_text(std::string_view json_text)
{
  return from_json(parse_json(json_text));
}

SourceRegistry SourceRegistry::from_file(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
  {
    throw std::invalid_argument("config: cannot read " + path.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  try
  {
    return from_text(ss.str());
  }
  catch (const std::invalid_argument& e)
  {
    throw std::invalid_argument(path.string() + ": " + e.what());
  }
}

nlohmann::json SourceRegistry::to_json() const
{
  nlohmann::json arr = nlohmann::json::array();
  for (const auto& s : classes_)
  {
    arr.push_back(s.to_json());
  }
  return nlohmann::json{
    {"format", kFormat}, {"version", kVersion}, {"sources", std::move(arr)}};
}

const SourceClass* SourceRegistry::find(std::string_view name) const
{
  for (const auto& s : classes_)
  {
    if (s.name == name)
    {
      return &s;
    }
  }
  return nullptr;
}

const SourceClass& SourceRegistry::at(std::string_view name) const
{
  if (const SourceClass* s = find(name))
  {
    return *s;
  }
  throw std::out_of_range("unknown source class '" + std::string(name) + "'");
}

void SourceRegistry::validate() const
{
  for (std::size_t i = 0; i < classes_.size(); ++i)
  {
    classes_[i].validate();
    for (std::size_t k = 0; k < i; ++k)
    {
      if (classes_[k].name == classes_[i].name)
      {
        throw std::invalid_argument(
          "config: duplicate source class '" + classes_[i].name + "'");
      }
    }
  }
}

}  // namespace atmo
