// atmo_queryd: the query daemon (T10).  A FastCGI responder behind nginx
// serving /healthz, /v1/meta and /v1/point over the published snapshots
// (04-query-api.md).  Read-only on the store.
//
//   atmo_queryd --root /opt/atmotensor/data [--socket /run/atmo/query.sock]
//               [--tcp-port N] [--threads N] [--queue N]
//
// SIGTERM/SIGINT: stop accepting, finish in-flight requests, exit.
// SIGHUP: re-read the manifest now (there is no config file yet).
#include <pthread.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>

#include "atmo/fcgi.h"
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
    "                   [--threads N] [--queue N]\n"
    "  --socket defaults to /run/atmo/query.sock; --tcp-port listens on\n"
    "  127.0.0.1 as well (0 = no TCP).\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv)
{
  std::string root;
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
    else
      return usage();
  }
  if (root.empty() || opt.threads == 0 || opt.queue == 0)
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
    atmo::QueryApi api(root, loader);
    atmo::fcgi::Server server(
      opt, [&api](const atmo::fcgi::Request& r) { return api.handle(r); });
    server.start();
    log(
      "serving " + root + " on " + opt.unix_path +
      (opt.tcp ? " and 127.0.0.1:" + std::to_string(server.tcp_port()) : ""));
    for (;;)
    {
      int sig = 0;
      sigwait(&set, &sig);
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
        log("SIGHUP: snapshot " + (s ? std::to_string(s->seq()) : "none"));
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
