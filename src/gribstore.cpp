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

std::int64_t now_s()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

}  // namespace

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
