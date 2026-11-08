#include "atmo/gribstore.h"

#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "atmo/source.h"
#include "jsonutil.h"

namespace atmo
{
namespace
{

using nlohmann::json;

constexpr const char* kWhat = "grib index";

std::int64_t now_s()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

GridKey parse_key(const std::string& s)
{
  bool ok = s.size() == 16;
  for (char c : s)
  {
    ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
  if (!ok)
  {
    throw std::invalid_argument(
      std::string(kWhat) + ": bad hex key \"" + s + "\"");
  }
  return std::stoull(s, nullptr, 16);
}

}  // namespace

// ---------------------------------------------------------------------------
// GribIndex
// ---------------------------------------------------------------------------

void GribIndex::validate() const
{
  auto bad = [](const std::string& why)
  { return std::invalid_argument(std::string(kWhat) + ": " + why); };
  if (!SourceClass::valid_name(model))
  {
    throw bad("bad model name");
  }
  if (grib.empty())
  {
    throw bad("no grib path");
  }
  std::set<std::tuple<std::uint32_t, std::uint16_t, std::uint8_t, float>> seen;
  std::map<std::uint32_t, GridKey> grid_of_fhr;
  for (const auto& e : entries)
  {
    if (e.length == 0 || e.offset > size || e.length > size - e.offset)
    {
      throw bad("message outside the file");
    }
    if (!seen
           .insert(
             {e.fhr, e.info.param_id, e.info.level_type, e.info.level_value})
           .second)
    {
      throw bad("field listed twice for one forecast hour");
    }
    auto [it, fresh] = grid_of_fhr.insert({e.fhr, e.grid});
    if (!fresh && it->second != e.grid)
    {
      throw bad("two grids for one forecast hour");
    }
  }
}

json GribIndex::to_json() const
{
  json msgs = json::array();
  for (const auto& e : entries)
  {
    msgs.push_back(
      {{"offset", e.offset},
       {"length", e.length},
       {"hash", key_hex(e.hash)},
       {"param", e.info.param_id},
       {"level_type", e.info.level_type},
       {"level_value", e.info.level_value},
       {"flags", e.info.flags},
       {"fhr", e.fhr},
       {"grid", key_hex(e.grid)}});
  }
  json skips = json::array();
  for (const auto& [m, why] : skipped)
  {
    skips.push_back({{"message", m}, {"reason", why}});
  }
  return json{
    {"format", kFormat},
    {"version", kVersion},
    {"model", model},
    {"run", run_s},
    {"grib", grib},
    {"size", size},
    {"messages_total", messages_total},
    {"messages", std::move(msgs)},
    {"skipped", std::move(skips)}};
}

std::string GribIndex::to_text() const
{
  return to_json().dump(1, ' ') + "\n";
}

GribIndex GribIndex::from_json(const json& j)
{
  detail::check_keys(
    j, kWhat,
    {"format", "version", "model", "run", "grib", "size", "messages_total",
     "messages", "skipped"});
  detail::check_format(j, kWhat, kFormat, kVersion);
  GribIndex x;
  x.model = detail::get_str(j, "model", kWhat);
  x.run_s = detail::get_i64(j, "run", kWhat);
  x.grib = detail::get_str(j, "grib", kWhat);
  x.size = detail::get_u64(j, "size", kWhat);
  x.messages_total = detail::get_u64(j, "messages_total", kWhat);
  for (const auto& m : detail::get_array(j, "messages", kWhat))
  {
    detail::check_keys(
      m, kWhat,
      {"offset", "length", "hash", "param", "level_type", "level_value",
       "flags", "fhr", "grid"});
    GribIndexEntry e;
    e.offset = detail::get_u64(m, "offset", kWhat);
    e.length = detail::get_u64(m, "length", kWhat);
    e.hash = parse_key(detail::get_str(m, "hash", kWhat));
    const std::uint32_t param = detail::get_u32(m, "param", kWhat);
    const std::uint32_t lt = detail::get_u32(m, "level_type", kWhat);
    const std::uint32_t flags = detail::get_u32(m, "flags", kWhat);
    if (param > 65535 || lt > 255 || flags > 255)
    {
      throw std::invalid_argument(std::string(kWhat) + ": value out of range");
    }
    e.info.param_id = static_cast<std::uint16_t>(param);
    e.info.level_type = static_cast<std::uint8_t>(lt);
    e.info.flags = static_cast<std::uint8_t>(flags);
    e.info.level_value =
      static_cast<float>(detail::get_double(m, "level_value", kWhat));
    e.fhr = detail::get_u32(m, "fhr", kWhat);
    e.grid = parse_key(detail::get_str(m, "grid", kWhat));
    x.entries.push_back(e);
  }
  for (const auto& s : detail::get_array(j, "skipped", kWhat))
  {
    detail::check_keys(s, kWhat, {"message", "reason"});
    x.skipped.push_back(
      {detail::get_u64(s, "message", kWhat),
       detail::get_str(s, "reason", kWhat)});
  }
  x.validate();
  return x;
}

GribIndex GribIndex::from_text(std::string_view text)
{
  return from_json(detail::parse_json(text, kWhat));
}

std::size_t GribMessageData::bytes() const
{
  return values.capacity() * sizeof(double) + sizeof(*this);
}

// ---------------------------------------------------------------------------
// GribCache
// ---------------------------------------------------------------------------

struct GribCache::Impl
{
  using Ptr = std::shared_ptr<const GribMessageData>;

  GribCacheLimits limits;
  GribDecoder decoder;
  std::function<std::size_t()> rss;

  mutable std::mutex mu;
  std::unordered_map<std::uint64_t, Ptr> map;
  std::unordered_map<std::uint64_t, std::shared_future<Ptr>> inflight;
  std::size_t bytes = 0;
  Stats stats;

  // Drops unused entries, oldest access first, until bytes <= target.
  // Caller holds mu.
  std::size_t purge_locked(std::size_t target)
  {
    std::vector<std::pair<std::int64_t, std::uint64_t>> victims;
    for (const auto& [h, p] : map)
    {
      if (p.use_count() == 1)  // only the map holds it
      {
        victims.push_back({p->last_accessed.load(), h});
      }
    }
    std::sort(victims.begin(), victims.end());
    std::size_t n = 0;
    for (const auto& [t, h] : victims)
    {
      if (bytes <= target)
      {
        break;
      }
      auto it = map.find(h);
      bytes -= it->second->bytes();
      map.erase(it);
      ++n;
    }
    stats.evictions += n;
    return n;
  }

  bool rss_over() const
  {
    return limits.rss_limit != 0 && rss && rss() > limits.rss_limit;
  }

  std::size_t target() const
  {
    return static_cast<std::size_t>(
      static_cast<double>(limits.max_bytes) * limits.purge_to);
  }
};

GribCache::GribCache(
  GribCacheLimits limits,
  GribDecoder decoder,
  std::function<std::size_t()> rss)
    : impl_(std::make_unique<Impl>())
{
  if (!(limits.purge_to > 0.0 && limits.purge_to <= 1.0))
  {
    throw std::invalid_argument("GribCache: purge_to must be in (0, 1]");
  }
  impl_->limits = limits;
  impl_->decoder = std::move(decoder);
  impl_->rss = rss ? std::move(rss) : std::function<std::size_t()>(process_rss);
}

GribCache::~GribCache() = default;

std::shared_ptr<const GribMessageData> GribCache::get(
  const std::filesystem::path& file,
  const GribIndexEntry& entry,
  std::shared_ptr<const Grid> grid)
{
  Impl& im = *impl_;
  std::unique_lock<std::mutex> lk(im.mu);
  if (auto it = im.map.find(entry.hash); it != im.map.end())
  {
    ++im.stats.hits;
    it->second->last_accessed = now_s();
    return it->second;
  }
  if (auto it = im.inflight.find(entry.hash); it != im.inflight.end())
  {
    ++im.stats.coalesced;
    auto fut = it->second;
    lk.unlock();
    return fut.get();  // rethrows the decoder's exception, if any
  }
  if (!im.decoder)
  {
    throw std::runtime_error("GribCache: built without GRIB decoding");
  }
  std::promise<Impl::Ptr> promise;
  im.inflight[entry.hash] = promise.get_future().share();
  ++im.stats.misses;
  lk.unlock();

  Impl::Ptr data;
  try
  {
    auto d = im.decoder(file, entry, std::move(grid));
    d->hash = entry.hash;
    d->last_accessed = now_s();
    data = std::move(d);
  }
  catch (...)
  {
    promise.set_exception(std::current_exception());
    lk.lock();
    im.inflight.erase(entry.hash);
    throw;
  }

  lk.lock();
  im.map[entry.hash] = data;
  im.bytes += data->bytes();
  im.inflight.erase(entry.hash);
  // Purge on insert: over budget, or the process is too big.  `data` is
  // held here, so the new entry itself is never a victim.
  bool trim = false;
  if (im.bytes > im.limits.max_bytes)
  {
    im.purge_locked(im.target());
  }
  if (im.rss_over())
  {
    // The process is too big even if the cache is within budget: shed the
    // oldest unused (1 - purge_to) of the cache on every insert until RSS
    // is back under the limit (maintain() drops all unused entries).
    const auto shed = static_cast<std::size_t>(
      static_cast<double>(im.bytes) * im.limits.purge_to);
    trim = im.purge_locked(std::min(im.target(), shed)) > 0;
  }
  lk.unlock();
  if (trim)
  {
    ::malloc_trim(0);
  }
  promise.set_value(data);
  return data;
}

void GribCache::maintain()
{
  Impl& im = *impl_;
  std::unique_lock<std::mutex> lk(im.mu);
  if (!im.rss_over())
  {
    return;
  }
  // Out of control: everything nobody holds goes.
  const bool trim = im.purge_locked(0) > 0;
  lk.unlock();
  if (trim)
  {
    ::malloc_trim(0);
  }
}

GribCache::Stats GribCache::stats() const
{
  std::lock_guard<std::mutex> lk(impl_->mu);
  Stats s = impl_->stats;
  s.entries = impl_->map.size();
  s.bytes = impl_->bytes;
  return s;
}

std::size_t process_rss()
{
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f)
  {
    return 0;
  }
  unsigned long size = 0, resident = 0;
  const int n = std::fscanf(f, "%lu %lu", &size, &resident);
  std::fclose(f);
  if (n != 2)
  {
    return 0;
  }
  return static_cast<std::size_t>(resident) *
         static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
}

}  // namespace atmo
