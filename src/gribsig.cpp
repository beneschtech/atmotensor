// GRIB2 parameter signatures (T15).  See include/atmo/gribsig.h.
#include "atmo/gribsig.h"

#include <eccodes.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <stdexcept>

#include "atmo/grid.h"

namespace atmo
{

namespace
{

// kSignatureVersion's list.  07-grib-catalog.md lists the groups; eccodes
// spellings and aliases of the same keys are listed too, because the keys
// iterator returns whichever name the definition files use.
const std::vector<SignatureDenyRule> kDeny = {
  // time
  {"forecastTime", "time"},
  {"indicatorOfUnitOfTimeRange", "time"},
  {"indicatorOfUnitForForecastTime", "time"},
  {"indicatorOfUnitForTimeRange", "time"},
  {"indicatorOfUnitForTimeIncrement", "time"},
  {"hoursAfterDataCutoff", "time"},
  {"minutesAfterDataCutoff", "time"},
  {"yearOfEndOfOverallTimeInterval", "time"},
  {"monthOfEndOfOverallTimeInterval", "time"},
  {"dayOfEndOfOverallTimeInterval", "time"},
  {"hourOfEndOfOverallTimeInterval", "time"},
  {"minuteOfEndOfOverallTimeInterval", "time"},
  {"secondOfEndOfOverallTimeInterval", "time"},
  {"lengthOfTimeRange", "time"},
  {"timeIncrement", "time"},
  {"numberOfTimeRange", "time"},
  {"numberOfTimeRanges", "time"},
  {"numberOfMissingInStatisticalProcess", "time"},
  {"wallClockInitialTimeOfExecution*", "time"},
  {"yearOfForecastUsedInLocalTime", "time"},
  {"monthOfForecastUsedInLocalTime", "time"},
  {"dayOfForecastUsedInLocalTime", "time"},
  {"hourOfForecastUsedInLocalTime", "time"},
  {"minuteOfForecastUsedInLocalTime", "time"},
  {"secondOfForecastUsedInLocalTime", "time"},
  // level
  {"typeOfFirstFixedSurface", "level"},
  {"scaleFactorOfFirstFixedSurface", "level"},
  {"scaledValueOfFirstFixedSurface", "level"},
  {"typeOfSecondFixedSurface", "level"},
  {"scaleFactorOfSecondFixedSurface", "level"},
  {"scaledValueOfSecondFixedSurface", "level"},
  // ensemble member (stored on the run)
  {"perturbationNumber", "member"},
  {"typeOfEnsembleForecast", "member"},
  {"numberOfForecastsInEnsemble", "member"},
  // process bookkeeping
  {"typeOfGeneratingProcess", "process"},
  {"generatingProcessIdentifier", "process"},
  {"backgroundProcess", "process"},
  {"analysisInstant*", "process"},
  {"NV", "process"},
  {"numberOfCoordinatesValues", "process"},
  // model version date (v2, Q18b; templates 4.60/4.61 and the 4.13x
  // ones; eccodes spells it both ways)
  {"YearOfModelVersion", "model"},
  {"MonthOfModelVersion", "model"},
  {"DayOfModelVersion", "model"},
  {"HourOfModelVersion", "model"},
  {"MinuteOfModelVersion", "model"},
  {"SecondOfModelVersion", "model"},
  {"yearOfModelVersion", "model"},
  {"monthOfModelVersion", "model"},
  {"dayOfModelVersion", "model"},
  {"hourOfModelVersion", "model"},
  {"minuteOfModelVersion", "model"},
  {"secondOfModelVersion", "model"},
  {"dateOfModelVersion", "model"},
  {"timeOfModelVersion", "model"},
};

constexpr std::string_view kStatKey = "statInterval";

std::uint32_t u32(long v)
{
  return v < 0 ? 0u : static_cast<std::uint32_t>(v);
}

// Code table 4.4 unit -> seconds; nullopt for calendar units.
std::optional<std::int64_t> unit_seconds(long code)
{
  switch (code)
  {
    case 0:
      return 60;
    case 1:
      return 3600;
    case 2:
      return 86400;
    case 10:
      return 3 * 3600;
    case 11:
      return 6 * 3600;
    case 12:
      return 12 * 3600;
    case 13:
      return 1;
    default:
      return std::nullopt;
  }
}

// Nominal length for calendar units (only for stat_interval_s / offsets).
std::int64_t nominal_seconds(long code)
{
  if (auto s = unit_seconds(code))
  {
    return *s;
  }
  switch (code)
  {
    case 3:
      return 30LL * 86400;
    case 4:
      return 365LL * 86400;
    case 5:
      return 3650LL * 86400;
    case 6:
      return 30LL * 365 * 86400;
    case 7:
      return 36500LL * 86400;
    default:
      throw std::runtime_error(
        "grib signature: unknown time unit " + std::to_string(code));
  }
}

std::int64_t utc(long y, long mo, long d, long h, long mi, long s)
{
  std::tm t{};
  t.tm_year = static_cast<int>(y - 1900);
  t.tm_mon = static_cast<int>(mo - 1);
  t.tm_mday = static_cast<int>(d);
  t.tm_hour = static_cast<int>(h);
  t.tm_min = static_cast<int>(mi);
  t.tm_sec = static_cast<int>(s);
  return static_cast<std::int64_t>(::timegm(&t));
}

double level_value(long factor, long scaled)
{
  if (scaled == CODES_MISSING_LONG)
  {
    return 0.0;
  }
  if (factor == CODES_MISSING_LONG)
  {
    factor = 0;
  }
  double v = static_cast<double>(scaled);
  double p = 1.0;
  for (long i = 0; i < (factor < 0 ? -factor : factor); ++i)
  {
    p *= 10.0;
  }
  return factor >= 0 ? v / p : v * p;
}

std::string hex(const std::uint8_t* p, std::size_t n)
{
  static const char* d = "0123456789abcdef";
  std::string s(2 * n, '0');
  for (std::size_t i = 0; i < n; ++i)
  {
    s[2 * i] = d[p[i] >> 4];
    s[2 * i + 1] = d[p[i] & 15];
  }
  return s;
}

std::uint64_t be(const std::uint8_t* p, int n)
{
  std::uint64_t v = 0;
  for (int i = 0; i < n; ++i)
  {
    v = (v << 8) | p[i];
  }
  return v;
}

void finish(GribSignature& sig, const std::map<std::string, std::string>& kv)
{
  const std::string head = "v" + std::to_string(kSignatureVersion) + "\n";
  std::string base = head;
  std::string full = head;
  for (const auto& [k, v] : kv)
  {
    std::string line = k + "=" + v + "\n";
    full += line;
    if (k != kStatKey)
    {
      base += line;
    }
  }
  sig.version = kSignatureVersion;
  sig.canonical = full;
  sig.hash = signature_hash(full);
  sig.base_hash = signature_hash(base);
}

struct HandleDeleter
{
  void operator()(codes_handle* h) const { codes_handle_delete(h); }
};

struct IterDeleter
{
  void operator()(codes_keys_iterator* it) const
  {
    codes_keys_iterator_delete(it);
  }
};

std::string get_string(codes_handle* h, const char* key)
{
  char buf[512];
  std::size_t n = sizeof buf;
  if (codes_get_string(h, key, buf, &n) != 0)
  {
    return {};
  }
  return std::string(buf, n > 0 && buf[n - 1] == '\0' ? n - 1 : n);
}

// A code table 4.2 column; empty where eccodes has no entry and answers
// the bare number (or "unknown"/"Missing").
std::string table_entry(codes_handle* h, const char* key)
{
  std::string v = get_string(h, key);
  const bool number =
    !v.empty() && v.find_first_not_of("0123456789") == std::string::npos;
  if (number || v == "unknown" || v == "Missing" || v == "missing")
  {
    return {};
  }
  return v;
}

long get_long(codes_handle* h, const char* key, long fallback)
{
  long v = 0;
  return codes_get_long(h, key, &v) == 0 ? v : fallback;
}

// The first occurrence's value of a section 4 key, if present.
struct Sec4
{
  std::vector<std::pair<std::string, long>> keys;  // numbered names

  const long* find(std::string_view name) const
  {
    for (const auto& [k, v] : keys)
    {
      if (k == name)
      {
        return &v;
      }
    }
    return nullptr;
  }
  long get(std::string_view name, long fallback) const
  {
    const long* p = find(name);
    return p ? *p : fallback;
  }
  // Every occurrence of `name` in order (name, name#2, ...).
  std::vector<long> all(const std::string& name) const
  {
    std::vector<long> out;
    if (const long* p = find(name))
    {
      out.push_back(*p);
      for (int i = 2;; ++i)
      {
        const long* q = find(name + "#" + std::to_string(i));
        if (!q)
        {
          break;
        }
        out.push_back(*q);
      }
    }
    return out;
  }
};

Sec4 read_section4(codes_handle* h)
{
  long o4 = 0;
  long l4 = 0;
  if (
    codes_get_long(h, "offsetSection4", &o4) != 0 ||
    codes_get_long(h, "section4Length", &l4) != 0)
  {
    throw std::runtime_error("grib signature: no section 4");
  }
  const unsigned long flags = CODES_KEYS_ITERATOR_SKIP_COMPUTED |
                              CODES_KEYS_ITERATOR_SKIP_READ_ONLY |
                              CODES_KEYS_ITERATOR_SKIP_FUNCTION;
  std::unique_ptr<codes_keys_iterator, IterDeleter> it(
    codes_keys_iterator_new(h, flags, nullptr));
  if (!it)
  {
    throw std::runtime_error("grib signature: cannot iterate keys");
  }
  std::map<std::string, std::pair<bool, int>> seen;  // in sec4, count
  Sec4 out;
  while (codes_keys_iterator_next(it.get()))
  {
    const std::string name = codes_keys_iterator_get_name(it.get());
    auto [pos, fresh] = seen.try_emplace(name, false, 0);
    if (fresh)
    {
      std::size_t off = 0;
      pos->second.first = codes_get_offset(h, name.c_str(), &off) == 0 &&
                          off >= static_cast<std::size_t>(o4) &&
                          off < static_cast<std::size_t>(o4 + l4);
    }
    if (!pos->second.first)
    {
      continue;
    }
    long v = 0;
    std::size_t n = 1;
    if (codes_keys_iterator_get_long(it.get(), &v, &n) != 0 || n != 1)
    {
      continue;  // not an integer key
    }
    const int k = ++pos->second.second;
    out.keys.emplace_back(k == 1 ? name : name + "#" + std::to_string(k), v);
  }
  return out;
}

std::string value_text(long v)
{
  return v == CODES_MISSING_LONG ? std::string("missing") : std::to_string(v);
}

// The undecodable fallback and describe_grib(bytes) share these checks.
void check_grib2(const std::uint8_t* p, std::size_t len)
{
  if (len < 16 || std::memcmp(p, "GRIB", 4) != 0)
  {
    throw std::runtime_error("grib signature: not a GRIB message");
  }
  if (p[7] != 2)
  {
    throw std::runtime_error(
      "grib signature: GRIB edition " + std::to_string(p[7]) +
      " not supported");
  }
}

}  // namespace

bool GribSignature::identified() const
{
  return !undecodable && ecc_param_id.has_value() &&
         ecc_short_name != "unknown" && !ecc_short_name.empty();
}

const std::vector<SignatureDenyRule>& signature_deny_list()
{
  return kDeny;
}

bool signature_denies(std::string_view key)
{
  // Numbered repeats (key#2) are denied with their key.
  const auto hash = key.find('#');
  if (hash != std::string_view::npos)
  {
    key = key.substr(0, hash);
  }
  for (const auto& r : kDeny)
  {
    std::string_view rule = r.key;
    if (!rule.empty() && rule.back() == '*')
    {
      if (key.substr(0, rule.size() - 1) == rule.substr(0, rule.size() - 1))
      {
        return true;
      }
    }
    else if (key == rule)
    {
      return true;
    }
  }
  return false;
}

std::uint64_t signature_hash(std::string_view canonical)
{
  return fnv1a64(canonical.data(), canonical.size());
}

std::vector<std::string> signature_diff(std::string_view a, std::string_view b)
{
  auto parse = [](std::string_view t)
  {
    std::map<std::string, std::string> m;
    std::size_t i = 0;
    bool first = true;
    while (i < t.size())
    {
      std::size_t e = t.find('\n', i);
      if (e == std::string_view::npos)
      {
        e = t.size();
      }
      std::string_view line = t.substr(i, e - i);
      i = e + 1;
      if (first)
      {
        first = false;
        if (line.find('=') == std::string_view::npos)
        {
          continue;  // version line
        }
      }
      const auto eq = line.find('=');
      if (eq == std::string_view::npos)
      {
        continue;
      }
      m[std::string(line.substr(0, eq))] = std::string(line.substr(eq + 1));
    }
    return m;
  };
  const auto ma = parse(a);
  const auto mb = parse(b);
  std::vector<std::string> out;
  for (const auto& [k, v] : ma)
  {
    auto it = mb.find(k);
    if (it == mb.end())
    {
      out.push_back(k + ": " + v + " -> (absent)");
    }
    else if (it->second != v)
    {
      out.push_back(k + ": " + v + " -> " + it->second);
    }
  }
  for (const auto& [k, v] : mb)
  {
    if (!ma.count(k))
    {
      out.push_back(k + ": (absent) -> " + v);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

GribHeader describe_grib(grib_handle* h)
{
  if (get_long(h, "edition", 0) != 2)
  {
    throw std::runtime_error("grib signature: not GRIB2");
  }
  GribHeader out;
  GribSignature& sig = out.sig;
  sig.discipline = u32(get_long(h, "discipline", 255));
  sig.centre = u32(get_long(h, "centre", 65535));
  out.subcentre = u32(get_long(h, "subCentre", 0));
  out.base_time_s = utc(
    get_long(h, "year", 1970), get_long(h, "month", 1), get_long(h, "day", 1),
    get_long(h, "hour", 0), get_long(h, "minute", 0), get_long(h, "second", 0));

  // An unknown template leaves eccodes with a half-parsed handle (it logs
  // "Unable to find template" but still returns one): no section 5.
  long s5 = 0;
  if (codes_get_long(h, "section5Length", &s5) != 0)
  {
    throw std::runtime_error("grib signature: undecodable message");
  }
  const Sec4 s4 = read_section4(h);
  sig.pdt = u32(s4.get(
    "productDefinitionTemplateNumber",
    get_long(h, "productDefinitionTemplateNumber", 0)));
  sig.category = u32(s4.get("parameterCategory", 255));
  sig.number = u32(s4.get("parameterNumber", 255));
  out.process = u32(s4.get("generatingProcessIdentifier", 255));

  std::map<std::string, std::string> kv;
  kv["discipline"] = std::to_string(sig.discipline);
  kv["centre"] = std::to_string(sig.centre);
  kv["productDefinitionTemplateNumber"] = std::to_string(sig.pdt);
  for (const auto& [k, v] : s4.keys)
  {
    if (!signature_denies(k))
    {
      kv[k] = value_text(v);
    }
  }

  // Time.
  const long ft = s4.get("forecastTime", 0);
  const long ft_unit = s4.get(
    "indicatorOfUnitForForecastTime", s4.get("indicatorOfUnitOfTimeRange", 1));
  out.offset_start_s = static_cast<std::int64_t>(ft) * nominal_seconds(ft_unit);
  out.offset_end_s = out.offset_start_s;
  const std::vector<long> lengths = s4.all("lengthOfTimeRange");
  const std::vector<long> units = s4.all("indicatorOfUnitForTimeRange");
  if (!lengths.empty())
  {
    std::string text;
    for (std::size_t i = 0; i < lengths.size(); ++i)
    {
      const long unit = i < units.size() ? units[i] : 1;
      std::string part;
      if (i == 0 && ft == 0)
      {
        part = "run";
        sig.stat_interval_s = 0;
      }
      else if (auto us = unit_seconds(unit))
      {
        part = std::to_string(static_cast<std::int64_t>(lengths[i]) * *us);
      }
      else
      {
        part = std::to_string(lengths[i]) + "u" + std::to_string(unit);
      }
      if (i == 0 && !sig.stat_interval_s)
      {
        const std::int64_t s =
          static_cast<std::int64_t>(lengths[0]) * nominal_seconds(unit);
        sig.stat_interval_s = static_cast<std::uint32_t>(
          std::clamp<std::int64_t>(s, 1, UINT32_MAX));
      }
      text += (i ? "," : "") + part;
    }
    kv[std::string(kStatKey)] = text;
    if (const long* tp = s4.find("typeOfStatisticalProcessing"))
    {
      if (*tp >= 0 && *tp != CODES_MISSING_LONG)
      {
        sig.stat_process = static_cast<std::uint32_t>(*tp);
      }
    }
    // End = start + the first (overall) range, as eccodes' endStep; the
    // end-of-interval date is used only for calendar units.
    const long unit0 = units.empty() ? 1 : units[0];
    const long ey = s4.get("yearOfEndOfOverallTimeInterval", -1);
    if (!unit_seconds(unit0) && ey > 0)
    {
      out.offset_end_s = utc(
                           ey, s4.get("monthOfEndOfOverallTimeInterval", 1),
                           s4.get("dayOfEndOfOverallTimeInterval", 1),
                           s4.get("hourOfEndOfOverallTimeInterval", 0),
                           s4.get("minuteOfEndOfOverallTimeInterval", 0),
                           s4.get("secondOfEndOfOverallTimeInterval", 0)) -
                         out.base_time_s;
    }
    else
    {
      out.offset_end_s =
        out.offset_start_s +
        static_cast<std::int64_t>(lengths[0]) * nominal_seconds(unit0);
    }
  }
  out.valid_time_s = out.base_time_s + out.offset_end_s;

  // Levels.
  auto level = [&](const char* type, const char* factor, const char* value)
  {
    GribLevel l;
    const long t = s4.get(type, 255);
    if (t == CODES_MISSING_LONG || t == 255 || t < 0)
    {
      return l;
    }
    l.type = u32(t);
    l.value = level_value(s4.get(factor, 0), s4.get(value, CODES_MISSING_LONG));
    return l;
  };
  out.level1 = level(
    "typeOfFirstFixedSurface", "scaleFactorOfFirstFixedSurface",
    "scaledValueOfFirstFixedSurface");
  out.level2 = level(
    "typeOfSecondFixedSurface", "scaleFactorOfSecondFixedSurface",
    "scaledValueOfSecondFixedSurface");

  // Ensemble member.
  if (const long* pn = s4.find("perturbationNumber"))
  {
    out.member = *pn == CODES_MISSING_LONG ? 0 : static_cast<int>(*pn);
    if (const long* te = s4.find("typeOfEnsembleForecast"))
    {
      out.member_type = static_cast<int>(*te);
    }
  }

  sig.ecc_short_name = get_string(h, "shortName");
  sig.ecc_name = get_string(h, "name");
  sig.ecc_units = get_string(h, "units");
  sig.table_name = table_entry(h, "parameterName");
  if (!sig.table_name.empty())
  {
    sig.table_units = table_entry(h, "parameterUnits");
  }
  long pid = 0;
  if (codes_get_long(h, "paramId", &pid) == 0 && pid > 0)
  {
    sig.ecc_param_id = pid;
  }

  finish(sig, kv);
  return out;
}

GribHeader describe_grib(const void* msg, std::size_t len)
{
  check_grib2(static_cast<const std::uint8_t*>(msg), len);
  std::unique_ptr<codes_handle, HandleDeleter> h(
    codes_handle_new_from_message(nullptr, msg, len));
  if (h)
  {
    try
    {
      return describe_grib(h.get());
    }
    catch (const std::runtime_error&)
    {
      // fall through to the raw signature
    }
  }
  return describe_grib_raw(msg, len);
}

GribHeader describe_grib_raw(const void* msg, std::size_t len)
{
  const auto* p = static_cast<const std::uint8_t*>(msg);
  check_grib2(p, len);
  GribHeader out;
  GribSignature& sig = out.sig;
  sig.undecodable = true;
  sig.discipline = p[6];
  const std::uint64_t total = be(p + 8, 8);
  if (total > len || total < 16 + 4)
  {
    throw std::runtime_error("grib signature: truncated message");
  }
  const std::uint8_t* s4 = nullptr;
  std::size_t l4 = 0;
  std::size_t i = 16;
  while (i + 5 <= total)
  {
    if (std::memcmp(p + i, "7777", 4) == 0)
    {
      break;
    }
    const std::uint64_t sl = be(p + i, 4);
    if (sl < 5 || i + sl > total)
    {
      throw std::runtime_error("grib signature: bad section length");
    }
    const std::uint8_t num = p[i + 4];
    const std::uint8_t* s = p + i;
    if (num == 1 && sl >= 19)
    {
      sig.centre = static_cast<std::uint32_t>(be(s + 5, 2));
      out.subcentre = static_cast<std::uint32_t>(be(s + 7, 2));
      out.base_time_s = utc(
        static_cast<long>(be(s + 12, 2)), s[14], s[15], s[16], s[17], s[18]);
    }
    else if (num == 4 && !s4)
    {
      s4 = s;
      l4 = static_cast<std::size_t>(sl);
    }
    i += static_cast<std::size_t>(sl);
  }
  if (!s4 || l4 < 11)
  {
    throw std::runtime_error("grib signature: no section 4");
  }
  sig.pdt = static_cast<std::uint32_t>(be(s4 + 7, 2));
  sig.category = s4[9];
  sig.number = s4[10];
  // Octet 14 is the process id in the common prefix of nearly all
  // templates; anything shorter keeps "missing".
  out.process = l4 >= 14 ? s4[13] : 255u;
  out.valid_time_s = out.base_time_s;
  std::map<std::string, std::string> kv;
  kv["centre"] = std::to_string(sig.centre);
  kv["discipline"] = std::to_string(sig.discipline);
  kv["productDefinitionTemplateNumber"] = std::to_string(sig.pdt);
  kv["raw4"] = hex(s4 + 9, l4 - 9);
  sig.ecc_short_name = "unknown";
  finish(sig, kv);
  return out;
}

std::vector<GribSpan> grib_message_spans(const void* data, std::size_t len)
{
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::vector<GribSpan> out;
  std::size_t i = 0;
  while (i + 8 <= len)
  {
    const void* hit = std::memchr(p + i, 'G', len - i);
    if (!hit)
    {
      break;
    }
    i = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - p);
    if (i + 8 > len || std::memcmp(p + i, "GRIB", 4) != 0)
    {
      ++i;
      continue;
    }
    const std::uint8_t ed = p[i + 7];
    std::uint64_t n = 0;
    if (ed == 2)
    {
      if (i + 16 > len)
      {
        throw std::runtime_error("grib: truncated message header");
      }
      n = be(p + i + 8, 8);
    }
    else if (ed == 1)
    {
      n = be(p + i + 4, 3);
    }
    else
    {
      ++i;
      continue;
    }
    if (n < 16 || n > len - i)
    {
      throw std::runtime_error(
        "grib: truncated message at offset " + std::to_string(i));
    }
    out.push_back({i, n, ed});
    i += static_cast<std::size_t>(n);
  }
  return out;
}

}  // namespace atmo
