// atmo_productd: derived products daemon (06-derived-products.md; T21, T22).
//
//   atmo_productd --config /opt/atmotensor/etc/productd.json [--once]
//
// Config (JSON; holds no secrets, the database settings stay in the
// catalog file, which must be readable only by the service user):
//   {"catalog": "/opt/atmotensor/etc/catalog.json",
//    "root": "/opt/atmotensor/data", "tmp": "/opt/atmotensor/tmp",
//    "devices": ["Tesla M40", "Intel", "Portable Computing Language"],
//    "poll_ms": 1000, "job_timeout_s": 600, "max_attempts": 2,
//    "bits": 16, "cache_mb": 512}
// devices: OpenCL devices to use, in order of preference, matched as
// case-insensitive substrings of "<platform> / <device>"; a job goes to the
// first one that can hold it (Q2b).  Empty or absent: every device found.
// --once: one pass, print what it did, exit (for checks and cron).
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "atmo/productd.h"
#ifdef ATMO_HAVE_OPENCL
#include "atmo/opencl.h"
#endif

namespace
{

std::atomic<bool> g_stop{false};

void on_signal(int)
{
  g_stop.store(true);
}

int usage()
{
  std::fprintf(stderr, "usage: atmo_productd --config FILE [--once]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv)
{
  std::string config;
  bool once = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc)
    {
      config = argv[++i];
    }
    else if (a == "--once")
    {
      once = true;
    }
    else
    {
      return usage();
    }
  }
  if (config.empty())
  {
    return usage();
  }
  try
  {
    std::ifstream in(config);
    if (!in)
    {
      throw std::runtime_error("cannot read " + config);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const auto j = nlohmann::json::parse(ss.str());
    atmo::ProductDaemonOptions opt;
    opt.catalog =
      atmo::CatalogConfig::from_file(j.at("catalog").get<std::string>());
    opt.root = j.at("root").get<std::string>();
    opt.tmp = j.value("tmp", (opt.root / "tmp").string());
    opt.job_timeout_s = j.value("job_timeout_s", 600);
    opt.max_attempts = j.value("max_attempts", 2);
    opt.bits = j.value("bits", 16);
    opt.cache_mb = j.value("cache_mb", std::size_t{512});
    char host[256] = {};
    ::gethostname(host, sizeof host - 1);
    opt.worker = std::string(host) + ":" + std::to_string(::getpid());
    const auto poll = std::chrono::milliseconds(j.value("poll_ms", 1000));

    std::unique_ptr<atmo::KernelExecutor> kernels;
#ifdef ATMO_HAVE_OPENCL
    std::vector<std::string> devices;
    if (j.contains("devices"))
    {
      devices = j["devices"].get<std::vector<std::string>>();
    }
    kernels =
      atmo::make_opencl_executor(devices, j.value("job_timeout_s", 600));
    for (const auto& d : static_cast<atmo::OpenClExecutor&>(*kernels).devices())
    {
      std::fprintf(stderr, "atmo_productd: OpenCL device %s\n", d.c_str());
    }
#else
    std::fprintf(
      stderr,
      "atmo_productd: built without OpenCL; kernel products will fail\n");
#endif
    const std::string worker = opt.worker;
    atmo::ProductDaemon d(std::move(opt), std::move(kernels));
    if (once)
    {
      const auto st = d.step(1000000);
      for (const auto& line : st.log)
      {
        std::printf("%s\n", line.c_str());
      }
      std::printf(
        "generation %llu: %zu arrival(s), %zu job(s) created, %zu requeued, "
        "%zu superseded, %zu rebuild(s), %zu kernel build(s), %zu stale, %zu "
        "ran, %zu failed\n",
        static_cast<unsigned long long>(st.generation), st.arrivals,
        st.jobs_created, st.jobs_requeued, st.jobs_superseded, st.rebuilds,
        st.kernels_built, st.stale, st.ran, st.failed);
      return st.failed ? 1 : 0;
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    std::fprintf(stderr, "atmo_productd: running as %s\n", worker.c_str());
    d.run(g_stop, poll);
    return 0;
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "atmo_productd: %s\n", e.what());
    return 1;
  }
}
