# Implementation backlog

Order matters: later tasks use earlier ones. Specs live in `docs/design/`.
For each task: write the tests from the spec's acceptance list FIRST, make them
pass, run ctest with the default and asan presets, follow docs/CONVENTIONS.md
(declarations in include/atmo, code in src, `atmo::`, no new dependencies
unless a spec names one). Mutation
check: temporarily break the core logic and confirm a test fails. Do not commit
unless Chris asks. If the spec is ambiguous or seems wrong, add an entry to
`docs/QUESTIONS.md` and move to another task.

Status legend: [ ] todo, [~] in progress, [x] done.

- [x] T0  KdTree4, embed(), Grid, GridStore (see CLAUDE.md)
- [x] T1  Source classes + config. `include/atmo/source.hpp`: `SourceClass`
      {name, bucket_seconds, L_km, tau_seconds, vertical_scale, retain_seconds},
      `lambda()` = L_km / tau_seconds, `Embedding make_embedding(bucket_start)`,
      JSON load/validate. Vendor a single-header JSON parser under
      `third_party/` (nlohmann/json, MIT) and note it in CLAUDE.md. Tests:
      defaults from 01-obs-store.md, bad config rejected (non-positive tau,
      duplicate names).
- [x] T2  Obs segment write/read (01-obs-store.md): `ObsSegmentWriter`
      (add_report(...), seal(path)), `ObsSegment::open(path)` with mmap, CRCs,
      prebuilt tree. Acceptance tests 1, 2, 5, 6.
- [ ] T3  Station table (append-only generations, validity intervals, stable
      refs). Tests: round trip, ref stability across generations, lookup by
      id and by (id, time).
- [ ] T4  `ObsIndex`: visible-version knn/radius/corridor across segments and
      buckets (01-obs-store.md). Acceptance tests 3, 4; plus a randomized test
      against a brute-force oracle that implements "latest version wins".
- [ ] T5  Manifest + snapshot + publisher lock (02-...md). Acceptance tests
      1-4. Use fork() or a second process for the lock test.
- [ ] T6  Retention + gc (expire buckets/runs, grace-period unlink,
      `gc()` of unreferenced files). Tests with fake clocks.
- [ ] T7  Field file write/read (03-field-tiles.md) + `sample()` bilinear
      including periodic halo. Acceptance tests 1-4.
- [ ] T8  Raw journal + replay (02-...md acceptance test 5), group commit.
- [ ] T9  Query engine library: `point()` combining Grid::locate, FieldFile
      sample (space, time, log-p), ObsIndex; unit tests on synthetic data.
- [ ] T10 FastCGI responder library (`include/atmo/fcgi.hpp`, spec and tests
      in 04-query-api.md "FastCGI front end"), then the query daemon skeleton:
      /healthz, /v1/meta, /v1/point over T9. Can start independently of T2-T9
      using a stub handler. Provide an example nginx config under `deploy/`.
- [ ] T11 Ingest: GRIB2 via eccodes -> GridStore + field files. Identity bytes
      = GRIB2 section 3 octets; normalise scan modes; convert to SI. eccodes
      comes from the distro package (D9). CMake: auto-detect (CONFIG package,
      then pkg-config, then find_path/find_library), honour
      `-DECCODES_ROOT=/path` (and CMAKE_PREFIX_PATH) for a hand-built copy,
      expose `ATMO_WITH_ECCODES=AUTO|ON|OFF`; with it off/absent, everything
      else (including all existing tests) must still build and pass. Test
      GRIB fixtures: generate tiny GRIB2 files with eccodes itself in a test
      helper rather than checking in large binaries.
- [ ] T12 Route sampling + corridor obs.
- [ ] T13 OpenCL ABI: flat-buffer views of Grid / FieldFile tile tables / obs
      CSR bucket grids, and `grid_sample()` / `obs_neighbors()` device
      functions. Design review with Chris first.
