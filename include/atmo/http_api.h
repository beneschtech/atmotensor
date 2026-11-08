// HTTP API of the query daemon (T10; 04-query-api.md): routes FastCGI
// requests to /healthz, /v1/meta and /v1/point over the current snapshot or
// one pinned with ?snapshot=<seq>, plus the CURRENT watcher.
//
// Every JSON response carries X-Atmo-Snapshot.  Errors are
// {"error": {"code", "message"}} with 4xx for caller mistakes and 503
// while no snapshot is loaded.  Responses for the live snapshot are
// Cache-Control: no-store; pinned ones are immutable and cacheable.
// /v1/point requires `time`, so equal requests on one snapshot give
// byte-identical responses.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "atmo/fcgi.h"
#include "atmo/query.h"
#include "atmo/snapshot.h"

namespace atmo
{

// "a=1&b=x%20y+z" -> {a: "1", b: "x y z"}; a repeated key keeps the last
// value.  std::invalid_argument for bad percent escapes.
std::map<std::string, std::string> parse_query_string(std::string_view qs);

class QueryApi
{
 public:
  // `loader` must outlive the API.  Up to `pinned_cache` older snapshots
  // stay open for ?snapshot=.
  QueryApi(
    std::filesystem::path root,
    SnapshotLoader& loader,
    std::size_t pinned_cache = 4);
  ~QueryApi();

  // Thread-safe.
  fcgi::Response handle(const fcgi::Request& r) const;

 private:
  struct View
  {
    std::shared_ptr<const Snapshot> snap;
    std::shared_ptr<const SnapshotFieldStore> store;
    bool pinned = false;
  };
  // Current snapshot (nullptr snap if none yet), or the pinned one.
  View view(std::uint64_t pinned_seq) const;

  std::filesystem::path root_;
  SnapshotLoader& loader_;
  std::size_t pinned_cache_;
  mutable std::mutex mu_;
  mutable View current_;
  mutable std::list<View> pinned_;  // most recently used first
};

// Calls loader.refresh() whenever manifest/CURRENT changes (inotify on the
// manifest directory) and at least every `poll` as a fallback.  Errors
// from refresh() go to `on_error` (the old snapshot stays).
class SnapshotWatcher
{
 public:
  SnapshotWatcher(
    std::filesystem::path root,
    SnapshotLoader& loader,
    std::function<void(const std::string&)> on_error,
    std::chrono::milliseconds poll = std::chrono::milliseconds(1000));
  ~SnapshotWatcher();  // stop()
  SnapshotWatcher(const SnapshotWatcher&) = delete;
  SnapshotWatcher& operator=(const SnapshotWatcher&) = delete;

  void start();
  void stop();

 private:
  void run();

  std::filesystem::path root_;
  SnapshotLoader& loader_;
  std::function<void(const std::string&)> on_error_;
  std::chrono::milliseconds poll_;
  std::atomic<bool> stop_{false};
  int wake_r_ = -1, wake_w_ = -1;
  std::thread thread_;
};

}  // namespace atmo
