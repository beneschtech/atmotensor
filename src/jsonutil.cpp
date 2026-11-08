#include "jsonutil.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace atmo::detail
{
namespace
{

using nlohmann::json;

[[noreturn]] void bad(std::string_view what, const std::string& problem)
{
  throw std::invalid_argument(std::string(what) + ": " + problem);
}

std::string quoted(const char* key)
{
  return std::string("\"") + key + "\"";
}

// j[key], with a missing key (or a non-object j) reported like any other
// error instead of as nlohmann's exception.
const json& field(const json& j, const char* key, std::string_view what)
{
  if (!j.is_object())
  {
    bad(what, "not a JSON object");
  }
  auto it = j.find(key);
  if (it == j.end())
  {
    bad(what, "missing " + quoted(key));
  }
  return *it;
}

}  // namespace

void check_keys(
  const json& j,
  std::string_view what,
  std::initializer_list<const char*> keys)
{
  if (!j.is_object())
  {
    bad(what, "not a JSON object");
  }
  for (const auto& [k, v] : j.items())
  {
    bool ok = false;
    for (const char* a : keys)
    {
      ok = ok || k == a;
    }
    if (!ok)
    {
      bad(what, "unknown key \"" + k + "\"");
    }
  }
  for (const char* a : keys)
  {
    if (!j.contains(a))
    {
      bad(what, "missing " + quoted(a));
    }
  }
}

std::uint64_t get_u64(const json& j, const char* key, std::string_view what)
{
  const json& v = field(j, key, what);
  if (!v.is_number_unsigned())
  {
    bad(what, quoted(key) + " must be an unsigned integer");
  }
  return v.get<std::uint64_t>();
}

std::uint32_t get_u32(const json& j, const char* key, std::string_view what)
{
  const std::uint64_t v = get_u64(j, key, what);
  if (v > std::numeric_limits<std::uint32_t>::max())
  {
    bad(what, quoted(key) + " out of range");
  }
  return static_cast<std::uint32_t>(v);
}

std::int64_t get_i64(const json& j, const char* key, std::string_view what)
{
  const json& v = field(j, key, what);
  if (v.is_number_unsigned())
  {
    if (
      v.get<std::uint64_t>() >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
      bad(what, quoted(key) + " out of range");
    }
    return static_cast<std::int64_t>(v.get<std::uint64_t>());
  }
  if (!v.is_number_integer())
  {
    bad(what, quoted(key) + " must be an integer");
  }
  return v.get<std::int64_t>();
}

double get_double(const json& j, const char* key, std::string_view what)
{
  const json& v = field(j, key, what);
  if (!v.is_number() || !std::isfinite(v.get<double>()))
  {
    bad(what, quoted(key) + " must be a finite number");
  }
  return v.get<double>();
}

std::string get_str(const json& j, const char* key, std::string_view what)
{
  const json& v = field(j, key, what);
  if (!v.is_string())
  {
    bad(what, quoted(key) + " must be a string");
  }
  return v.get<std::string>();
}

const json& get_array(const json& j, const char* key, std::string_view what)
{
  const json& v = field(j, key, what);
  if (!v.is_array())
  {
    bad(what, quoted(key) + " must be an array");
  }
  return v;
}

void check_format(
  const json& j,
  std::string_view what,
  const char* format,
  int version)
{
  if (!j.is_object())
  {
    bad(what, "not a JSON object");
  }
  auto f = j.find("format");
  if (f == j.end() || !f->is_string() || *f != format)
  {
    bad(what, std::string("\"format\" must be \"") + format + "\"");
  }
  auto v = j.find("version");
  if (v == j.end() || !v->is_number_integer() || *v != version)
  {
    bad(what, "unsupported \"version\" (want " + std::to_string(version) + ")");
  }
}

json parse_json(std::string_view text, std::string_view what)
{
  try
  {
    return json::parse(text.begin(), text.end());
  }
  catch (const json::exception& e)
  {
    bad(what, std::string("invalid JSON: ") + e.what());
  }
}

std::string read_text_file(
  const std::filesystem::path& p,
  std::string_view what)
{
  std::ifstream in(p, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error(std::string(what) + ": cannot read " + p.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad())
  {
    throw std::runtime_error(
      std::string(what) + ": error reading " + p.string());
  }
  return ss.str();
}

}  // namespace atmo::detail
