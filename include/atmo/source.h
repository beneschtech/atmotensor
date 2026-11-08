// Source classes: one k-d tree metric per class of observations
// (docs/design/01-obs-store.md, "Source classes").
//
//   lambda (km per second) = L_km / tau_seconds
//
// i.e. tau_seconds of time difference counts as much as L_km of horizontal
// distance.  Each class gets its own trees (never mix lambdas in one tree);
// the embedding's t0 is the start of the time bucket the tree covers.
//
// The source classes are edited in the web app and stored in MySQL (D10);
// Symfony exports them as config/sources.json
// (docs/design/05-reference-data.md), read with SourceRegistry::from_file:
//
//   { "format": "atmo-sources", "version": 1, "exported_at": "...",
//     "sources": [ { "name": "metar", "bucket_seconds": 3600, "L_km": 100,
//                    "tau_seconds": 3600, "vertical_scale": 10,
//                    "retain_seconds": 259200 } ] }
//
// Every field of a class is required and unknown keys are rejected (they
// are almost always typos); other top-level keys are ignored.  Every error
// is std::invalid_argument.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/geo.h"

namespace atmo
{

struct SourceClass
{
  // Used as a directory name (obs/<name>/...) and stored in segment headers.
  static constexpr std::size_t kMaxName = 31;

  std::string name;
  std::int64_t bucket_seconds = 3600;
  double L_km = 100.0;
  double tau_seconds = 3600.0;
  double vertical_scale = 10.0;
  std::int64_t retain_seconds = 259200;  // 72 h (D1)

  bool operator==(const SourceClass& o) const = default;

  // km per second: tau_seconds of time costs as much as L_km of distance.
  double lambda() const;

  // floor(t_s / bucket_seconds) * bucket_seconds, also for negative t_s.
  std::int64_t bucket_start_s(std::int64_t t_s) const;

  // Embedding for the tree of the bucket starting at `start_s`.
  // std::invalid_argument if that is not a bucket start.
  Embedding make_embedding(std::int64_t start_s) const;

  static bool valid_name(std::string_view n);
  void validate() const;

  nlohmann::json to_json() const;

  // One class object; every field required.
  static SourceClass from_json(const nlohmann::json& j);
  static SourceClass from_text(std::string_view json_text);
};

// An ordered set of source classes with unique names.
class SourceRegistry
{
 public:
  SourceRegistry() = default;
  explicit SourceRegistry(std::vector<SourceClass> classes);

  bool operator==(const SourceRegistry&) const = default;

  static constexpr const char* kFormat = "atmo-sources";
  static constexpr int kVersion = 1;

  // Suggested starting values from 01-obs-store.md (tune later).  The same
  // values seed the MySQL table; here they serve tests and tools.
  static SourceRegistry defaults();

  // A whole export: {"format": "atmo-sources", "version": 1, "sources": [...]}
  static SourceRegistry from_json(const nlohmann::json& j);
  static SourceRegistry from_text(std::string_view json_text);
  // Reads config/sources.json.
  static SourceRegistry from_file(const std::filesystem::path& path);

  nlohmann::json to_json() const;

  const SourceClass* find(std::string_view name) const;
  // std::out_of_range if absent.
  const SourceClass& at(std::string_view name) const;

  const std::vector<SourceClass>& all() const { return classes_; }
  std::size_t size() const { return classes_.size(); }

 private:
  void validate() const;

  std::vector<SourceClass> classes_;
};

}  // namespace atmo
