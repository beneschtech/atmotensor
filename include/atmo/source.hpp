// Source classes: one k-d tree metric per class of observations
// (docs/design/01-obs-store.md, "Source classes").
//
//   lambda (km per second) = L_km / tau_seconds
//
// i.e. tau_seconds of time difference counts as much as L_km of horizontal
// distance.  Each class gets its own trees (never mix lambdas in one tree);
// the embedding's t0 is the start of the time bucket the tree covers.
//
// Config is JSON (SourceRegistry::from_file / from_text / from_json), either a
// bare array of class objects or an object with a "sources" array (other
// top-level keys are left for other loaders):
//
//   { "sources": [ { "name": "metar", "bucket_seconds": 3600, "L_km": 100,
//                    "tau_seconds": 3600, "vertical_scale": 10,
//                    "retain_seconds": 259200 } ] }
//
// For a built-in class name (see defaults()) omitted fields take the built-in
// value; any other name must give every field.  Unknown keys are rejected
// (they are almost always typos).  Every error is std::invalid_argument.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/geo.hpp"

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

  // `base` supplies values for omitted fields (nullptr: all fields required).
  static SourceClass from_json(
    const nlohmann::json& j,
    const SourceClass* base);
  // Uses the built-in defaults for omitted fields when the name is built in.
  static SourceClass from_json(const nlohmann::json& j);
  static SourceClass from_text(std::string_view json_text);
};

// An ordered set of source classes with unique names.
class SourceRegistry
{
 public:
  SourceRegistry() = default;
  explicit SourceRegistry(std::vector<SourceClass> classes);

  // Suggested starting values from 01-obs-store.md (tune later).
  static SourceRegistry defaults();

  static SourceRegistry from_json(const nlohmann::json& j);
  static SourceRegistry from_text(std::string_view json_text);
  // Reads e.g. config/atmo.json.
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
