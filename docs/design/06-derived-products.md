# Derived products: arrival monitor, product definitions, processors

Status: decided 2026-11-21 (Chris + architect Claude), Q1-Q5 answered. Replaces the
pre-catalog sketch of D12 (field files + variable codes): since D13 every
grid lives in the GRIB catalog, so derived products do too. Tasks T11b
(rewritten) and T20-T23 in TASKS.md. Decisions in "Questions" at the end.

## Goal

When grids arrive, run processes that make new grids from them, the way
LEADS makes its centre-254 "MFS" products. A product's outputs are
ordinary catalogued GRIB messages, so they can be drawn, queried,
exported, and used as inputs to further products (chains). Everything is
configured in the web app; the engine supplies the processors.

First product, proof of concept: 10 m wind speed from 10 m u/v. Then the
pieces the fruiting model needs (precip buckets from run totals, rolling
sums/means over runs, terrain-aware scores), so that model becomes a
chain of products whose intermediates can each be looked at on the map.

## Pieces

    ingest (atmo_grib_ingest) --commit, generation++--> grib_message rows
                                                          (ingest_seq)
           atmo_productd: monitor  -- reads rows with ingest_seq > cursor
                          matcher  -- which product inputs did they fill?
                          queue    -- derived_job rows (MySQL)
                          workers  -- run processor, write GRIB2,
                                      catalog it like any ingest
                                      (new ingest_seq -> chains trigger)
    web: product editor, arrival/job monitor, colormap editor

### Arrival monitor

`atmo_productd` polls `grib_catalog_state.generation` once a second (as
`atmo_queryd` does). When it moves, it reads `grib_message` rows with
`ingest_seq` in (cursor, generation] (indexed, T14) and feeds them to the
matcher, then stores the new cursor in `derived_state` (single row). A
restart resumes from the cursor; nothing is missed and replaying is
harmless because job creation is idempotent (unique key below).

No separate arrivals table: the catalog already is the record of what
arrived and when (`grib_file.ingested_at`, `ingest_seq`). The web
monitor page reads the catalog directly.

### Product definitions (web owns, engine reads)

Built in T20 (web migration Version20261121200000, entities in
src/Entity/Derived). The web app already had a `product` table (COG
files), hence the `derived_` prefix. The derived_* tables carry their own
schema version in `derived_state.schema_version` (2 since the
rebuild request column), checked by
atmo_productd; `grib_catalog_state.schema_version` stays 3, so ingest and
atmo_queryd are unaffected. CHECK constraints enforce the input shapes
(own_previous: own_output_role + step_s, no grid/field; same /
previous_step: grid + field), process_id 1..254, slug [a-z0-9_.-]+.

Processes (web migration Version20261122150000): `derived_group` (id,
name unique, description, sort_order) and a nullable
`derived_product.group_id` (ON DELETE SET NULL) group products for
display: "Turbulence", "Mushrooms". The Derived page draws each process
as a graph and the grid picker files AtmoTensor / Derived models under
it. Display only: atmo_productd never reads either (it selects explicit
columns), a product's process is not part of its output signature (no
revision bump, no rebuild), its GRIB identity stays centre 254 /
subcentre 0 / its own process_id, and the derived schema version stays 2.

    derived_product    id, slug (unique), name, processor_kind
                       ENUM(builtin, kernel), processor NULL (built-in
                       name), kernel_id NULL, params JSON, trigger_kind
                       ENUM(offset), enabled, process_id
                       (generatingProcessIdentifier of its outputs,
                       1..254, unique), spinup_s NULL, owner_id NULL,
                       revision, rebuild_requested_at NULL, updated_at
    derived_product_input      id, product_id, role (processor's input name, e.g.
                       "u"), model_grid_api (e.g. "gfs_0.25p"; a product's
                       slug for chains), field (field spec exactly as the
                       by-name API takes it: "wind_u@10m"),
                       own_output_role (own_previous only), offset_rule
                       ENUM(same, previous_step, own_previous) DEFAULT
                       same, step_s NULL, required
                       unique(product_id, role)
    derived_product_output     id, product_id, role, discipline, category, number,
                       level1_type, level1_value, level2_type,
                       level2_value, units (informational)
    derived_processor  name (PK), version, description, inputs JSON,
                       outputs JSON, params JSON schema
                       (written by the engine at start; web reads it to
                       build the editor form)

Input matching is exact, by the same names the by-name API uses
(api_name + field + first surface), never the best-guess path: guessing is
for humans typing URLs. The matcher resolves each input to (model_grid,
signature set, level) once per catalog generation.

One product = one processor run per (run, offset). Several outputs per
product are allowed (a fruiting score product may also emit its
sub-scores).

Chains: an input may name another product's api_name. The web app
rejects a save that would make a cycle (parents stored as the source of
truth, as discussed for the DAG; DFS on save).

### Trigger

`offset` (only trigger in v1): for each (product, input run, offset) the
job is created when every input with offset_rule `same` has a message at
that offset in the same run, and every `previous_step` input has one at
offset - step_s. Inputs must come from the same base time, except chained
product outputs, which carry their source run's base time anyway.

`own_previous` (Q5, state carried across steps and runs): the input is
this product's own output, one step back. For offset t after the
product's first offset in a run, that is its output at t - step_s in the
same run (so a run's jobs run in order, each waiting for the previous
one's output to be catalogued). For the run's first offset it is its
output from the newest earlier run of the same model_grid at the same
valid time (6-hourly runs: the previous run's f006 seeds this run's
f000). This makes the chain behave like an analysis cycle: state is
handed from run to run at matching valid times, and each forecast walks
forward from it, so nothing is double counted or skipped. If no such
output exists (first run, missing run, retention), the kernel gets an
all-NaN array and the param `has_prev = 0`, the job is flagged
`chain_start`, and the web shows the product as "history rebuilding"
until the chain has run for the product's declared `spinup_s`.

Later, not in v1: `run` trigger (once a run is complete). "Complete" needs
an expected offset list per model, which the catalog does not have.

### Jobs

    derived_job   id, product_id, product_revision, processor_version,
                  source_run_id (grib_run, ON DELETE CASCADE),
                  offset_end_s, status ENUM(queued, running, done, failed,
                  superseded), attempts, inputs JSON (role -> message id),
                  outputs JSON (role -> message id), error TEXT,
                  device, flags (chain_start), claimed_by, queued_at,
                  started_at, finished_at
                  unique(product_id, source_run_id, offset_end_s,
                         product_revision, processor_version,
                         kernel_revision)   (versions 0 = n/a)

Claimed with a conditional UPDATE (as the export worker does), so several
workers or processes are safe. A failed job retries once, then stays
failed with the processor's message; the web shows it. A job left
`running` by a dead worker is requeued after a timeout.

A newer copy of an input (same unique key, latest wins in the catalog,
Q16b) re-queues the SAME job row: inputs updated, attempts reset, status
queued (the unique key has no input ids, so a new row would collide).
`superseded` marks a job of an older product revision / processor
version / kernel revision once a job of the newer one exists for the same
(product, run, offset). Editing a product bumps `revision`; existing
outputs stay until "Rebuild" is pressed (web), which queues jobs for
every retained run.

Rebuild requests (derived schema 2, web migration Version20261121220000):
the web app sets `derived_product.rebuild_requested_at` (UTC) when a
product is created, enabled again, or Rebuild is pressed. The arrival
monitor only sees new arrivals, so without this a product would never
process data that arrived before it existed. atmo_productd, on seeing a
non-NULL value: matches the product against every retained run of its
inputs, as if each matching message had just arrived; creates jobs for
the current revision (product revision, processor version / kernel
revision) where inputs are complete and no such job exists; marks jobs
of older revisions at the same (run, offset) `superseded`; then clears
the column only if it still holds the value it read (`UPDATE ... SET
rebuild_requested_at = NULL WHERE id = ? AND rebuild_requested_at = ?`),
so a second press during the scan is not lost. Disabled products are
skipped (and their request kept). The web app may also set a `failed`
job back to `queued` (Retry: attempts 0, error/claim/times cleared).

Provenance: the job row is the record (input message ids, processor
name + version, product revision). The output message's `grib_message`
row links back through `derived_job.outputs`.

### Outputs

GRIB2, written with eccodes by cloning the first input message (keeps
section 3, the grid, and the time section) and setting:
- discipline/category/number and the levels from `derived_product_output`;
- centre 254 (local, as LEADS), subcentre 0,
  generatingProcessIdentifier = `product.process_id`,
  typeOfGeneratingProcess = 2 (forecast) as the source;
- packing: simple packing, bitsPerValue chosen from the processor's
  declared precision (default 16 over the field's range); NaN -> bitmap.

File: `grib/<product slug>/<base time>/<slug>.f<offset>.grib2`, linked
in and catalogued by the same `Catalog::ingest` path as feed files, so
it gets a model (centre 254 -> subcentre 0 -> process id), the source's
grid, the source's base time, and a new `ingest_seq`, which is what
triggers downstream products.

The web app names centre 254 "AtmoTensor", subcentre 0 "Derived", and
each product's model row after the product (`grib_model.name`), and sets
`api_name` to the product slug.

### Processors: built-in C++ and OpenCL kernels

Decided by Chris 2026-11-21: wherever possible a product points at an
OpenCL kernel, so the maths can be changed on the fly (edit, save, the
next job uses it) without rebuilding or redeploying the engine.
Built-in C++ processors remain for what a kernel cannot express and as
the reference the kernels are tested against.

    derived_kernel   id, name, revision (bumped on every save), source
                     TEXT (OpenCL C), entry (kernel function name),
                     inputs JSON (ordered roles, SI units, needs_alpha),
                     outputs JSON (ordered roles, default parameter),
                     params JSON (ordered scalar args: name, type
                     float|int, default, min, max), source_hash,
                     build_status ENUM(pending, ok, error), build_log
                     TEXT, built_revision, updated_at, updated_by
    derived_kernel_revision  kernel_id, revision, source, hashes,
                     saved_at, saved_by   (history; outputs keep
                     pointing at the revision that made them)

Columns written by the engine: build_status, build_log, built_revision.
Everything else: web app.

Kernel ABI (one work item per grid cell, global size = ni * nj, index
j*ni + i as everywhere else; NaN = missing in and out):

    __kernel void <entry>(
        __global const float* <input 1>, ... ,   // declared order
        __global float* <output 1>, ... ,        // declared order
        const int ni, const int nj,
        [__global const float* alpha,]           // if any input needs it
        <param 1>, ... )                         // float / int scalars

ni/nj let a kernel read neighbours (stencils: gradients, smoothing).
Inputs are SI floats; `alpha` is the grid's rotation (grid.h) for
kernels that need earth-relative winds. This is a narrow ABI for
products only; T13's general device views (field tiles, obs) stay a
separate design.

Compile and run:
- `atmo_productd` compiles a kernel when its revision changes (polls
  `derived_kernel` with the catalog generation), caches the program
  binary per (source_hash, device), and writes build_status / build_log
  back. The web editor shows the log; a kernel that does not build fails
  only its own products' jobs, with the log as the error.
- Jobs record kernel id + revision (instead of processor_version).
  Saving a new revision affects new jobs; "Rebuild" re-runs retained
  runs with it.
- Device: first in the configured list that can hold the job (Q2b);
  the device is recorded on the job.
- A watchdog timeout per job; kernels run inside atmo_productd, never in
  ingest or the query daemon, so a hung or crashing kernel costs product
  jobs only.
- Editing kernels is an administrator action. Kernels are code running
  on our hardware; D11 (no customer code on our servers) still holds,
  so customers configure products from existing kernels and processors
  but do not write kernels.

Built-in C++ processors (spec in code):

    struct ProcessorSpec { name, version, inputs (role, units SI),
                           outputs (role, default parameter), params };
    using ProcessorFn = void(*)(const ProcessorInputs&, const json& params,
                                ProcessorOutputs&);

Same data shape as the kernel ABI. The engine writes the list to
`derived_processor` at start.

v1:
- `wind_speed` (u, v -> speed) as BOTH a built-in and a seed kernel
  (`wind_speed.cl`, seeded into derived_kernel by a web command); the
  test checks they agree with hand-computed values. Magnitude does not
  depend on grid- vs earth-relative components, so no alpha.
- `wind_direction` (u, v -> direction from, degrees true): needs alpha
  for grid-relative winds (grid "resolution and component flags").
- Next, for the fruiting chain: `bucket_from_run_total` (run total at t
  minus at t - step, `previous_step` input), `ramp` (piecewise-linear
  0-1 score from params), `weighted_sum`; all natural kernels.

Rolling history (days since rain, event totals, rolling RH mean) uses
`own_previous` inputs: e.g. days since rain = previous value + step
hours / 24, reset to 0 where this step's precip bucket >= the threshold
param. Each is an ordinary product, drawable on the map.

## Web

- Products page: list (status of the last run per product: done / failed /
  waiting for inputs), editor (processor from `derived_processor`, inputs
  picked with the LEADS-style grid picker restricted to the shapes the
  processor accepts, outputs, params form from the processor's schema),
  Rebuild, enable/disable.
- Arrivals page: per model_grid, the newest runs with the offsets present,
  message counts and last arrival time (from the catalog), plus a product
  x offset grid of job states for the selected run; failed cells show the
  error.
- Colormap editor (independent of the above): edit segments of the
  existing `colormap` tables with a live preview.

## Questions

Q1 [decided by Chris 2026-11-21] Outputs get their own tree: centre 254
   "AtmoTensor" -> "Derived" -> one model per product.
Q2 [decided by Chris 2026-11-21] Products point at an OpenCL kernel
   wherever possible, editable on the fly; built-in C++ processors for
   the rest. No external commands. See "Processors".
Q2b [decided by Chris 2026-11-21] Kernels may use the M40 (the LLM is
   moving to a separate machine). atmo_productd's config file has an
   ordered device list, default M40, then the Intel iGPU, then CPU
   (PoCL). A job goes to the first device that can hold it (its buffers
   fit in free device memory and the program builds for that device);
   otherwise the next one is tried, and the device used is recorded on
   the job. No device at all -> the job fails with that reason (built-in
   C++ processors need no device).
Q3 [decided by Chris 2026-11-21] Separate daemon `atmo_productd`
   (ingest stays simple; products can lag or fail without holding up
   ingest; kernels and GPUs stay out of the ingest process).
Q4 [decided by Chris 2026-11-21] v1 outputs stay on their input's grid
   (keeps kernels simple: every input and output is one array on one
   grid). Terrain is resampled onto each model grid once and stored as
   a static product (offset 0). Regridding is a later, separate step.
Q5 [decided by Chris 2026-11-21] State across runs lives in the
   catalog: the `own_previous` input rule (see "Trigger"), not a state
   file. Rolling features are products like any other and can be drawn
   and debugged on the map.

## Implementation (T21, T22, 2026-11-21)

Built: include/atmo/products.h (definitions, built-ins, matcher),
products_db.h (derived_* tables), productd.h (the pass loop), opencl.h
(kernels), tools/atmo_productd.cpp (deploy/productd.json,
deploy/atmo-productd.service), grib_derive() in grib.h.  Choices the
sections above did not pin down are listed for review in QUESTIONS.md
Q25: output directory and file names, two web-owned columns set once,
input units, first start, retries, `has_prev`, alpha, template 4.0 only,
superseding on claim, statistical inputs, OpenCL timeouts.

