// atmo_productd's work loop as a library (06-derived-products.md; T21), so
// tests can drive it pass by pass against a throwaway database.  Part of
// `atmo_catalog` (needs MySQL and eccodes).
//
// One pass (step()):
//   1. heartbeat (derived_state.heartbeat_at);
//   2. reload definitions and, when the catalog generation moved, the
//      tables; rebuild the matcher;
//   3. compile kernels whose revision is not built (KernelExecutor);
//   4. arrivals: messages with ingest_seq in (cursor, generation] -> jobs
//      (ProductMatcher::plan), then the cursor moves;
//   5. rebuild requests: plan_all() per product, compare-and-clear;
//   6. requeue jobs left running past the timeout;
//   7. claim and run up to `max_jobs` jobs: decode the inputs (GribApi,
//      parameter units), run the built-in or kernel, write one GRIB2 file
//      per job (grib_derive, one message per output), catalogue it
//      (catalog_grib + Catalog::ingest: a new generation, which is what
//      triggers chained products), record the output message ids.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "atmo/catalog.h"
#include "atmo/gribstore.h"
#include "atmo/products.h"
#include "atmo/products_db.h"

namespace atmo
{

struct ProductDaemonOptions
{
  std::filesystem::path root;  // store root (/opt/atmotensor/data)
  std::filesystem::path tmp;   // scratch for output files before linking
  CatalogConfig catalog;
  std::string worker;  // unique per process (host:pid)
  int job_timeout_s = 600;
  int max_attempts = 2;
  int retry_delay_s = 60;  // a failed attempt waits this long in the queue
  std::size_t arrivals_batch = 200000;
  int bits = 16;  // GRIB2 simple packing of outputs
  std::size_t cache_mb = 512;
};

struct ProductPassStats
{
  std::uint64_t generation = 0;
  std::size_t arrivals = 0;
  std::size_t jobs_created = 0;
  std::size_t jobs_requeued = 0;
  std::size_t jobs_superseded = 0;
  std::size_t rebuilds = 0;
  std::size_t kernels_built = 0;
  std::size_t stale = 0;
  std::size_t ran = 0;
  std::size_t failed = 0;
  std::vector<std::string> log;  // one line per notable event
};

class ProductDaemon
{
 public:
  // `kernels` may be null (kernel products then fail with a reason).
  // Connects twice (a writer and a read-only reader).  std::runtime_error
  // if either schema version is unknown.
  ProductDaemon(
    ProductDaemonOptions opt,
    std::unique_ptr<KernelExecutor> kernels);
  ~ProductDaemon();

  ProductPassStats step(std::size_t max_jobs = 64);

  // Passes until `stop`, sleeping `poll` when a pass found nothing to run.
  // Database errors are logged and retried after a pause (reconnecting).
  void run(const std::atomic<bool>& stop, std::chrono::milliseconds poll);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Output file name of a job: "<slug>.f<HHH>[<MM>].j<job>.<hash>.grib2",
// hash = the first 8 hex digits of the content's FNV-1a 64.  Files are
// immutable: a job that runs again (replaced input, rebuild) with other
// results gets another name; identical results reuse the file.
std::string product_output_name(
  const std::string& slug,
  std::int64_t offset_end_s,
  std::uint64_t job_id,
  std::uint64_t content_hash);

}  // namespace atmo
