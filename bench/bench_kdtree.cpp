// Build / query timings for KdTree4 at production-ish sizes.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "atmo/geo.h"
#include "atmo/kdtree.h"
#include "lambert.h"

using namespace atmo;
using Clock = std::chrono::steady_clock;

static double secs(Clock::time_point a, Clock::time_point b)
{
  return std::chrono::duration<double>(b - a).count();
}

template <class T>
static void run(const char* label, std::size_t n_pts)
{
  std::mt19937_64 rng(1);
  std::uniform_real_distribution<double> u(0, 1);
  Embedding e;
  e.t0_s = 1.7e9;
  e.km_per_second = 0.01;
  e.vertical_scale = 10;
  auto make = [&](std::size_t n)
  {
    std::vector<typename KdTree4<T>::Point> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
      v.push_back(
        embed<T>(
          std::asin(2 * u(rng) - 1) * 180 / kPi, -180 + 360 * u(rng),
          15000 * u(rng), e.t0_s + 3 * 86400.0 * u(rng), e));
    }
    return v;
  };
  auto pts = make(n_pts);
  auto t0 = Clock::now();
  KdTree4<T> tree(pts);
  auto t1 = Clock::now();
  std::printf("%s  build %zu pts: %.2f s\n", label, n_pts, secs(t0, t1));

  auto qs = make(1000000);
  t0 = Clock::now();
  double acc = 0;
  for (auto& q : qs)
    acc += tree.nearest(q)->d2;
  t1 = Clock::now();
  std::printf(
    "%s  1M nearest:   %.2f s  (%.2f us/query)  [chk %.3g]\n", label,
    secs(t0, t1), secs(t0, t1), acc);

  t0 = Clock::now();
  std::size_t cnt = 0;
  for (std::size_t i = 0; i < 200000; ++i)
    cnt += tree.knn(qs[i], 8).size();
  t1 = Clock::now();
  std::printf(
    "%s  200k knn(8):  %.2f s  (%.2f us/query)  [chk %zu]\n", label,
    secs(t0, t1), secs(t0, t1) * 5, cnt);

  t0 = Clock::now();
  cnt = 0;
  for (std::size_t i = 0; i < 100000; ++i)
    cnt += tree.radius(qs[i], 100).size();
  t1 = Clock::now();
  std::printf(
    "%s  100k radius(100 km): %.2f s  (%.2f us/query, avg %.1f hits)\n", label,
    secs(t0, t1), secs(t0, t1) * 10, static_cast<double>(cnt) / 100000.0);
}

int main()
{
  run<double>("double", 2000000);
  run<float>("float ", 2000000);

  // Full HRRR-sized Lambert grid (1799 x 1059): every point must find itself.
  atmo_test::Lambert L(38.5, 38.5, 38.5, -97.5, 6371229.0);
  auto ll = atmo_test::lambert_grid(L, 1799, 1059, 3000.0);
  Embedding e;
  std::vector<KdTree4<>::Point> pts;
  pts.reserve(ll.size());
  for (auto& p : ll)
    pts.push_back(embed(p.first, p.second, 0, 0, e));
  auto t0 = Clock::now();
  KdTree4<> tree(pts);
  auto t1 = Clock::now();
  std::size_t wrong = 0;
  auto t2 = Clock::now();
  for (std::size_t i = 0; i < pts.size(); ++i)
  {
    auto nn = tree.nearest(pts[i]);
    if (!nn || nn->id != i)
      ++wrong;
  }
  auto t3 = Clock::now();
  std::printf(
    "lambert 1799x1059 (%zu pts): build %.2f s, self-nearest all: %.2f s, "
    "wrong=%zu\n",
    pts.size(), secs(t0, t1), secs(t2, t3), wrong);
  return wrong == 0 ? 0 : 1;
}
