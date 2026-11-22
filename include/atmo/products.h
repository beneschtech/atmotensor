// Derived products (D12 as redesigned 2026-11-21,
// docs/design/06-derived-products.md; T21).
//
// The core of atmo_productd without MySQL, eccodes or OpenCL:
//   * product, kernel and interface definitions as read from the derived_*
//     tables (the web app owns them; products_db.h reads them);
//   * the built-in C++ processors (wind_speed, wind_direction, ...): one
//     function over arrays in the grid's normalised order (index j*ni + i),
//     the same data shape the OpenCL kernel ABI uses;
//   * the matcher: which (product, source run, offset) jobs are ready,
//     given the catalog, either for newly arrived messages or for every
//     retained run (rebuild requests).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "atmo/grib_api.h"
#include "atmo/gribsig.h"

namespace atmo
{

// ---- definitions ------------------------------------------------------------

enum class OffsetRule
{
  same,           // same run and offset as the job
  previous_step,  // same run, offset - step_s
  own_previous,   // this product's own output one step back
};

std::optional<OffsetRule> offset_rule_from(std::string_view s);

struct ProductInput
{
  std::string role;
  OffsetRule rule = OffsetRule::same;
  std::string model;       // api_name or product slug (not own_previous)
  std::string field;       // "name@level" (not own_previous)
  std::string own_output;  // output role (own_previous only)
  std::int64_t step_s = 0;
  bool required = true;
};

struct ProductOutput
{
  std::string role;
  std::uint32_t discipline = 0, category = 0, number = 0;
  GribLevel level1;
  GribLevel level2;
  std::string units;
};

struct ParamDecl
{
  std::string name;
  bool is_int = false;
  std::optional<double> def, min, max;
};

struct IoDecl
{
  std::string role;
  std::string units;
  bool needs_alpha = false;  // inputs: earth-relative winds wanted
};

// What a kernel or built-in processor takes and makes, in argument order.
struct ProductInterface
{
  std::vector<IoDecl> inputs;
  std::vector<IoDecl> outputs;
  std::vector<ParamDecl> params;

  // From the JSON columns of derived_kernel / derived_processor.
  // std::invalid_argument for malformed text.
  static ProductInterface from_json(
    const std::string& inputs,
    const std::string& outputs,
    const std::string& params);
  void to_json(std::string& inputs, std::string& outputs, std::string& params)
    const;
  bool needs_alpha() const;
};

enum class ProcessorKind
{
  builtin,
  kernel,
};

struct Product
{
  std::uint64_t id = 0;
  std::string slug;
  std::string name;
  ProcessorKind kind = ProcessorKind::kernel;
  std::string processor;  // built-in name
  std::optional<std::uint64_t> kernel_id;
  std::map<std::string, double> params;  // by declared name
  bool enabled = true;
  std::uint32_t process_id = 0;
  std::optional<std::int64_t> spinup_s;
  int revision = 1;
  std::vector<ProductInput> inputs;
  std::vector<ProductOutput> outputs;
  std::optional<std::string> rebuild_requested;  // database text, as read

  const ProductOutput* output(std::string_view role) const;
};

struct ProductKernel
{
  std::uint64_t id = 0;
  std::string name;
  int revision = 1;
  std::string source;
  std::string entry;
  ProductInterface iface;
  std::string source_hash;
  std::optional<int> built_revision;
  std::string build_status;  // pending | ok | error
};

// ---- processors -------------------------------------------------------------

// Arrays of ni*nj floats in normalised order, NaN = missing; params as
// declared (int params hold whole numbers).  `alpha` (grid rotation,
// radians; zeros where winds are already earth-relative) is set when any
// input needs it.
struct ProcessorArgs
{
  std::uint32_t ni = 0;
  std::uint32_t nj = 0;
  std::vector<const float*> inputs;
  std::vector<float*> outputs;
  const float* alpha = nullptr;
  std::vector<double> params;

  std::size_t cells() const { return static_cast<std::size_t>(ni) * nj; }
};

using ProcessorFn = void (*)(const ProcessorArgs&);

struct BuiltinProcessor
{
  std::string name;
  int version = 1;  // bumped whenever the output for the same input can change
  std::string description;
  ProductInterface iface;
  ProcessorFn fn = nullptr;
};

const std::vector<BuiltinProcessor>& builtin_processors();
const BuiltinProcessor* find_builtin(std::string_view name);

// Runs OpenCL kernels (T22; opencl.h makes the real one).  build() compiles
// a kernel revision (cached by source hash per device) and returns the
// build log; run() executes it over `args` on the first configured device
// that can hold the job and returns that device's name.  std::runtime_error
// when no device could run it (the message says why for each).
struct KernelBuild
{
  bool ok = false;
  std::string log;
};

class KernelExecutor
{
 public:
  virtual ~KernelExecutor();
  virtual KernelBuild build(const ProductKernel& k) = 0;
  virtual std::string run(
    const ProductKernel& k,
    const ProcessorArgs& args) = 0;
};

// Parameter values in declaration order (missing -> default).
// std::invalid_argument if one has neither.
std::vector<double> param_values(
  const ProductInterface& iface,
  const std::map<std::string, double>& given);

// ---- matching ---------------------------------------------------------------

// A catalogued message, as the arrival monitor reads it.
struct ArrivedMessage
{
  std::uint64_t id = 0;
  std::uint64_t run_id = 0;
  std::uint64_t signature_id = 0;
  GribLevel level1;
  std::int64_t offset_end_s = 0;
};

// A job that can run now.  Versions are 0 where not applicable.
struct JobPlan
{
  std::uint64_t product_id = 0;
  int product_revision = 1;
  int processor_version = 0;
  int kernel_revision = 0;
  std::uint64_t run_id = 0;  // the source run (first non-own input's)
  std::int64_t offset_end_s = 0;
  std::map<std::string, std::uint64_t> inputs;  // role -> message id
  bool chain_start = false;  // an own_previous input had no history

  // Key of derived_job's unique index (with the versions).
  bool same_job(const JobPlan& o) const;
};

// What a product's inputs resolve to in one catalog generation.
struct ResolvedInput
{
  std::set<std::uint64_t> model_grids;
  std::set<std::uint64_t> signatures;
  GribLevel level;      // first surface; value compared as float
  std::string problem;  // non-empty: cannot be matched (yet)
};

// Builds, per catalog generation, the resolution of every enabled product's
// inputs, then answers which jobs are ready.  Names resolve exactly as the
// by-name API resolves them, but best guesses do not count: an input whose
// field is only a guess is a problem, not a match.  A model that is a
// product slug means that product's outputs (centre 254, subcentre 0, its
// process id), whatever their api_name.
class ProductMatcher
{
 public:
  // Versions to stamp on plans: product id -> (processor version, kernel
  // revision).
  using Versions = std::map<std::uint64_t, std::pair<int, int>>;

  ProductMatcher(
    std::shared_ptr<const CatalogTables> tables,
    const CatalogFieldStore& names,
    CatalogQuery& catalog,
    std::vector<Product> products,
    Versions versions);

  // Problems per product (unresolvable inputs), for logging.
  std::map<std::uint64_t, std::vector<std::string>> problems() const;

  // Jobs made ready by these arrivals (deduplicated).
  std::vector<JobPlan> plan(const std::vector<ArrivedMessage>& arrived);
  // Every ready job of one product over every retained run (rebuild).
  std::vector<JobPlan> plan_all(std::uint64_t product_id);
  // The job of (product, run, offset) if its inputs are complete; `why`
  // says what is missing otherwise.
  std::optional<JobPlan> ready(
    std::uint64_t product_id,
    std::uint64_t run_id,
    std::int64_t offset_end_s,
    std::string* why = nullptr);

  // Model grids holding a product's outputs.
  std::set<std::uint64_t> output_grids(std::uint64_t product_id) const;

 private:
  struct Entry
  {
    Product p;
    std::vector<ResolvedInput> in;  // parallel to p.inputs
    int primary = -1;               // index of the first non-own input
    std::pair<int, int> versions{0, 0};
  };
  const Entry* entry(std::uint64_t product_id) const;
  ResolvedInput resolve(const ProductInput& in) const;
  std::set<std::uint64_t> product_grids(std::uint32_t process_id) const;
  const CatRun* run_like(
    std::uint64_t model_grid_id,
    std::int64_t basetime_s,
    int member) const;
  std::vector<CatMessage> find(
    std::uint64_t run_id,
    std::int64_t offset,
    const std::set<std::uint64_t>& sigs,
    const GribLevel& level);
  std::optional<std::uint64_t> own_previous(
    const Entry& e,
    const ProductInput& in,
    const CatRun& run,
    std::int64_t offset,
    bool& wait,
    std::string* why);
  std::vector<CatMessage>& at(std::uint64_t run_id, std::int64_t offset);

  std::shared_ptr<const CatalogTables> t_;
  const CatalogFieldStore& names_;
  CatalogQuery& catalog_;
  std::vector<Entry> entries_;
  std::map<std::string, std::uint32_t> slugs_;  // lower-cased slug -> process
  // messages_at() results of this planning pass.
  std::map<std::pair<std::uint64_t, std::int64_t>, std::vector<CatMessage>>
    memo_;
};

}  // namespace atmo
