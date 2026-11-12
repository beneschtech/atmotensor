// Grid catalog: data types and connection config (no database code; the
// MySQL part is src/catalog_mysql.cpp in the atmo_catalog library).
#include "atmo/catalog.h"

#include <sys/stat.h>

#include <cctype>
#include <iterator>
#include <set>
#include <stdexcept>

#include "jsonutil.h"

namespace atmo
{

void CatalogFile::validate() const
{
  if (
    path.empty() || path.front() == '/' || path.find("..") != std::string::npos)
  {
    throw std::invalid_argument(
      "catalog: file path must be relative: \"" + path + "\"");
  }
  std::set<GridKey> keys;
  for (const auto& g : grids)
  {
    if (!keys.insert(g.key).second)
    {
      throw std::invalid_argument(
        "catalog: grid " + key_hex(g.key) + " listed twice");
    }
  }
  for (const auto& m : messages)
  {
    if (!keys.count(m.grid))
    {
      throw std::invalid_argument(
        "catalog: message grid " + key_hex(m.grid) + " is not listed");
    }
    if (m.header.sig.canonical.empty())
    {
      throw std::invalid_argument("catalog: message without a signature");
    }
  }
  if (messages.size() + skipped.size() > messages_total)
  {
    throw std::invalid_argument("catalog: more messages than messages_total");
  }
}

std::string catalog_parameter_units(const GribSignature& sig)
{
  if (!sig.table_name.empty())
  {
    return sig.table_units;
  }
  std::string u = sig.ecc_units;
  for (std::size_t p = u.find("**"); p != std::string::npos; p = u.find("**"))
  {
    u.erase(p, 2);
  }
  return u;
}

namespace
{

// Code table 4.10 in words; empty for accumulation (the name says it).
std::string statistic_word(std::uint32_t code)
{
  static const char* const words[] = {
    "average",
    "",
    "maximum",
    "minimum",
    "difference",
    "RMS",
    "standard deviation",
    "covariance",
    "difference",
    "ratio",
    "standardized anomaly",
    "summation"};
  if (code < std::size(words))
  {
    return words[code];
  }
  return "statistic " + std::to_string(code);
}

bool contains_nocase(std::string hay, std::string needle)
{
  for (auto* s : {&hay, &needle})
  {
    for (char& c : *s)
    {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return hay.find(needle) != std::string::npos;
}

std::string interval_text(std::uint32_t s)
{
  if (s % 86400 == 0 && s >= 2 * 86400)
  {
    return std::to_string(s / 86400) + "day";
  }
  if (s % 3600 == 0)
  {
    return std::to_string(s / 3600) + "hr";
  }
  if (s % 60 == 0)
  {
    return std::to_string(s / 60) + "min";
  }
  return std::to_string(s) + "s";
}

}  // namespace

std::string catalog_parameter_name(const GribSignature& sig)
{
  std::string name = sig.table_name.empty() ? sig.ecc_name : sig.table_name;
  if (name.empty() || sig.ecc_short_name == "unknown")
  {
    name = "Unknown " + std::to_string(sig.discipline) + "/" +
           std::to_string(sig.category) + "/" + std::to_string(sig.number);
  }
  if (!sig.stat_interval_s)
  {
    return name;
  }
  const std::uint32_t s = *sig.stat_interval_s;
  // Accumulations: "Total precipitation 3hr" / "... run total".  Other
  // statistics name the operation: "Wind speed maximum 1hr" / "... maximum
  // since run start" (unless eccodes' own name already says it).
  const std::string word =
    sig.stat_process ? statistic_word(*sig.stat_process) : std::string();
  if (word.empty())
  {
    return name + " " + (s == 0 ? std::string("run total") : interval_text(s));
  }
  if (!contains_nocase(name, word))
  {
    name += " " + word;
  }
  return name + " " +
         (s == 0 ? std::string("since run start") : interval_text(s));
}

std::string catalog_api_name(
  const std::string& model_label,
  const std::string& grid_name)
{
  std::string out;
  const std::string in = grid_name.empty()     ? model_label
                         : model_label.empty() ? grid_name
                                               : model_label + "_" + grid_name;
  for (const char c0 : in)
  {
    const char c =
      static_cast<char>(std::tolower(static_cast<unsigned char>(c0)));
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-';
    out += ok ? c : '_';
    if (out.size() == 100)
    {
      break;
    }
  }
  return out.empty() ? "unnamed" : out;
}

CatalogConfig CatalogConfig::from_text(const std::string& text)
{
  using namespace detail;
  const std::string what = "catalog config";
  const auto j = parse_json(text, what);
  if (!j.is_object())
  {
    throw std::invalid_argument(what + ": not an object");
  }
  static const std::set<std::string> allowed = {"host", "port",     "socket",
                                                "user", "password", "database"};
  for (const auto& [k, v] : j.items())
  {
    if (!allowed.count(k))
    {
      throw std::invalid_argument(what + ": unknown key \"" + k + "\"");
    }
  }
  CatalogConfig c;
  auto opt_str = [&](const char* k, std::string& out)
  {
    if (j.contains(k))
    {
      out = get_str(j, k, what);
    }
  };
  opt_str("host", c.host);
  opt_str("socket", c.socket);
  opt_str("password", c.password);
  c.user = get_str(j, "user", what);
  c.database = get_str(j, "database", what);
  if (j.contains("port"))
  {
    const std::uint32_t p = get_u32(j, "port", what);
    if (p == 0 || p > 65535)
    {
      throw std::invalid_argument(what + ": bad port");
    }
    c.port = p;
  }
  if (c.user.empty() || c.database.empty())
  {
    throw std::invalid_argument(what + ": user and database are required");
  }
  if (!c.socket.empty() && !c.host.empty())
  {
    throw std::invalid_argument(what + ": give host/port or socket, not both");
  }
  return c;
}

CatalogConfig CatalogConfig::from_file(const std::filesystem::path& path)
{
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0)
  {
    throw std::invalid_argument("catalog config: cannot stat " + path.string());
  }
  if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0)
  {
    throw std::invalid_argument(
      "catalog config: " + path.string() +
      " must be readable only by its owner (chmod 600)");
  }
  return from_text(detail::read_text_file(path, "catalog config"));
}

}  // namespace atmo
