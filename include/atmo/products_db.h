// The derived_* tables for atmo_productd (06-derived-products.md; T21):
// product and kernel definitions (web-owned, read here), the job queue,
// the arrival cursor and heartbeat, built-in processor registration and
// kernel build results (engine-owned columns).  Runs its SQL over a
// Catalog connection (catalog.h), so it is part of `atmo_catalog` and only
// built with a MySQL client library.  All times are UTC (the connection
// runs with time_zone +00:00).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "atmo/catalog.h"
#include "atmo/products.h"

namespace atmo
{

// derived_state.schema_version this engine knows (web migration
// Version20261121220000: rebuild_requested_at).
constexpr int kDerivedSchemaVersion = 2;

// derived_job.flags
constexpr std::uint32_t kJobChainStart = 1;

struct DerivedJob
{
  std::uint64_t id = 0;
  std::uint64_t product_id = 0;
  int product_revision = 1;
  int processor_version = 0;
  int kernel_revision = 0;
  std::uint64_t run_id = 0;
  std::int64_t offset_end_s = 0;
  std::map<std::string, std::uint64_t> inputs;
  std::uint32_t flags = 0;
  int attempts = 0;
};

struct JobInsertStats
{
  std::size_t created = 0;     // new rows
  std::size_t requeued = 0;    // same job, other input messages (Q16b copy)
  std::size_t superseded = 0;  // jobs of older revisions at the same key
};

class ProductsDb
{
 public:
  // std::runtime_error unless derived_state.schema_version is
  // kDerivedSchemaVersion.
  explicit ProductsDb(Catalog& db);

  // ---- state
  void heartbeat();
  std::uint64_t cursor();
  void set_cursor(std::uint64_t seq);

  // ---- definitions
  std::vector<Product> products();
  std::vector<ProductKernel> kernels();
  // Replaces derived_processor with this list.
  void register_builtins(const std::vector<BuiltinProcessor>& b);
  // Kernels whose revision is not built yet (revision != built_revision).
  void set_build(
    std::uint64_t kernel_id,
    int revision,
    bool ok,
    const std::string& log);

  // ---- arrivals: messages with ingest_seq in (after, upto], oldest first,
  // at most `limit`; `last` = the highest ingest_seq returned (or `upto`
  // when fewer than `limit` came back, so the cursor can move past it).
  std::vector<ArrivedMessage> arrivals(
    std::uint64_t after,
    std::uint64_t upto,
    std::size_t limit,
    std::uint64_t& last);

  // ---- rebuild requests (06 "Jobs"): products with rebuild_requested_at
  // set, and compare-and-clear (false if it changed since it was read).
  std::vector<std::pair<std::uint64_t, std::string>> rebuild_requests();
  bool clear_rebuild(std::uint64_t product_id, const std::string& seen);

  // ---- jobs
  JobInsertStats add_jobs(const std::vector<JobPlan>& plans);
  // Jobs left `running` longer than this go back to `queued`.
  std::size_t requeue_stale(int timeout_s);
  // Claims the oldest queued job that is due (queued_at <= now; retries
  // wait) with a conditional UPDATE (safe with several workers); `worker`
  // must be unique per claimer.
  std::optional<DerivedJob> claim(const std::string& worker);
  void done(
    std::uint64_t job_id,
    const std::map<std::string, std::uint64_t>& outputs,
    const std::string& device);
  // Back to `queued` (claimable after `retry_delay_s`) while attempts <
  // max_attempts, else `failed`.
  void failed(
    std::uint64_t job_id,
    const std::string& error,
    int attempts,
    int max_attempts,
    int retry_delay_s = 60);
  void superseded(std::uint64_t job_id);

  // ---- outputs
  // Message ids of a catalogued file by (discipline, category, number,
  // level1 type, level1 value as float bits).
  std::map<std::string, std::uint64_t> output_messages(
    const std::string& file_path,
    const Product& p);
  // Names the product's model (grib_model.name, if still '') and gives its
  // model grids the product slug as api_name when no other model grid has
  // it (the engine's default api_name is "254-0-<process>_<grid>").
  void name_outputs(const Product& p);

  Catalog& db() { return db_; }

 private:
  Catalog& db_;
};

}  // namespace atmo
