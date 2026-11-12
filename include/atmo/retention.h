// Retention (decision D1; 01-obs-store.md "Retention", 03-field-tiles.md):
// which files the next manifest keeps.  Expiry only removes entries; the
// publisher then retires the files and collect_garbage() unlinks them after
// the grace period (02-manifest-and-processes.md, "Retirement").
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "atmo/manifest.h"

namespace atmo
{

struct RetentionPolicy
{
  // Newest K runs per model (D1 default 4); per-model overrides.
  std::size_t runs_per_model = 4;
  std::map<std::string, std::size_t, std::less<>> runs_by_model;

  // K for `model`.
  std::size_t runs_for(std::string_view model) const;
};

struct RetentionResult
{
  Manifest manifest;  // `m` without the expired entries
  std::size_t segments_expired = 0;
  std::size_t fields_expired = 0;
  bool changed() const;
};

// Pure function of its inputs:
//   * a segment expires when bucket_start + bucket_seconds (its own bucket
//     length) < now - retain_seconds of its source class, so every version
//     in a bucket expires together;
//   * fields: per model, the newest K distinct run times stay (all their
//     forecast hours), older runs expire whole;
//   * grids never expire: grids/ is shared with the grid catalog (D13,
//     T18), whose runs Catalog::expire() retires (catalog.h).
// Stations, seq, sources and next_receipt_seq are untouched.
RetentionResult apply_retention(
  const Manifest& m,
  std::int64_t now_s,
  const RetentionPolicy& policy);

}  // namespace atmo
