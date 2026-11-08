#include "atmo/refdata.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <tuple>

#include "fileio.h"
#include "jsonutil.h"

namespace atmo
{
namespace
{

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kWho = "refdata";

struct IdTypeName
{
  StationIdType type;
  const char* name;
};
constexpr IdTypeName kIdTypes[] = {
  {StationIdType::icao, "icao"},
  {StationIdType::wmo, "wmo"},
  {StationIdType::wigos, "wigos"},
  {StationIdType::platform, "platform"},
};

struct FlagName
{
  std::uint16_t flag;
  const char* name;
};
constexpr FlagName kFlags[] = {
  {kStationProvisional, "provisional"},
  {kStationMobile, "mobile"},
};

json time_to_json(std::int64_t t, std::int64_t open)
{
  return t == open ? json(nullptr) : json(t);
}

std::int64_t time_from_json(
  const json& j,
  const char* key,
  std::int64_t open,
  std::string_view what)
{
  return j.at(key).is_null() ? open : detail::get_i64(j, key, what);
}

float to_f32(double v, const char* key, std::string_view what)
{
  // Positions are stored as f32 (docs/CONVENTIONS.md); range checks are
  // the builder's.
  if (v > 1e30 || v < -1e30)
  {
    throw std::invalid_argument(
      std::string(what) + ": \"" + key + "\" out of range");
  }
  return static_cast<float>(v);
}

// Identifiers in a canonical order, so that a station read back from a
// table compares equal to the same station from an export.
Station canonical(Station s)
{
  std::sort(
    s.ids.begin(), s.ids.end(),
    [](const StationIdBinding& a, const StationIdBinding& b)
    {
      return std::make_tuple(
               static_cast<std::uint16_t>(a.id.type), a.id.value,
               a.valid_from_s, a.valid_to_s) <
             std::make_tuple(
               static_cast<std::uint16_t>(b.id.type), b.id.value,
               b.valid_from_s, b.valid_to_s);
    });
  return s;
}

std::string outbox_name(std::uint64_t generation)
{
  char buf[64];
  std::snprintf(buf, sizeof buf, "provisional-%016" PRIx64 ".json", generation);
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// StationsExport
// ---------------------------------------------------------------------------

json StationsExport::station_to_json(const Station& s)
{
  json flags = json::array();
  for (const auto& f : kFlags)
  {
    if (s.flags & f.flag)
    {
      flags.push_back(f.name);
    }
  }
  json ids = json::array();
  for (const auto& b : s.ids)
  {
    const char* type = "?";
    for (const auto& t : kIdTypes)
    {
      if (t.type == b.id.type)
      {
        type = t.name;
      }
    }
    ids.push_back(
      {{"type", type},
       {"value", b.id.value},
       {"valid_from", time_to_json(b.valid_from_s, kTimeMin)},
       {"valid_to", time_to_json(b.valid_to_s, kTimeMax)}});
  }
  json periods = json::array();
  for (const auto& p : s.periods)
  {
    periods.push_back(
      {{"valid_from", time_to_json(p.valid_from_s, kTimeMin)},
       {"valid_to", time_to_json(p.valid_to_s, kTimeMax)},
       {"lat_deg", p.lat_deg},
       {"lon_deg", p.lon_deg},
       {"elevation_m", p.elevation_m}});
  }
  return json{
    {"ref", s.ref},
    {"name", s.name},
    {"flags", std::move(flags)},
    {"merged_into", s.merged_into == 0 ? json(nullptr) : json(s.merged_into)},
    {"ids", std::move(ids)},
    {"periods", std::move(periods)}};
}

Station StationsExport::station_from_json(const json& j)
{
  detail::check_keys(
    j, "station", {"ref", "name", "flags", "merged_into", "ids", "periods"});
  Station s;
  s.ref = detail::get_u32(j, "ref", "station");
  const std::string what = "station " + std::to_string(s.ref);
  if (s.ref == 0)
  {
    throw std::invalid_argument("station: \"ref\" must be >= 1");
  }
  s.name = detail::get_str(j, "name", what);
  for (const auto& f : detail::get_array(j, "flags", what))
  {
    std::uint16_t bit = 0;
    for (const auto& k : kFlags)
    {
      if (f.is_string() && f == k.name)
      {
        bit = k.flag;
      }
    }
    if (bit == 0 || (s.flags & bit))
    {
      throw std::invalid_argument(
        what + ": unknown or repeated flag " + f.dump());
    }
    s.flags = static_cast<std::uint16_t>(s.flags | bit);
  }
  if (!j.at("merged_into").is_null())
  {
    s.merged_into = detail::get_u32(j, "merged_into", what);
    if (s.merged_into == 0)
    {
      throw std::invalid_argument(what + ": \"merged_into\" must be >= 1");
    }
  }
  for (const auto& i : detail::get_array(j, "ids", what))
  {
    const std::string iw = what + ": id";
    detail::check_keys(i, iw, {"type", "value", "valid_from", "valid_to"});
    const std::string type = detail::get_str(i, "type", iw);
    StationIdBinding b;
    bool known = false;
    for (const auto& t : kIdTypes)
    {
      if (type == t.name)
      {
        b.id.type = t.type;
        known = true;
      }
    }
    if (!known)
    {
      throw std::invalid_argument(iw + ": unknown type \"" + type + "\"");
    }
    b.id.value = detail::get_str(i, "value", iw);
    b.valid_from_s = time_from_json(i, "valid_from", kTimeMin, iw);
    b.valid_to_s = time_from_json(i, "valid_to", kTimeMax, iw);
    s.ids.push_back(std::move(b));
  }
  for (const auto& p : detail::get_array(j, "periods", what))
  {
    const std::string pw = what + ": period";
    detail::check_keys(
      p, pw, {"valid_from", "valid_to", "lat_deg", "lon_deg", "elevation_m"});
    StationPeriod q;
    q.valid_from_s = time_from_json(p, "valid_from", kTimeMin, pw);
    q.valid_to_s = time_from_json(p, "valid_to", kTimeMax, pw);
    q.lat_deg = to_f32(detail::get_double(p, "lat_deg", pw), "lat_deg", pw);
    q.lon_deg = to_f32(detail::get_double(p, "lon_deg", pw), "lon_deg", pw);
    q.elevation_m =
      to_f32(detail::get_double(p, "elevation_m", pw), "elevation_m", pw);
    s.periods.push_back(q);
  }
  return s;
}

StationsExport StationsExport::from_json(const json& j)
{
  detail::check_format(j, "stations export", kFormat, kVersion);
  StationsExport ex;
  std::set<std::uint32_t> refs;
  for (const auto& e : detail::get_array(j, "stations", "stations export"))
  {
    Station s = station_from_json(e);
    if (!refs.insert(s.ref).second)
    {
      throw std::invalid_argument(
        "stations export: ref " + std::to_string(s.ref) + " listed twice");
    }
    ex.stations.push_back(std::move(s));
  }
  return ex;
}

StationsExport StationsExport::from_text(std::string_view text)
{
  return from_json(detail::parse_json(text, "stations export"));
}

StationsExport StationsExport::from_file(const fs::path& path)
{
  std::string text;
  try
  {
    text = detail::read_text_file(path, "stations export");
  }
  catch (const std::runtime_error& e)
  {
    throw std::invalid_argument(e.what());
  }
  try
  {
    return from_text(text);
  }
  catch (const std::invalid_argument& e)
  {
    throw std::invalid_argument(path.string() + ": " + e.what());
  }
}

json StationsExport::to_json(const char* format) const
{
  json arr = json::array();
  for (const auto& s : stations)
  {
    arr.push_back(station_to_json(s));
  }
  return json{
    {"format", format}, {"version", kVersion}, {"stations", std::move(arr)}};
}

// ---------------------------------------------------------------------------
// RefDataImporter
// ---------------------------------------------------------------------------

RefDataImporter::RefDataImporter(ManifestPublisher& publisher)
    : pub_(publisher), root_(publisher.root()), config_(root_ / "config")
{
  // Best effort: which refs the web app already knows.  A missing or
  // invalid export just means every provisional station is pending.
  try
  {
    for (const auto& s :
         StationsExport::from_file(config_ / "stations.json").stations)
    {
      exported_refs_.insert(s.ref);
    }
  }
  catch (const std::invalid_argument&)
  {
  }
  if (auto t = current_stations())
  {
    write_outbox(*t);
  }
}

RefDataImporter::FileStamp RefDataImporter::stamp(const fs::path& p)
{
  struct stat st;
  FileStamp f;
  if (::stat(p.c_str(), &st) != 0)
  {
    return f;
  }
  f.exists = true;
  f.dev = static_cast<std::uint64_t>(st.st_dev);
  f.ino = static_cast<std::uint64_t>(st.st_ino);
  f.size = static_cast<std::uint64_t>(st.st_size);
  f.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000 +
               st.st_mtim.tv_nsec;
  return f;
}

std::shared_ptr<const StationTable> RefDataImporter::current_stations() const
{
  const auto& e = pub_.current().stations;
  if (!e)
  {
    return nullptr;
  }
  return StationTable::open(root_ / e->path, StationTable::Verify::header);
}

StationsEntry RefDataImporter::seal_generation(StationTableBuilder& b) const
{
  const fs::path dir = root_ / "stations";
  const fs::path path = dir / StationTable::file_name(b.generation());
  // The builder's generation is one above the published one, so a file
  // with that name is a leftover of a publish that never happened: no
  // manifest names it (generations never go backwards).
  std::error_code ec;
  if (fs::exists(path, ec))
  {
    fs::remove(path);
  }
  const fs::path sealed = b.seal(dir);
  auto t = StationTable::open(sealed, StationTable::Verify::full);
  return StationsEntry{
    t->generation(), "stations/" + sealed.filename().string(), t->header_crc()};
}

void RefDataImporter::write_outbox(const StationTable& table) const
{
  StationsExport pending;
  for (std::size_t i = 0; i < table.size(); ++i)
  {
    const StationView v = table.station(i);
    if (v.ref() >= kProvisionalRefBase && !exported_refs_.count(v.ref()))
    {
      pending.stations.push_back(v.to_station());
    }
  }
  if (pending.stations.empty())
  {
    return;
  }
  const fs::path path = config_ / "outbox" / outbox_name(table.generation());
  std::error_code ec;
  if (fs::exists(path, ec))
  {
    return;
  }
  json j = pending.to_json(StationsExport::kOutboxFormat);
  j["generation"] = table.generation();
  const std::string text = j.dump(1, ' ') + "\n";
  detail::publish_file(
    path, std::vector<unsigned char>(text.begin(), text.end()), kWho);
}

RefImportReport RefDataImporter::poll()
{
  RefImportReport r;
  const fs::path src_path = config_ / "sources.json";
  const fs::path sta_path = config_ / "stations.json";
  const FileStamp src = stamp(src_path), sta = stamp(sta_path);
  const bool src_new = !seen_sources_ || *seen_sources_ != src;
  const bool sta_new = !seen_stations_ || *seen_stations_ != sta;
  if (!src_new && !sta_new)
  {
    return r;
  }

  Manifest next = pub_.current();
  bool changed = false;

  if (src_new && src.exists)
  {
    try
    {
      SourceRegistry reg = SourceRegistry::from_file(src_path);
      for (const auto& seg : next.segments)
      {
        if (!reg.find(seg.source))
        {
          throw std::invalid_argument(
            "source class \"" + seg.source +
            "\" removed while segments of it are still retained");
        }
      }
      if (!(reg == next.sources))
      {
        next.sources = std::move(reg);
        r.sources_updated = true;
        changed = true;
      }
    }
    catch (const std::invalid_argument& e)
    {
      r.errors.push_back(src_path.string() + ": " + e.what());
    }
  }

  if (next.sources.size() == 0)
  {
    // Nothing can be published yet.  Leave stations.json unseen so it is
    // imported as soon as a valid sources.json arrives.
    if (src_new && !src.exists)
    {
      r.errors.push_back(
        src_path.string() + ": missing; nothing is published until it exists");
    }
    seen_sources_ = src;
    return r;
  }

  std::set<std::uint32_t> exported;
  bool have_export = false;
  if (sta_new && sta.exists)
  {
    try
    {
      const StationsExport ex = StationsExport::from_file(sta_path);
      auto prev = current_stations();
      StationTableBuilder b =
        prev ? StationTableBuilder(*prev) : StationTableBuilder();
      bool sta_changed = !prev;
      for (const auto& s : ex.stations)
      {
        exported.insert(s.ref);
        const Station* old = b.find(s.ref);
        if (old)
        {
          if (canonical(*old) != canonical(s))
          {
            b.update(s);
            sta_changed = true;
          }
        }
        else if (s.ref >= kProvisionalRefBase)
        {
          throw std::invalid_argument(
            "ref " + std::to_string(s.ref) +
            " is in the provisional range but ingest never assigned it");
        }
        else
        {
          b.add(s);
          sta_changed = true;
        }
      }
      if (prev)
      {
        for (std::size_t i = 0; i < prev->size(); ++i)
        {
          const std::uint32_t ref = prev->station(i).ref();
          if (ref < kProvisionalRefBase && !exported.count(ref))
          {
            throw std::invalid_argument(
              "station ref " + std::to_string(ref) +
              " is missing (refs are never removed)");
          }
        }
      }
      if (sta_changed)
      {
        next.stations = seal_generation(b);
        r.stations_updated = true;
        changed = true;
      }
      have_export = true;
    }
    catch (const std::invalid_argument& e)
    {
      r.errors.push_back(sta_path.string() + ": " + e.what());
    }
    catch (const std::out_of_range& e)
    {
      r.errors.push_back(sta_path.string() + ": " + e.what());
    }
  }

  if (changed)
  {
    r.published_seq = pub_.publish(next).seq;
  }
  // Only now: if publishing threw, the files are retried on the next poll.
  seen_sources_ = src;
  seen_stations_ = sta;
  if (have_export)
  {
    exported_refs_ = std::move(exported);
  }
  return r;
}

std::vector<std::uint32_t> RefDataImporter::add_provisional(
  std::vector<Station> stations)
{
  std::vector<std::uint32_t> refs;
  if (stations.empty())
  {
    return refs;
  }
  if (pub_.current().sources.size() == 0)
  {
    throw std::invalid_argument(
      "refdata: cannot publish before config/sources.json was imported");
  }
  auto prev = current_stations();
  StationTableBuilder b =
    prev ? StationTableBuilder(*prev) : StationTableBuilder();
  for (auto& s : stations)
  {
    if (s.ref != 0)
    {
      throw std::invalid_argument(
        "refdata: provisional stations get their ref from ingest (ref 0)");
    }
    s.flags = static_cast<std::uint16_t>(s.flags | kStationProvisional);
    refs.push_back(b.add(std::move(s)));
  }
  Manifest next = pub_.current();
  next.stations = seal_generation(b);
  pub_.publish(next);
  write_outbox(*StationTable::open(
    root_ / next.stations->path, StationTable::Verify::header));
  return refs;
}

}  // namespace atmo
