// atmo::KdTree4 -- static, flat-array 4D k-d tree.
//
// Points are 4 coordinates in a single Euclidean metric.  For AtmoTensor the
// coordinates come from atmo::embed() (geo.h): x,y,z in km on a sphere of
// radius R + scaled height, and w = lambda * (t - t0).  The per-source time
// weighting (lambda) is therefore baked into the coordinates, and one tree is
// built per source class.
//
// Layout (GPU/OpenCL friendly: no pointers, only integer indices):
//   * Implicit balanced tree over [0, n).  A range [lo, hi) with more than
//     kLeafSize entries has its splitting point at mid = lo + (hi-lo)/2;
//     children are [lo, mid) and [mid+1, hi).  Ranges of kLeafSize or fewer
//     are leaves and are scanned linearly.
//   * pts_[i]  : coordinates of the entry at slot i
//   * ids_[i]  : caller's original index of that entry
//   * axis_[m] : split axis of the node at slot m (only meaningful for
//                non-leaf split slots)
//   KdTreeView4 searches those three arrays wherever they live (vectors
//   owned by a KdTree4, or an mmap'd file); KdTree4 owns and builds them.
//
// Determinism: every result is ordered by (squared distance, id), and build
// breaks coordinate ties by id, so output never depends on thread/hash/
// allocator behaviour.  Queries are exactly equal to a brute-force scan that
// uses KdTree4::dist2.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace atmo
{

// Non-owning, read-only view of a built tree's flat arrays (e.g. arrays that
// live in an mmap'd file).  All searching happens here; KdTree4 delegates to
// it, so a view over the same arrays answers exactly like the tree.
template <typename T = double>
class KdTreeView4
{
 public:
  using Scalar = T;
  using Point = std::array<T, 4>;
  using Id = std::uint32_t;

  struct Hit
  {
    Id id;
    T d2;  // squared distance
  };

  static constexpr std::size_t kLeafSize = 8;

  // The one and only distance function.  Tests' oracles must use this too so
  // that floating-point results are bit-identical.
  static T dist2(const Point& a, const Point& b)
  {
    const T d0 = a[0] - b[0];
    const T d1 = a[1] - b[1];
    const T d2 = a[2] - b[2];
    const T d3 = a[3] - b[3];
    return ((d0 * d0 + d1 * d1) + d2 * d2) + d3 * d3;
  }

  // Total order used for every result list.
  static bool hit_less(const Hit& a, const Hit& b)
  {
    return a.d2 < b.d2 || (a.d2 == b.d2 && a.id < b.id);
  }

  KdTreeView4() = default;
  KdTreeView4(
    const Point* pts,
    const Id* ids,
    const std::uint8_t* axes,
    std::size_t n)
      : pts_(pts), ids_(ids), axis_(axes), n_(n)
  {
  }

  std::size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  const Point* points() const { return pts_; }
  const Id* ids() const { return ids_; }
  const std::uint8_t* axes() const { return axis_; }

  // Cheap structural check for arrays that came from outside (a file):
  // ids are a permutation of [0, n), split axes are < 4 and coordinates are
  // finite, so no query can index out of range.  It cannot prove the arrays
  // are a correctly *ordered* tree; checksums cover that.
  bool well_formed() const
  {
    if (n_ > 0 && (!pts_ || !ids_ || !axis_))
      return false;
    std::vector<bool> seen(n_, false);
    for (std::size_t i = 0; i < n_; ++i)
    {
      const Id id = ids_[i];
      if (id >= n_ || seen[id])
        return false;
      seen[id] = true;
      if (axis_[i] > 3)
        return false;
      for (T c : pts_[i])
      {
        if (!std::isfinite(c))
          return false;
      }
    }
    return true;
  }

  // Closest point (ties -> lowest id).  nullopt if the tree is empty.
  std::optional<Hit> nearest(const Point& q) const
  {
    NearestCollector c;
    search(q, c);
    if (!c.have)
      return std::nullopt;
    return c.best;
  }

  // The k closest points, ascending by (d2, id).  Fewer if size() < k.
  std::vector<Hit> knn(const Point& q, std::size_t k) const
  {
    if (k == 0 || empty())
      return {};
    KnnCollector c(std::min(k, n_));
    search(q, c);
    std::sort(c.heap.begin(), c.heap.end(), &hit_less);
    return std::move(c.heap);
  }

  // All points with d2 <= r*r, ascending by (d2, id).  Empty for r < 0 / NaN.
  std::vector<Hit> radius(const Point& q, T r) const
  {
    if (!(r >= T(0)) || empty())
      return {};
    RadiusCollector c(r * r);
    search(q, c);
    std::sort(c.hits.begin(), c.hits.end(), &hit_less);
    return std::move(c.hits);
  }

 private:
  struct NearestCollector
  {
    Hit best{0, T(0)};
    bool have = false;
    T bound() const
    {
      return have ? best.d2 : std::numeric_limits<T>::infinity();
    }
    void offer(Id id, T d2)
    {
      const Hit h{id, d2};
      if (!have || hit_less(h, best))
      {
        best = h;
        have = true;
      }
    }
  };

  struct KnnCollector
  {
    std::size_t k;
    std::vector<Hit> heap;  // max-heap under hit_less
    explicit KnnCollector(std::size_t k_) : k(k_) { heap.reserve(k_); }
    T bound() const
    {
      return heap.size() < k ? std::numeric_limits<T>::infinity()
                             : heap.front().d2;
    }
    void offer(Id id, T d2)
    {
      const Hit h{id, d2};
      if (heap.size() < k)
      {
        heap.push_back(h);
        std::push_heap(heap.begin(), heap.end(), &hit_less);
      }
      else if (hit_less(h, heap.front()))
      {
        std::pop_heap(heap.begin(), heap.end(), &hit_less);
        heap.back() = h;
        std::push_heap(heap.begin(), heap.end(), &hit_less);
      }
    }
  };

  struct RadiusCollector
  {
    T r2;
    std::vector<Hit> hits;
    explicit RadiusCollector(T r2_) : r2(r2_) {}
    T bound() const { return r2; }
    void offer(Id id, T d2)
    {
      if (d2 <= r2)
        hits.push_back(Hit{id, d2});
    }
  };

  template <class C>
  void search(const Point& q, C& c) const
  {
    if (n_ != 0)
      rec(0, n_, q, c);
  }

  // Pruning uses "far side only if d*d <= bound" (not <) so equal-distance
  // candidates with a smaller id on the far side are still found.  d*d is
  // the same floating-point term dist2 would add, and FP addition of
  // non-negative terms is monotone, so pruning never discards a point that
  // the brute-force scan would have ranked inside the bound.
  template <class C>
  void rec(std::size_t lo, std::size_t hi, const Point& q, C& c) const
  {
    if (hi - lo <= kLeafSize)
    {
      for (std::size_t i = lo; i < hi; ++i)
      {
        c.offer(ids_[i], dist2(q, pts_[i]));
      }
      return;
    }
    const std::size_t mid = lo + (hi - lo) / 2;
    c.offer(ids_[mid], dist2(q, pts_[mid]));
    const int a = axis_[mid];
    const T d = q[a] - pts_[mid][a];
    if (d < T(0))
    {
      rec(lo, mid, q, c);
      if (d * d <= c.bound())
        rec(mid + 1, hi, q, c);
    }
    else
    {
      rec(mid + 1, hi, q, c);
      if (d * d <= c.bound())
        rec(lo, mid, q, c);
    }
  }

  const Point* pts_ = nullptr;
  const Id* ids_ = nullptr;
  const std::uint8_t* axis_ = nullptr;
  std::size_t n_ = 0;
};

template <typename T = double>
class KdTree4
{
 public:
  using View = KdTreeView4<T>;
  using Scalar = T;
  using Point = typename View::Point;
  using Id = typename View::Id;
  using Hit = typename View::Hit;

  static constexpr std::size_t kLeafSize = View::kLeafSize;

  static T dist2(const Point& a, const Point& b) { return View::dist2(a, b); }
  static bool hit_less(const Hit& a, const Hit& b)
  {
    return View::hit_less(a, b);
  }

  KdTree4() = default;
  explicit KdTree4(const std::vector<Point>& pts) { build(pts); }

  // Ids are indices into `pts`.  Throws std::invalid_argument on non-finite
  // coordinates and std::length_error if there are more than 2^32-1 points.
  void build(const std::vector<Point>& pts)
  {
    const std::size_t n = pts.size();
    if (n > std::numeric_limits<Id>::max())
    {
      throw std::length_error("KdTree4: too many points");
    }
    for (const Point& p : pts)
    {
      for (T c : p)
      {
        if (!std::isfinite(c))
        {
          throw std::invalid_argument("KdTree4: non-finite coordinate");
        }
      }
    }
    std::vector<Item> items(n);
    for (std::size_t i = 0; i < n; ++i)
    {
      items[i].p = pts[i];
      items[i].id = static_cast<Id>(i);
    }
    axis_.assign(n, 0);
    build_rec(items, 0, n);
    pts_.resize(n);
    ids_.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
      pts_[i] = items[i].p;
      ids_[i] = items[i].id;
    }
  }

  std::size_t size() const { return ids_.size(); }
  bool empty() const { return ids_.empty(); }

  // Valid until the tree is rebuilt or destroyed.
  View view() const
  {
    return View(pts_.data(), ids_.data(), axis_.data(), ids_.size());
  }

  std::optional<Hit> nearest(const Point& q) const { return view().nearest(q); }
  std::vector<Hit> knn(const Point& q, std::size_t k) const
  {
    return view().knn(q, k);
  }
  std::vector<Hit> radius(const Point& q, T r) const
  {
    return view().radius(q, r);
  }

  // Flat arrays, e.g. for uploading to an OpenCL buffer or writing to a file.
  const std::vector<Point>& points() const { return pts_; }
  const std::vector<Id>& ids() const { return ids_; }
  const std::vector<std::uint8_t>& axes() const { return axis_; }

 private:
  struct Item
  {
    Point p;
    Id id;
  };

  void build_rec(std::vector<Item>& items, std::size_t lo, std::size_t hi)
  {
    if (hi - lo <= kLeafSize)
      return;
    Point mn = items[lo].p;
    Point mx = mn;
    for (std::size_t i = lo + 1; i < hi; ++i)
    {
      for (int a = 0; a < 4; ++a)
      {
        mn[a] = std::min(mn[a], items[i].p[a]);
        mx[a] = std::max(mx[a], items[i].p[a]);
      }
    }
    int ax = 0;
    T widest = mx[0] - mn[0];
    for (int a = 1; a < 4; ++a)
    {
      if (mx[a] - mn[a] > widest)
      {
        widest = mx[a] - mn[a];
        ax = a;
      }
    }
    const std::size_t mid = lo + (hi - lo) / 2;
    std::nth_element(
      items.begin() + static_cast<std::ptrdiff_t>(lo),
      items.begin() + static_cast<std::ptrdiff_t>(mid),
      items.begin() + static_cast<std::ptrdiff_t>(hi),
      [ax](const Item& a, const Item& b)
      { return a.p[ax] < b.p[ax] || (a.p[ax] == b.p[ax] && a.id < b.id); });
    axis_[mid] = static_cast<std::uint8_t>(ax);
    build_rec(items, lo, mid);
    build_rec(items, mid + 1, hi);
  }

  std::vector<Point> pts_;
  std::vector<Id> ids_;
  std::vector<std::uint8_t> axis_;
};

}  // namespace atmo
