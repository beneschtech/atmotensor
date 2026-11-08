// Reference-data import (docs/design/05-reference-data.md, D10): the web
// app exports config/sources.json and config/stations.json; ingest
// validates them and publishes them through the manifest, so every
// snapshot pins one version of each.  Ingest also creates provisional
// stations for unknown identifiers and reports them to the web app through
// config/outbox/ (Q7).
//
// Rules implemented here (see QUESTIONS.md Q8 for the open policy points):
//   * An invalid export is rejected as a whole and the previous version
//     stays published; the error is reported once per file change.
//   * sources.json replaces the manifest's source classes.  Removing (or
//     renaming) a class that still has segments in the manifest is
//     rejected until those segments have expired.
//   * stations.json becomes the next station generation, built from the
//     previous one: stations in the export replace theirs; a web-app ref
//     ([1, 2^31)) of the previous generation missing from the export is an
//     error (refs are never removed); a provisional ref ([2^31, 2^32))
//     missing from the export is kept (the web app has not imported it
//     yet); the export may not invent provisional refs.  An export equal
//     to the current generation publishes nothing.
//   * Nothing is published before a valid sources.json exists.
//   * Outbox: whenever ingest publishes a generation with provisional
//     stations the last valid stations.json does not contain yet, it writes
//     config/outbox/provisional-<gen:016x>.json listing all of them.  A
//     station can therefore appear in several outbox files; the web app's
//     import must be idempotent by ref.  The engine never deletes outbox
//     files (the web app does after importing).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atmo/manifest.h"
#include "atmo/stations.h"

namespace atmo
{

// A config/stations.json export, or an outbox file (same station objects).
// Times: null <-> kTimeMin (valid_from) / kTimeMax (valid_to).
struct StationsExport
{
  static constexpr const char* kFormat = "atmo-stations";
  static constexpr const char* kOutboxFormat = "atmo-provisional-stations";
  static constexpr int kVersion = 1;

  std::vector<Station> stations;

  bool operator==(const StationsExport&) const = default;

  // Strict: format/version required, unknown keys rejected (top-level keys
  // other than format/version/stations are informational and ignored, as
  // for every export), every station field required, refs >= 1 and unique.
  // Station rules beyond the JSON shape are checked by the builder.
  // std::invalid_argument on any error.
  static StationsExport from_json(const nlohmann::json& j);
  static StationsExport from_text(std::string_view text);
  static StationsExport from_file(const std::filesystem::path& path);

  static nlohmann::json station_to_json(const Station& s);
  static Station station_from_json(const nlohmann::json& j);
  nlohmann::json to_json(const char* format = kFormat) const;
};

struct RefImportReport
{
  bool sources_updated = false;
  bool stations_updated = false;
  std::optional<std::uint64_t>
    published_seq;  // set if a manifest was published
  // One message per rejected file; the previous version stays published.
  std::vector<std::string> errors;
};

// Uses the ingest daemon's publisher; not thread-safe (call from the
// publishing thread).
class RefDataImporter
{
 public:
  // Re-announces pending provisional stations of the current generation if
  // their outbox file is missing (e.g. a crash after publishing).
  explicit RefDataImporter(ManifestPublisher& publisher);

  // Imports config/sources.json and config/stations.json if they changed
  // (inode, size or mtime) since the last poll, and publishes one manifest
  // with whatever was accepted.  I/O failures while publishing throw
  // std::runtime_error; validation failures go into the report.
  RefImportReport poll();

  // Adds provisional stations (ref must be 0; the provisional flag is set
  // here) as the next generation, publishes it, writes the outbox file, and
  // returns the assigned refs in input order.  std::invalid_argument if a
  // station is invalid or no source classes have been published yet.
  std::vector<std::uint32_t> add_provisional(std::vector<Station> stations);

 private:
  struct FileStamp
  {
    bool exists = false;
    std::uint64_t dev = 0, ino = 0, size = 0;
    std::int64_t mtime_ns = 0;
    bool operator==(const FileStamp&) const = default;
  };
  static FileStamp stamp(const std::filesystem::path& p);

  std::shared_ptr<const StationTable> current_stations() const;
  // Seals `b` (replacing an unpublished leftover of the same generation)
  // and returns its manifest entry.
  StationsEntry seal_generation(StationTableBuilder& b) const;
  void write_outbox(const StationTable& table) const;

  ManifestPublisher& pub_;
  std::filesystem::path root_, config_;
  std::optional<FileStamp> seen_sources_, seen_stations_;
  // Refs in the last valid stations.json (empty if none seen yet).
  std::set<std::uint32_t> exported_refs_;
};

}  // namespace atmo
