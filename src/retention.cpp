#include "atmo/retention.h"

#include <set>

namespace atmo
{

std::size_t RetentionPolicy::runs_for(std::string_view model) const
{
  auto it = runs_by_model.find(model);
  return it == runs_by_model.end() ? runs_per_model : it->second;
}

bool RetentionResult::changed() const
{
  return segments_expired + fields_expired + grids_expired > 0;
}

RetentionResult apply_retention(
  const Manifest& m,
  std::int64_t now_s,
  const RetentionPolicy& policy)
{
  RetentionResult r;
  r.manifest = m;
  Manifest& out = r.manifest;

  out.segments.clear();
  for (const auto& s : m.segments)
  {
    const SourceClass& c = m.sources.at(s.source);
    if (s.bucket_start_s + s.bucket_seconds < now_s - c.retain_seconds)
    {
      ++r.segments_expired;
    }
    else
    {
      out.segments.push_back(s);
    }
  }

  // Newest K run times per model.
  std::map<std::string, std::set<std::int64_t>> runs;
  for (const auto& f : m.fields)
  {
    runs[f.model].insert(f.run_s);
  }
  std::map<std::string, std::set<std::int64_t>> kept_runs;
  for (const auto& [model, times] : runs)
  {
    const std::size_t k = policy.runs_for(model);
    auto& kept = kept_runs[model];
    for (auto it = times.rbegin(); it != times.rend() && kept.size() < k; ++it)
    {
      kept.insert(*it);
    }
  }
  out.fields.clear();
  std::set<GridKey> used;
  for (const auto& f : m.fields)
  {
    if (kept_runs[f.model].count(f.run_s))
    {
      out.fields.push_back(f);
      used.insert(f.grid_key);
    }
    else
    {
      ++r.fields_expired;
    }
  }

  out.grids.clear();
  for (const auto& g : m.grids)
  {
    if (used.count(g.key))
    {
      out.grids.push_back(g);
    }
    else
    {
      ++r.grids_expired;
    }
  }
  return r;
}

}  // namespace atmo
