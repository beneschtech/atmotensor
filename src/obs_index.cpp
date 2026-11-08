#include "atmo/obs_index.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace atmo
{

namespace
{

using Tree = KdTreeView4<float>;
using Version = std::pair<std::uint64_t, std::uint64_t>;  // (key, seq)

struct Entry
{
  std::shared_ptr<const ObsSegment> seg;
  Embedding emb;
  std::unique_ptr<KdTree4<float>> own;  // set if rebuilt in RAM (Q5b)
  Tree tree;
};

struct Bucket
{
  std::int64_t start = 0;
  std::int64_t len = 0;
  std::vector<Version> versions;  // sorted, unique

  bool contains(std::int64_t t) const { return t >= start && t - start < len; }
};

// True if `seg` was built with `cls`'s distance metric.  bucket_seconds
// does not affect distances, so it is not compared.
bool same_metric(const ObsSegment& seg, const SourceClass& cls)
{
  return seg.L_km() == cls.L_km && seg.tau_seconds() == cls.tau_seconds &&
         seg.vertical_scale() == cls.vertical_scale &&
         seg.embedding().radius_km == Embedding{}.radius_km;
}

bool overlap(const Bucket& a, const Bucket& b)
{
  return a.start < b.start + b.len && b.start < a.start + a.len;
}

std::size_t twice(std::size_t k)
{
  return k > std::numeric_limits<std::size_t>::max() / 2
           ? std::numeric_limits<std::size_t>::max()
           : 2 * k;
}

}  // namespace

struct ObsIndex::Source
{
  SourceClass cls;
  std::vector<Entry> entries;  // non-empty segments only
  std::vector<Bucket> buckets;
};

namespace
{

struct Candidate
{
  float d2;
  std::uint64_t key;
  const Entry* entry;
  std::size_t row;
};

bool cand_less(const Candidate& a, const Candidate& b)
{
  return a.d2 < b.d2 || (a.d2 == b.d2 && a.key < b.key);
}

// What one query needs: the entries and buckets that overlap its time
// window, and the query point in each entry's embedding.
class Scope
{
 public:
  Scope(
    const std::vector<Entry>& entries,
    const std::vector<Bucket>& buckets,
    const SourceClass& cls,
    const ObsQuery& q)
      : q_(q)
  {
    w_ = std::isnan(q.window_s) ? 3.0 * cls.tau_seconds : q.window_s;
    const double lo = q.t_s - w_;
    const double hi = q.t_s + w_;
    for (const Entry& e : entries)
    {
      if (
        double(e.seg->obs_time_max_s()) >= lo &&
        double(e.seg->obs_time_min_s()) <= hi)
      {
        entries_.push_back(&e);
        qp_.push_back(
          embed<float>(q.lat_deg, q.lon_deg, q.height_m, q.t_s, e.emb));
      }
    }
    for (const Bucket& b : buckets)
    {
      if (double(b.start) <= hi && double(b.start + b.len) > lo)
        buckets_.push_back(&b);
    }
  }

  std::size_t size() const { return entries_.size(); }
  const Entry& entry(std::size_t i) const { return *entries_[i]; }
  const Tree::Point& point(std::size_t i) const { return qp_[i]; }

  // Keeps `h` of entry i if the row is in the window and visible.
  void offer(std::size_t i, const Tree::Hit& h, std::vector<Candidate>& out)
    const
  {
    const Entry& e = *entries_[i];
    const ObsSegment& s = *e.seg;
    const std::int64_t ot = s.obs_times_s()[h.id];
    if (!(std::fabs(double(ot) - q_.t_s) <= w_))
      return;
    const std::uint64_t key = s.report_keys()[h.id];
    const std::uint64_t seq = s.receipt_seqs()[h.id];
    if (!visible(key, seq, ot))
      return;
    out.push_back(Candidate{h.d2, key, &e, h.id});
  }

 private:
  // No newer version with seq <= as_of exists.  Every version shares the
  // obs_time, so it lives in a selected bucket that contains `ot`.
  bool visible(std::uint64_t key, std::uint64_t seq, std::int64_t ot) const
  {
    if (seq > q_.as_of)
      return false;
    if (seq == std::numeric_limits<std::uint64_t>::max())
      return true;
    for (const Bucket* b : buckets_)
    {
      if (!b->contains(ot))
        continue;
      const auto it = std::lower_bound(
        b->versions.begin(), b->versions.end(), Version{key, seq + 1});
      if (it != b->versions.end() && it->first == key && it->second <= q_.as_of)
        return false;
    }
    return true;
  }

  const ObsQuery& q_;
  double w_ = 0.0;
  std::vector<const Entry*> entries_;
  std::vector<Tree::Point> qp_;
  std::vector<const Bucket*> buckets_;
};

std::vector<ObsHit> to_hits(const std::vector<Candidate>& c, std::size_t n)
{
  std::vector<ObsHit> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    const ObsSegment* s = c[i].entry->seg.get();
    out.push_back(ObsHit{c[i].d2, s, s->report(c[i].row)});
  }
  return out;
}

}  // namespace

ObsIndex::ObsIndex(
  const SourceRegistry& sources,
  std::vector<std::shared_ptr<const ObsSegment>> segments)
{
  for (const SourceClass& c : sources.all())
  {
    auto s = std::make_unique<Source>();
    s->cls = c;
    sources_.push_back(std::move(s));
  }
  for (auto& seg : segments)
  {
    if (!seg)
      throw std::invalid_argument("ObsIndex: null segment");
    auto it = std::find_if(
      sources_.begin(), sources_.end(),
      [&](const auto& s) { return s->cls.name == seg->source_name(); });
    if (it == sources_.end())
    {
      throw std::invalid_argument(
        "ObsIndex: " + seg->path().string() + ": unknown source class '" +
        std::string(seg->source_name()) + "'");
    }
    Source& src = **it;
    ++n_segments_;
    max_seq_ = std::max(max_seq_, seg->receipt_seq_max());
    if (seg->size() == 0)
      continue;

    auto bit = std::find_if(
      src.buckets.begin(), src.buckets.end(),
      [&](const Bucket& b)
      {
        return b.start == seg->bucket_start_s() &&
               b.len == seg->bucket_seconds();
      });
    if (bit == src.buckets.end())
    {
      src.buckets.push_back(
        Bucket{seg->bucket_start_s(), seg->bucket_seconds(), {}});
      bit = src.buckets.end() - 1;
    }
    for (std::size_t i = 0; i < seg->size(); ++i)
      bit->versions.emplace_back(seg->report_keys()[i], seg->receipt_seqs()[i]);

    Entry e;
    e.seg = seg;
    if (same_metric(*seg, src.cls))
    {
      e.emb = seg->embedding();
      e.tree = seg->tree();
    }
    else
    {
      e.emb.t0_s = static_cast<double>(seg->bucket_start_s());
      e.emb.km_per_second = src.cls.lambda();
      e.emb.vertical_scale = src.cls.vertical_scale;
      std::vector<Tree::Point> pts(seg->size());
      for (std::size_t i = 0; i < seg->size(); ++i)
      {
        pts[i] = embed<float>(
          seg->lats_deg()[i], seg->lons_deg()[i], seg->heights_m()[i],
          static_cast<double>(seg->obs_times_s()[i]), e.emb);
      }
      e.own = std::make_unique<KdTree4<float>>(pts);
      e.tree = e.own->view();
      ++rebuilt_;
    }
    src.entries.push_back(std::move(e));
  }

  for (auto& src : sources_)
  {
    auto dup = [&](const std::string& where)
    {
      return std::invalid_argument(
        "ObsIndex: source '" + src->cls.name +
        "': duplicate (report_key, receipt_seq) " + where);
    };
    for (Bucket& b : src->buckets)
    {
      std::sort(b.versions.begin(), b.versions.end());
      if (
        std::adjacent_find(b.versions.begin(), b.versions.end()) !=
        b.versions.end())
        throw dup("in bucket " + std::to_string(b.start));
    }
    // Overlapping buckets exist only around a bucket_seconds change.
    for (std::size_t i = 0; i < src->buckets.size(); ++i)
    {
      for (std::size_t j = i + 1; j < src->buckets.size(); ++j)
      {
        const Bucket& a = src->buckets[i];
        const Bucket& b = src->buckets[j];
        if (!overlap(a, b))
          continue;
        std::vector<Version> both;
        std::set_intersection(
          a.versions.begin(), a.versions.end(), b.versions.begin(),
          b.versions.end(), std::back_inserter(both));
        if (!both.empty())
          throw dup("in overlapping buckets " + std::to_string(a.start));
      }
    }
  }
}

ObsIndex::~ObsIndex() = default;
ObsIndex::ObsIndex(ObsIndex&&) noexcept = default;
ObsIndex& ObsIndex::operator=(ObsIndex&&) noexcept = default;

const ObsIndex::Source& ObsIndex::source(std::string_view name) const
{
  for (const auto& s : sources_)
  {
    if (s->cls.name == name)
      return *s;
  }
  throw std::out_of_range(
    "ObsIndex: unknown source class '" + std::string(name) + "'");
}

std::vector<ObsHit> ObsIndex::knn(
  std::string_view source_name,
  const ObsQuery& q,
  std::size_t k) const
{
  const Source& src = source(source_name);
  if (k == 0)
    return {};
  const Scope scope(src.entries, src.buckets, src.cls, q);
  std::vector<Candidate> cands;
  for (std::size_t kp = twice(k);; kp = twice(kp))
  {
    cands.clear();
    // Smallest last d2 over trees that still have unreturned rows.
    float bound = std::numeric_limits<float>::infinity();
    bool open = false;
    for (std::size_t i = 0; i < scope.size(); ++i)
    {
      const Tree& tree = scope.entry(i).tree;
      const auto hits = tree.knn(scope.point(i), kp);
      if (hits.size() < tree.size())
      {
        open = true;
        bound = std::min(bound, hits.back().d2);
      }
      for (const auto& h : hits)
        scope.offer(i, h, cands);
    }
    std::sort(cands.begin(), cands.end(), &cand_less);
    if (!open)
      return to_hits(cands, std::min(k, cands.size()));
    const auto safe = static_cast<std::size_t>(
      std::lower_bound(
        cands.begin(), cands.end(), bound,
        [](const Candidate& c, float b) { return c.d2 < b; }) -
      cands.begin());
    if (safe >= k)
      return to_hits(cands, k);
  }
}

std::vector<ObsHit> ObsIndex::radius(
  std::string_view source_name,
  const ObsQuery& q,
  float r_km) const
{
  const Source& src = source(source_name);
  if (!(r_km >= 0.0f))
    return {};
  const Scope scope(src.entries, src.buckets, src.cls, q);
  std::vector<Candidate> cands;
  for (std::size_t i = 0; i < scope.size(); ++i)
  {
    for (const auto& h : scope.entry(i).tree.radius(scope.point(i), r_km))
      scope.offer(i, h, cands);
  }
  std::sort(cands.begin(), cands.end(), &cand_less);
  return to_hits(cands, cands.size());
}

}  // namespace atmo
