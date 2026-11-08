// atmo_queryd: the query daemon (T10).  A FastCGI responder behind nginx
// serving /healthz, /v1/meta and /v1/point over the published snapshots
// (04-query-api.md).  Read-only on the store.
//
//   atmo_queryd --root /opt/atmotensor/data [--socket /run/atmo/query.sock]
//               [--tcp-port N] [--threads N] [--queue N]
//               [--cache-mb N] [--rss-limit-mb N]
//
// GRIB-native data is decoded on demand into a cache of --cache-mb
// (default 4096); when the process RSS exceeds --rss-limit-mb (default
// 2 x cache + 1024; 0 = off) unused entries are dropped, also by a
// cleanup that runs every 5 s.
//
// SIGTERM/SIGINT: stop accepting, finish in-flight requests, exit.
// SIGHUP: re-read the manifest now and log cache statistics.
#include <pthread.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>

#include "atmo/fcgi.h"
#ifdef ATMO_HAVE_ECCODES
#include "atmo/grib.h"
#endif
#include "atmo/gribstore.h"
#include "atmo/http_api.h"
#include "atmo/snapshot.h"

namespace
{

void log(const std::string& msg)
{
  std::fprintf(stderr, "atmo_queryd: %s\n", msg.c_str());
}

int usage()
{
  std::fprintf(
    stderr,
    "usage: atmo_queryd --root DIR [--socket PATH] [--tcp-port N]\n"
    "                   [--threads N] [--queue N] [--cache-mb N]\n"
    "                   [--rss-limit-mb N]\n"
    "  --socket defaults to /run/atmo/query.sock; --tcp-port listens on\n"
    "  127.0.0.1 as well (0 = no TCP).\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv)
{
  std::string root;
  long cache_mb = 4096, rss_mb = -1;
  atmo::fcgi::ServerOptions opt;
  opt.unix_path = "/run/atmo/query.sock";
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--root")
      root = next();
    else if (a == "--socket")
      opt.unix_path = next();
    else if (a == "--tcp-port")
    {
      const int p = std::atoi(next().c_str());
      opt.tcp = p > 0;
      opt.tcp_port = static_cast<std::uint16_t>(p);
    }
    else if (a == "--threads")
      opt.threads = static_cast<std::size_t>(std::atoi(next().c_str()));
    else if (a == "--queue")
      opt.queue = static_cast<std::size_t>(std::atoi(next().c_str()));
    else if (a == "--cache-mb")
      cache_mb = std::atol(next().c_str());
    else if (a == "--rss-limit-mb")
      rss_mb = std::atol(next().c_str());
    else
      return usage();
  }
  if (rss_mb < 0)
  {
    rss_mb = 2 * cache_mb + 1024;
  }
  if (root.empty() || opt.threads == 0 || opt.queue == 0 || cache_mb <= 0)
  {
    return usage();
  }

  // Signals are handled by sigwait() in this thread only.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGHUP);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  try
  {
    atmo::SnapshotLoader loader(root);
    atmo::SnapshotWatcher watcher(
      root, loader, [](const std::string& e) { log("snapshot: " + e); });
    watcher.start();
    atmo::GribCacheLimits limits;
    limits.max_bytes = static_cast<std::size_t>(cache_mb) << 20;
    limits.rss_limit = static_cast<std::size_t>(rss_mb) << 20;
#ifdef ATMO_HAVE_ECCODES
    atmo::GribCache cache(limits, atmo::grib_decoder());
#else
    atmo::GribCache cache(limits, {});  // GRIB-native data samples as null
#endif
    atmo::QueryApi api(root, loader, 4, &cache);
    atmo::fcgi::Server server(
      opt, [&api](const atmo::fcgi::Request& r) { return api.handle(r); });
    server.start();
    log(
      "serving " + root + " on " + opt.unix_path +
      (opt.tcp ? " and 127.0.0.1:" + std::to_string(server.tcp_port()) : ""));
    const timespec tick{5, 0};
    for (;;)
    {
      const int sig = sigtimedwait(&set, nullptr, &tick);
      if (sig < 0)
      {
        cache.maintain();  // forced cleanup if RSS is out of control
        continue;
      }
      if (sig == SIGHUP)
      {
        try
        {
          loader.refresh();
        }
        catch (const std::exception& e)
        {
          log(std::string("refresh: ") + e.what());
        }
        auto s = loader.current();
        const auto st = cache.stats();
        log(
          "SIGHUP: snapshot " + (s ? std::to_string(s->seq()) : "none") +
          ", grib cache " + std::to_string(st.entries) + " messages " +
          std::to_string(st.bytes >> 20) + " MB, " + std::to_string(st.hits) +
          " hits " + std::to_string(st.misses) + " decodes " +
          std::to_string(st.evictions) + " evictions, rss " +
          std::to_string(atmo::process_rss() >> 20) + " MB");
        continue;
      }
      log("stopping");
      server.stop();
      watcher.stop();
      log("stopped after " + std::to_string(server.served()) + " requests");
      return 0;
    }
  }
  catch (const std::exception& e)
  {
    log(std::string("error: ") + e.what());
    return 1;
  }
}
