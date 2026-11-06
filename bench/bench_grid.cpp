// Grid store timings on an HRRR-sized (1799 x 1059) Lambert grid.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

#include "atmo/gridstore.h"
#include "lambert.h"

using namespace atmo;
using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point a, Clock::time_point b)
{
  return std::chrono::duration<double>(b - a).count();
}

int main(int argc, char** argv)
{
  const std::filesystem::path dir =
    argc > 1 ? std::filesystem::path(argv[1])
             : std::filesystem::temp_directory_path() / "atmo_bench_gridstore";
  std::filesystem::remove_all(dir);

  atmo_test::Lambert L(38.5, 38.5, 38.5, -97.5, 6371229.0);
  const int nx = 1799, ny = 1059;
  const double dx = 3000.0;
  GridIdentity id;
  id.canon = {'h', 'r', 'r', 'r', '-', 'l', 'i', 'k', 'e'};
  id.ni = nx;
  id.nj = ny;
  GridData data;
  for (auto& p : atmo_test::lambert_grid(L, nx, ny, dx))
  {
    data.lat_deg.push_back(p.first);
    data.lon_deg.push_back(p.second);
  }

  GridStore store(dir);
  auto t0 = Clock::now();
  auto g =
    store.put(id, data);  // derives rotation, writes, fsyncs, reloads+verifies
  auto t1 = Clock::now();
  std::printf(
    "put (derive rotation + write + fsync + load/CRC): %.2f s, file %.1f MB\n",
    secs(t0, t1),
    static_cast<double>(std::filesystem::file_size(store.path_for(id.key()))) /
      1e6);

  g.reset();
  GridStore store2(dir);
  t0 = Clock::now();
  g = store2.get(id.key());
  t1 = Clock::now();
  std::printf(
    "get (mmap + verify all CRCs):                     %.3f s\n", secs(t0, t1));

  t0 = Clock::now();
  (void)g->locate(38.5, -97.5);
  t1 = Clock::now();
  std::printf(
    "first locate (builds the lookup tree):           %.2f s\n", secs(t0, t1));

  std::mt19937_64 rng(1);
  std::uniform_real_distribution<double> ux(-2.6e6, 2.6e6), uy(-1.55e6, 1.55e6);
  std::vector<std::pair<double, double>> qs;
  for (int i = 0; i < 1000000; ++i)
  {
    double lat, lon;
    L.inverse(ux(rng), uy(rng), lat, lon);
    qs.emplace_back(lat, lon);
  }
  t0 = Clock::now();
  std::size_t hit = 0;
  for (auto& q : qs)
    hit += g->locate(q.first, q.second).has_value();
  t1 = Clock::now();
  std::printf(
    "1M locate: %.2f s (%.2f us/query), %zu inside\n", secs(t0, t1),
    secs(t0, t1), hit);
  std::filesystem::remove_all(dir);
  return 0;
}
