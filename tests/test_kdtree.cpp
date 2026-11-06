#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "atmo/geo.h"
#include "atmo/kdtree.h"
#include "check.h"
#include "lambert.h"

using namespace atmo;

namespace
{

// ---- brute-force oracle -----------------------------------------------

template <class T>
std::vector<typename KdTree4<T>::Hit> oracle_all(
  const std::vector<typename KdTree4<T>::Point>& pts,
  const typename KdTree4<T>::Point& q)
{
  std::vector<typename KdTree4<T>::Hit> v;
  v.reserve(pts.size());
  for (std::size_t i = 0; i < pts.size(); ++i)
  {
    v.push_back({static_cast<std::uint32_t>(i), KdTree4<T>::dist2(q, pts[i])});
  }
  std::sort(v.begin(), v.end(), &KdTree4<T>::hit_less);
  return v;
}

template <class T>
bool same(
  const std::vector<typename KdTree4<T>::Hit>& a,
  const std::vector<typename KdTree4<T>::Hit>& b)
{
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
  {
    if (a[i].id != b[i].id || a[i].d2 != b[i].d2)
      return false;
  }
  return true;
}

// Compare nearest / knn / radius against the oracle for one query.
template <class T>
void check_query(
  const KdTree4<T>& tree,
  const std::vector<typename KdTree4<T>::Point>& pts,
  const typename KdTree4<T>::Point& q,
  const std::vector<T>& radii,
  const std::vector<std::size_t>& ks)
{
  const auto all = oracle_all<T>(pts, q);

  auto nn = tree.nearest(q);
  if (all.empty())
  {
    CHECK(!nn.has_value());
  }
  else
  {
    CHECK(nn.has_value());
    if (nn)
    {
      CHECK(nn->id == all[0].id);
      CHECK(nn->d2 == all[0].d2);
    }
  }
  for (std::size_t k : ks)
  {
    std::vector<typename KdTree4<T>::Hit> want(
      all.begin(), all.begin() + std::min(k, all.size()));
    CHECK(same<T>(tree.knn(q, k), want));
  }
  for (T r : radii)
  {
    std::vector<typename KdTree4<T>::Hit> want;
    for (const auto& h : all)
    {
      if (h.d2 <= r * r)
        want.push_back(h);
    }
    CHECK(same<T>(tree.radius(q, r), want));
  }
}

// ---- random data ---------------------------------------------------------

template <class T>
std::vector<typename KdTree4<T>::Point> random_obs(
  std::mt19937_64& rng,
  int n,
  const Embedding& e,
  double t_base,
  double t_span)
{
  std::uniform_real_distribution<double> u(0, 1);
  std::vector<typename KdTree4<T>::Point> v;
  for (int i = 0; i < n; ++i)
  {
    const double lat =
      std::asin(2 * u(rng) - 1) * 180 / kPi;  // uniform on sphere
    const double lon = -180 + 360 * u(rng);
    const double h = 15000 * u(rng);
    const double t = t_base + t_span * u(rng);
    v.push_back(embed<T>(lat, lon, h, t, e));
  }
  return v;
}

template <class T>
void run_random_vs_oracle()
{
  std::mt19937_64 rng(12345);
  Embedding e;
  e.t0_s = 1.7e9;
  e.km_per_second = 0.01;  // 1 hour == 36 km
  e.vertical_scale = 10;   // 1 km up costs like 10 km sideways
  auto pts = random_obs<T>(rng, 5000, e, e.t0_s, 3 * 86400.0);
  KdTree4<T> tree(pts);
  CHECK(tree.size() == pts.size());
  auto qs = random_obs<T>(rng, 300, e, e.t0_s, 3 * 86400.0);
  const std::vector<T> radii = {T(0), T(50), T(500), T(3000), T(20000)};
  const std::vector<std::size_t> ks = {1, 3, 17, 100};
  for (const auto& q : qs)
    check_query<T>(tree, pts, q, radii, ks);
}

}  // namespace

// ---- tests ---------------------------------------------------------------

TEST(empty_tree)
{
  KdTree4<> t;
  const KdTree4<>::Point q = {0, 0, 0, 0};
  CHECK(t.empty());
  CHECK(!t.nearest(q).has_value());
  CHECK(t.knn(q, 5).empty());
  CHECK(t.radius(q, 1e9).empty());
  KdTree4<> t2(std::vector<KdTree4<>::Point>{});
  CHECK(!t2.nearest(q).has_value());
}

TEST(single_point)
{
  KdTree4<> t(std::vector<KdTree4<>::Point>{{1, 2, 3, 4}});
  const KdTree4<>::Point q = {1, 2, 3, 5};
  auto nn = t.nearest(q);
  CHECK(nn.has_value());
  if (nn)
  {
    CHECK(nn->id == 0);
    CHECK(nn->d2 == 1.0);
  }
  CHECK(t.knn(q, 5).size() == 1);
  CHECK(t.radius(q, 0.5).empty());
  CHECK(t.radius(q, 1.0).size() == 1);  // boundary is inclusive
}

TEST(degenerate_arguments)
{
  KdTree4<> t(std::vector<KdTree4<>::Point>{{0, 0, 0, 0}, {1, 0, 0, 0}});
  const KdTree4<>::Point q = {0, 0, 0, 0};
  CHECK(t.knn(q, 0).empty());
  CHECK(t.knn(q, 99).size() == 2);
  CHECK(t.radius(q, -1.0).empty());
  CHECK(t.radius(q, std::nan("")).empty());
  CHECK(t.radius(q, 0.0).size() == 1);
}

TEST(rejects_non_finite)
{
  bool threw = false;
  try
  {
    KdTree4<> t(std::vector<KdTree4<>::Point>{{0, 0, std::nan(""), 0}});
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  CHECK(threw);
  threw = false;
  try
  {
    KdTree4<> t(std::vector<KdTree4<>::Point>{{0, HUGE_VAL, 0, 0}});
  }
  catch (const std::invalid_argument&)
  {
    threw = true;
  }
  CHECK(threw);
}

// Every size around the leaf/split boundaries, continuous and tie-heavy data.
template <class T>
static void sizes_sweep()
{
  std::mt19937_64 rng(777);
  std::uniform_real_distribution<double> uc(-1, 1);
  std::uniform_int_distribution<int> ul(0, 3);
  std::vector<int> sizes;
  for (int n = 0; n <= 70; ++n)
    sizes.push_back(n);
  for (int n : {127, 128, 129, 255, 256, 257, 1000})
    sizes.push_back(n);
  for (int n : sizes)
  {
    for (int lattice = 0; lattice < 2; ++lattice)
    {
      std::vector<typename KdTree4<T>::Point> pts;
      for (int i = 0; i < n; ++i)
      {
        typename KdTree4<T>::Point p;
        for (int a = 0; a < 4; ++a)
        {
          p[a] = lattice ? static_cast<T>(ul(rng)) : static_cast<T>(uc(rng));
        }
        pts.push_back(p);
      }
      KdTree4<T> tree(pts);
      for (int qi = 0; qi < 12; ++qi)
      {
        typename KdTree4<T>::Point q;
        for (int a = 0; a < 4; ++a)
        {
          q[a] = lattice ? static_cast<T>(ul(rng)) : static_cast<T>(uc(rng));
        }
        check_query<T>(
          tree, pts, q, {T(0), T(0.5), T(1), T(1.5), T(10)}, {1, 2, 5, 9, 64});
      }
    }
  }
}
TEST(sizes_sweep_double)
{
  sizes_sweep<double>();
}
TEST(sizes_sweep_float)
{
  sizes_sweep<float>();
}

TEST(random_vs_oracle_double)
{
  run_random_vs_oracle<double>();
}
TEST(random_vs_oracle_float)
{
  run_random_vs_oracle<float>();
}

TEST(query_exactly_on_a_point)
{
  std::mt19937_64 rng(5);
  Embedding e;
  e.km_per_second = 0.02;
  auto pts = random_obs<double>(rng, 2000, e, 0, 86400);
  KdTree4<> tree(pts);
  for (std::size_t i = 0; i < pts.size(); i += 7)
  {
    auto nn = tree.nearest(pts[i]);
    CHECK(nn.has_value());
    if (nn)
    {
      CHECK(nn->id == i);
      CHECK(nn->d2 == 0.0);
    }
  }
}

TEST(duplicates_break_ties_by_id)
{
  std::vector<KdTree4<>::Point> pts;
  for (int i = 0; i < 100; ++i)
    pts.push_back({5, 5, 5, 5});  // ids 0..99
  for (int i = 0; i < 50; ++i)
    pts.push_back({double(i), 0, 0, 0});
  KdTree4<> tree(pts);
  const KdTree4<>::Point q = {5, 5, 5, 5};
  auto k = tree.knn(q, 10);
  CHECK(k.size() == 10);
  for (std::size_t i = 0; i < k.size(); ++i)
  {
    CHECK(k[i].id == i);  // lowest ids first
    CHECK(k[i].d2 == 0.0);
  }
  CHECK(tree.radius(q, 0.0).size() == 100);
  check_query<double>(tree, pts, q, {0.0, 3.0, 100.0}, {1, 10, 100, 101, 200});
}

TEST(poles)
{
  Embedding e;
  e.km_per_second = 0.01;
  // All longitudes at the pole must be the same place.
  auto a = embed(90.0, 0.0, 0, 0, e);
  auto b = embed(90.0, 123.4, 0, 0, e);
  auto s1 = embed(-90.0, 0.0, 0, 0, e);
  auto s2 = embed(-90.0, -77.0, 0, 0, e);
  CHECK(KdTree4<>::dist2(a, b) < 1e-18);
  CHECK(KdTree4<>::dist2(s1, s2) < 1e-18);
  CHECK(KdTree4<>::dist2(a, s1) > 1e8);

  for (double sign : {1.0, -1.0})
  {
    std::vector<KdTree4<>::Point> pts;
    for (double lat : {89.999, 89.9, 89.0, 85.0})
    {
      for (int lon = -180; lon < 180; lon += 10)
      {
        pts.push_back(embed(sign * lat, lon, 0, 0, e));
      }
    }
    pts.push_back(embed(sign * 90.0, 0.0, 0, 0, e));  // the pole itself
    const std::uint32_t pole_id = static_cast<std::uint32_t>(pts.size() - 1);
    KdTree4<> tree(pts);
    for (double lon : {-180.0, -90.0, 0.0, 45.0, 77.0, 180.0})
    {
      auto q = embed(sign * 90.0, lon, 0, 0, e);
      auto nn = tree.nearest(q);
      CHECK(nn.has_value());
      if (nn)
        CHECK(nn->id == pole_id);
      check_query<double>(tree, pts, q, {0.0, 1.0, 20.0, 500.0}, {1, 5, 40});
    }
    // Slightly off the pole: oracle agreement.
    for (double lon : {10.0, 100.0, -170.0})
    {
      auto q = embed(sign * 89.95, lon, 0, 0, e);
      check_query<double>(tree, pts, q, {1.0, 20.0, 500.0}, {1, 5, 40});
    }
  }
}

TEST(dateline)
{
  Embedding e;  // lambda = 0: pure spatial, ranks must match great-circle
  // +-180 is the same meridian.
  CHECK(
    KdTree4<>::dist2(
      embed(40.0, 180.0, 0, 0, e), embed(40.0, -180.0, 0, 0, e)) < 1e-18);

  std::mt19937_64 rng(99);
  std::uniform_real_distribution<double> u(0, 1);
  std::vector<KdTree4<>::Point> pts;
  std::vector<std::pair<double, double>> ll;
  for (int i = 0; i < 3000; ++i)
  {
    double lon = 175 + 10 * u(rng);  // 175..185
    if (lon > 180)
      lon -= 360;
    double lat = (i % 3 == 0) ? 60 + 10 * u(rng) : -10 + 20 * u(rng);
    ll.emplace_back(lat, lon);
    pts.push_back(embed(lat, lon, 0, 0, e));
  }
  KdTree4<> tree(pts);
  for (double qlon : {180.0, -180.0, 179.99, -179.99, 177.0, -177.0})
  {
    for (double qlat : {0.0, 65.0, -9.0})
    {
      auto q = embed(qlat, qlon, 0, 0, e);
      auto nn = tree.nearest(q);
      CHECK(nn.has_value());
      if (!nn)
        continue;
      // Haversine brute force.
      std::size_t best = 0;
      double bd = 1e30;
      for (std::size_t i = 0; i < ll.size(); ++i)
      {
        double d = great_circle_km(qlat, qlon, ll[i].first, ll[i].second);
        if (d < bd)
        {
          bd = d;
          best = i;
        }
      }
      const double got =
        great_circle_km(qlat, qlon, ll[nn->id].first, ll[nn->id].second);
      CHECK(nn->id == best || std::fabs(got - bd) < 1e-6);
      check_query<double>(tree, pts, q, {10.0, 100.0}, {1, 8, 50});
    }
  }
}

TEST(time_axis_pure_spatial_when_lambda_zero)
{
  Embedding e;  // km_per_second = 0
  std::vector<KdTree4<>::Point> pts = {
    embed(40, -105, 1600, 0.0, e), embed(40, -105, 1600, 1e6, e),
    embed(40, -105, 1600, 2e9, e)};
  KdTree4<> tree(pts);
  auto r = tree.radius(embed(40, -105, 1600, 5.0, e), 0.0);
  CHECK(r.size() == 3);
  for (auto& h : r)
    CHECK(h.d2 == 0.0);
}

TEST(time_axis_same_place_different_time)
{
  Embedding e;
  e.km_per_second = 0.01;  // 1000 s == 10 km
  std::vector<KdTree4<>::Point> pts = {
    embed(40, -105, 0, 0.0, e), embed(40, -105, 0, 1000.0, e)};
  KdTree4<> tree(pts);
  auto q = embed(40, -105, 0, 0.0, e);
  CHECK(tree.radius(q, 9.999).size() == 1);
  CHECK(tree.radius(q, 10.001).size() == 2);
  auto k = tree.knn(q, 2);
  CHECK(k.size() == 2);
  if (k.size() == 2)
  {
    CHECK(k[0].id == 0);
    CHECK_NEAR(std::sqrt(k[1].d2), 10.0, 1e-9);
  }
  // A query 400 s later is nearer to the later obs than the earlier one... in
  // time only: 400 s -> 4 km, 600 s -> 6 km.
  auto q2 = embed(40, -105, 0, 400.0, e);
  auto nn = tree.nearest(q2);
  CHECK(nn.has_value());
  if (nn)
    CHECK(nn->id == 0);
}

TEST(time_precision_with_float_and_epoch_times)
{
  // Naive float epoch seconds near 1.7e9 have a spacing of 128 s.
  CHECK(static_cast<float>(1.7e9 + 60.0) == static_cast<float>(1.7e9));
  // With a per-tree base the same 60 s steps survive in a float tree.
  Embedding e;
  e.t0_s = 1.7e9;
  e.km_per_second = 0.01;
  std::vector<KdTree4<float>::Point> pts;
  for (int i = 0; i < 10; ++i)
    pts.push_back(embed<float>(0, 0, 0, e.t0_s + 60.0 * i, e));
  for (int i = 0; i < 10; ++i)
  {
    CHECK_NEAR(pts[i][3], 0.6 * i, 1e-5);
  }
  KdTree4<float> tree(pts);
  auto q =
    embed<float>(0, 0, 0, e.t0_s + 60.0 * 4 + 5.0, e);  // 5 s past obs #4
  auto nn = tree.nearest(q);
  CHECK(nn.has_value());
  if (nn)
    CHECK(nn->id == 4);
}

TEST(lambert_projection_sanity)
{
  atmo_test::Lambert L(38.5, 38.5, 38.5, -97.5, 6371229.0);
  double lat, lon;
  L.inverse(0, 0, lat, lon);
  CHECK_NEAR(lat, 38.5, 1e-9);
  CHECK_NEAR(lon, -97.5, 1e-9);
  for (double x : {-2.0e6, -5.0e5, 0.0, 1.3e6})
  {
    for (double y : {-1.4e6, 0.0, 9.0e5})
    {
      double x2, y2;
      L.inverse(x, y, lat, lon);
      L.forward(lat, lon, x2, y2);
      CHECK_NEAR(x2, x, 1e-3);
      CHECK_NEAR(y2, y, 1e-3);
    }
  }
}

TEST(lambert_grid_every_point_finds_itself)
{
  atmo_test::Lambert L(38.5, 38.5, 38.5, -97.5, 6371229.0);
  const int nx = 300, ny = 200;
  const double dx = 3000.0;
  auto ll = atmo_test::lambert_grid(L, nx, ny, dx);
  Embedding e;  // spatial only
  std::vector<KdTree4<>::Point> pts;
  for (auto& p : ll)
    pts.push_back(embed(p.first, p.second, 0, 0, e));
  KdTree4<> tree(pts);

  // Neighbour spacing should be ~3 km near the standard parallel.
  const std::size_t c = static_cast<std::size_t>(ny / 2) * nx + nx / 2;
  CHECK_NEAR(std::sqrt(KdTree4<>::dist2(pts[c], pts[c + 1])), 3.0, 0.03);
  CHECK_NEAR(std::sqrt(KdTree4<>::dist2(pts[c], pts[c + nx])), 3.0, 0.03);

  std::size_t wrong = 0;
  for (std::size_t i = 0; i < pts.size(); ++i)
  {
    auto nn = tree.nearest(pts[i]);
    if (!nn || nn->id != i || nn->d2 != 0.0)
      ++wrong;
  }
  CHECK(wrong == 0);

  // Off-grid queries (random offsets within a cell) vs the oracle.
  std::mt19937_64 rng(2024);
  std::uniform_real_distribution<double> uo(-1.5, 1.5);
  std::uniform_int_distribution<int> ui(5, nx - 6), uj(5, ny - 6);
  for (int t = 0; t < 40; ++t)
  {
    double x = -(nx - 1) * 0.5 * dx + ui(rng) * dx + uo(rng) * dx;
    double y = -(ny - 1) * 0.5 * dx + uj(rng) * dx + uo(rng) * dx;
    double lat, lon;
    L.inverse(x, y, lat, lon);
    check_query<double>(
      tree, pts, embed(lat, lon, 0, 0, e), {2.0, 5.0}, {1, 4, 12});
  }
}

int main()
{
  return tst::run_all();
}
