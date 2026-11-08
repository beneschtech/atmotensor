# AtmoTensor - project notes for Claude

Own-hardware weather data engine. Customers query it; it stores gridded model
data (GFS, RRFS, HRRR, ...) and observations (METAR/SYNOP, soundings, TAF,
PIREP, ...) and answers point, flight-route and area-product queries. Name:
AtmoTensor; C++ namespace `atmo`; store root `/opt/atmotensor/data` (tmp `/opt/atmotensor/tmp`,
feeds `/opt/atmotensor/ingest/<source>`; see deploy/README.md). C++20, Linux,
static library `libatmo` (.h declarations + src/*.cpp). Owner: Chris (C++ /
Qt / Symfony background, AIX-era storage instincts, wants to avoid
third-party/hosted dependencies).

## Coding conventions

`docs/CONVENTIONS.md` (agreed 2026-11-05) is binding and overrides older
code style: C++20; headers hold declarations only, with code in
`src/*.cpp` (exceptions: trivial getters/setters, templates, vendored
code); headers are `.h`; f32 wherever precision allows (OpenCL), with
documented exceptions; 80 columns; 2 spaces, no tabs; Allman braces
everywhere; long signatures one parameter per line. `.clang-format` encodes the layout;
run `clang-format -i` on every file you touch (never on `third_party/`).

## Build and test

CMake only (CLion); presets in CMakePresets.json: default, asan, ci.

    cmake --preset default && cmake --build --preset default -j
    ctest --preset default
    cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
    ./build-cmake/bench_kdtree; ./build-cmake/bench_grid
    # options: ATMO_SANITIZE ATMO_WERROR ATMO_BUILD_TESTS ATMO_BUILD_BENCH
    #          ATMO_WITH_ECCODES=AUTO|ON|OFF, ECCODES_ROOT=/prefix
    ./build-cmake/atmo_grib_ingest --root DIR --model gfs [--seed-sources] [--tiles] F.grib2
    ./build-cmake/atmo_queryd --root DIR --socket /run/atmo/query.sock  # behind nginx
    # deploy: deploy/README.md (nginx snippet + systemd unit); API contract: docs/api/query-api-v1.md

No Catch2/gtest: tests use the tiny harness in `tests/check.h` (TEST, CHECK,
CHECK_NEAR; non-fatal). Keep new tests dependency-free. `-ffp-contract=off` is
deliberate: tree results must stay bit-identical to the brute-force oracles.
Run the asan preset after touching anything with pointers or file I/O.

## Web app

`../web` (Symfony 7.4) owns MySQL and its schema (Doctrine migrations). The
engine never connects to MySQL: the web app exports reference data
(source classes, stations, ...) as JSON files into `/opt/atmotensor/data/config/`,
and ingest publishes them through the manifest. Contract:
`docs/design/05-reference-data.md` (D10).

## Layout

Each `include/atmo/<name>.h` (declarations) has a `src/<name>.cpp`, except
`kdtree.h` (templates only). CMake builds `libatmo` (target `atmo::atmo`)
and `atmo_testkit` (test harness + Lambert scaffolding) for tests/benches.

- `include/atmo/kdtree.h`   `KdTree4<T>` static flat-array 4D k-d tree;
  `KdTreeView4<T>` searches the same three arrays wherever they live (an
  mmap); KdTree4 delegates to it
- `include/atmo/geo.h`      `embed()` (lat, lon, height, time) -> 4D point
- `include/atmo/grid.h`     GridIdentity/GridKey, GridData, Grid
  (locate/nearest), rotation
- `include/atmo/gridstore.h` GridStore: immutable on-disk grid files
- `include/atmo/fieldfile.h` FieldFileWriter / FieldFile: per (model,
  run, fhr) float32 tiles (256 + halo 1, NaN outside, periodic halo wraps),
  lazy per-tile CRC, bilinear `sample(field, Grid::Cell)`
- `include/atmo/variable.h` VariableInfo + lookups (code, name, GRIB2);
  `include/atmo/variables.h` is GENERATED from docs/design/variables.csv
  (edit the CSV, build target `gen_variables`; codes are never reused)
- `include/atmo/grib.h` (only with eccodes, ATMO_HAVE_ECCODES) GribReader:
  section 3 identity, scan-mode normalisation, registry params + SI,
  levels (table 4.5), grid-relative u/v flag; ingest_grib() -> GridStore +
  one field file per (model, run, fhr). Ubuntu's eccodes CMake config is
  broken (missing libeccodes_f90), so it is probed in a sub-configure
  (cmake/eccodes_probe) before use; pkg-config is the usual route.
- `tools/atmo_grib_ingest.cpp` dev CLI: ingest + publish
- `include/atmo/query.h` point() engine: FieldStore interface (T7 files via
  SnapshotFieldStore; GRIB-native store later, Q10), field specs
  `name@level`, time/log-p/altitude interpolation, wind rotation, JSON
- `include/atmo/fcgi.h` in-house FastCGI responder: Connection (protocol
  state machine, no I/O), Server (poll I/O thread + worker pool, 503/504)
- `include/atmo/http_api.h` QueryApi (/healthz, /v1/meta, /v1/point,
  ?snapshot= pinning), SnapshotWatcher (inotify + poll); daemon in
  `tools/atmo_queryd.cpp`
- `include/atmo/gribstore.h` GRIB-native store (Q10, T10c): GribIndex
  (sidecar <file>.idx.json), GribCache (hash -> decoded doubles + shared
  grid, LRU purge by byte budget and /proc RSS, shared_ptr = in use,
  coalesced decodes), process_rss(); ingest/decoder in grib.h
  (index_grib, grib_decoder). Default ingest path; T7 tiles via --tiles.
- `include/atmo/crc32.h`    CRC-32 (zlib-compatible)
- `include/atmo/source.h`   SourceClass (metric per source class),
  SourceRegistry (JSON config)
- `include/atmo/obs_segment.h` ObsSegmentWriter / ObsSegment: immutable
  `*.seg` files (reports | values | raw | prebuilt tree), mmap'd, opened
  from the file without a rebuild
- `include/atmo/obs_index.h` ObsIndex: visible-version knn/radius over all
  segments of a snapshot (per-bucket version maps, time window, in-RAM
  tree rebuild on metric change)
- `include/atmo/stations.h` StationTableBuilder / StationTable: immutable
  station generations (typed ids with validity, position periods, stable
  refs); authoritative copy will live in MySQL (D10)
- `include/atmo/manifest.h` Manifest (strict JSON, relative paths per
  kind), ManifestPublisher (flock on manifest/LOCK, MANIFEST -> CURRENT.tmp
  -> rename protocol, keeps last M, cleans up crashed publishes),
  read_current_manifest()
- `include/atmo/retention.h` apply_retention(): obs buckets by
  retain_seconds, newest K runs per model, unused grids; the publisher's
  `retired` list + collect_garbage() unlink after the grace period
- `include/atmo/snapshot.h` Snapshot (every file of one manifest opened up
  front and checked against its entry, ObsIndex), SnapshotLoader
  (refresh()/current() over an atomic shared_ptr, reuses unchanged mappings)
- `include/atmo/refdata.h` StationsExport (stations.json / outbox JSON),
  RefDataImporter: config/sources.json + stations.json -> manifest (next
  station generation), provisional stations + config/outbox/ (Q7, Q8)
- `src/jsonutil.h/.cpp` internal: strict JSON field helpers (check_keys,
  get_u64/..., check_format, parse_json); all errors std::invalid_argument
- `src/fileio.h/.cpp` internal: TempFile (write in place, then
  fsync+link+dir fsync), publish_file, MappedFile, align_up; shared by every immutable-file writer/reader
- `third_party/`  vendored single headers (nlohmann/json 3.11.3, MIT); see
  its README.md. Included as `<nlohmann/json.hpp>` via a SYSTEM include
  path. Don't edit or reformat vendored files.
- `tests/`  harness (`check.h/.cpp`), brute-force oracles,
  `lambert.h/.cpp` (synthetic projected grid; test scaffolding only)
- `bench/`  kdtree and gridstore timings

## Design decisions (settled - don't relitigate without a reason)

**One 4D metric, per-source time weight.** `embed()` gives x,y,z in km as a 3D
vector at radius R + vertical_scale*height (no dateline/pole problems; chord is
monotonic in great-circle distance) and w = km_per_second*(t - t0). The time
weight (lambda) differs per source (METAR ~1 h, sounding ~6-12 h, PIREP ~1-2 h),
so build ONE TREE PER SOURCE CLASS; never mix lambdas in one tree. Time is
stored relative to a per-tree t0 (float epoch seconds lose 128 s resolution).
TAFs / SIGMETs and anything with a validity interval or spatial extent are NOT
points: they belong in an interval/R-tree structure, not KdTree4.

**KdTree4 contract.** Built once, immutable. Implicit balanced layout over flat
arrays (`points()`, `ids()`, `axes()`), leaf size 8, split on widest axis.
Results are ordered by (d2, id); build breaks ties by id. Pruning uses
`d*d <= bound` (not `<`) so equal-distance candidates with lower ids are found.
Queries must equal a scan using `KdTree4::dist2` exactly. No pointers, so the
arrays can be uploaded to OpenCL buffers as-is.

**Grids are index space + coordinate arrays.** Every grid is a regular (i, j)
array (index = j*ni + i, i fastest, j increasing); projection is just the
lat/lon arrays. Ingest must normalise GRIB scan modes to this order for data
AND coordinates. Identity = canonical bytes (for GRIB2: section 3 template
octets) + ni, nj, periodic_i, hashed with FNV-1a to a 64-bit GridKey; the full
bytes are verified on load, so collisions can't alias grids. All fields on the
same grid share one definition. Only structured grids; unstructured grids
(reduced Gaussian, cubed sphere) need their own class.

**Rotation.** alpha = angle of the grid +i axis CCW from local east, derived
from the coordinate arrays (centred difference), so no PROJ is needed.
`rotate_to_earth(alpha,u,v)` converts grid-relative winds. Check the GRIB
"resolution and component flags" before rotating: u/v may already be
earth-relative.

**locate() accuracy.** Nearest point via a lazily built KdTree4, then inverse
bilinear solve of the surrounding cells in the query's tangent plane. Cells are
treated as flat quads: fractions are off by up to ~1e-3 of a cell for 1 degree
cells at mid latitudes, scaling with cell size (~1e-4 at 0.25 deg, ~1e-5 at
3 km). Degenerate pole cells are handled by verifying the solve's residual
(the quadratic has a spurious root at the shared corner). A regular lat/lon
fast path with exact arithmetic is a possible later optimisation.

**Storage.** Grid and obs data are write-once, so: immutable checksummed files,
temp file + fsync + rename + dir fsync, idempotent put, mmap on read.
Preallocated files on XFS (O_DIRECT / io_uring) to start; raw block device only
if profiling demands it - the layout is identical either way. Grids are
re-fetchable from NOAA; observations are the irreplaceable part (replicate
sealed segments off-box). Tiles are 4 KB-aligned. Keep hot-tile LRU in RAM and
coalesce identical in-flight requests.

## How we work (two Claudes)

Design happens in the desktop-app session (Chris + architect Claude); this
terminal session does the implementation. Specs: `docs/design/*.md` (start with
`00-overview.md`, decisions D1-D7). Work queue: `docs/TASKS.md` (do tasks in
order, tests first, ASan, no commits unless asked). Anything ambiguous or
apparently wrong in a spec goes in `docs/QUESTIONS.md` - do not invent
architecture; park the task and take another. Keep CLAUDE.md in sync when a
task adds a convention.

Decisions so far (see 00-overview.md): latest-only retention (newest K runs,
72 h obs), HTTP/JSON + binary tiles served by a FastCGI responder behind nginx
(nginx owns TLS/auth/limits), separate ingest and query daemons over immutable
files + atomic manifest, corrections keep history (latest wins), raw feed
journal for durability, eccodes from the distro package (hand-built via
-DECCODES_ROOT must also work; core must build without it).

## Working notes

- The Claude desktop app's linked shell is a sandboxed VM: it may lack CMake and
  cannot delete files in connected folders. On the real workstation neither applies.
  (The old plain Makefile existed for that VM; it was removed 2026-11-05.)
- Benchmarks (2 cores, -O2): 2M-point KdTree4 builds in ~0.45 s, ~1.2 us
  nearest, ~3 us 8-NN; HRRR-size grid: write 0.42 s, load+verify 0.07 s,
  ~1.4 us/locate. GFS 0.25 f000 (696 messages, 506 MB GRIB2):
  atmo_grib_ingest 13.8 s, 108 MB RSS, 485 fields -> 2.2 GB field file
  (uncompressed f32 tiles; field selection per model matters, 03 spec).
- Do not commit or push unless asked.
- Conventions from T1/T2: JSON text entry points are `from_text()` / `from_file()`
  (a `from_json(string_view)` overload would be ambiguous with `json`); config errors
  are `std::invalid_argument`, file corruption is `std::runtime_error`. On-disk readers
  recompute every offset from the counts, check every CRC, and run structural checks
  (slices in range, tree ids a permutation) even with valid CRCs. Sealed files are
  published with link() (never replaces an existing file). Each new test file must be
  added to the `foreach` in CMakeLists.txt.
- Vertical position is always a geometric height in metres above MSL (reports and
  each profile value alike), never a level index or geopotential height; ingest
  converts (Q3).
