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
- [x] T1  Source classes + config. `include/atmo/source.h`: `SourceClass`
      {name, bucket_seconds, L_km, tau_seconds, vertical_scale, retain_seconds},
      `lambda()` = L_km / tau_seconds, `Embedding make_embedding(bucket_start)`,
      JSON load/validate. Vendor a single-header JSON parser under
      `third_party/` (nlohmann/json, MIT) and note it in CLAUDE.md. Tests:
      defaults from 01-obs-store.md, bad config rejected (non-positive tau,
      duplicate names).
- [x] T2  Obs segment write/read (01-obs-store.md): `ObsSegmentWriter`
      (add_report(...), seal(path)), `ObsSegment::open(path)` with mmap, CRCs,
      prebuilt tree. Acceptance tests 1, 2, 5, 6.
- [x] T3  Station table (append-only generations, validity intervals, stable
      refs). Tests: round trip, ref stability across generations, lookup by
      id and by (id, time).
- [x] T4  `ObsIndex`: visible-version knn/radius across segments and
      buckets (01-obs-store.md); visibility from a per-bucket version map. Acceptance tests 3, 4; plus a randomized test
      against a brute-force oracle that implements "latest version wins".
      Segments whose stored metric differs from the current source class get
      their tree rebuilt in RAM (Q5b, 05-reference-data.md); test it.
- [x] T5  Manifest + snapshot + publisher lock (02-...md). Acceptance tests
      1-4. Use fork() or a second process for the lock test.
      Done: `manifest.h` (Manifest JSON, ManifestPublisher, CURRENT
      readers), `snapshot.h` (Snapshot, SnapshotLoader). Deferred to T10:
      inotify watch of CURRENT, `?snapshot=<seq>` pinning. Field entries
      are carried but not opened until T7.
- [x] T5b Reference-data import (05-reference-data.md): watch config/,
      validate sources.json and stations.json, keep the previous version on
      error, publish through the manifest (source classes copied into it;
      stations as the next generation). Provisional-station write-back after
      Q7 is decided.
      Done: `refdata.h` (StationsExport JSON, RefDataImporter::poll() with
      change detection by inode/size/mtime, add_provisional() + outbox).
      Policy points in Q8. Deferred to the ingest daemon: inotify on
      config/ (poll() is what it calls).
- [x] T6  Retention + gc (expire buckets/runs, grace-period unlink,
      `gc()` of unreferenced files). Tests with fake clocks.
      Done: `retention.h` (apply_retention, RetentionPolicy), manifest
      `retired` list maintained by publish(), segment entries carry
      `bucket_seconds`, `ManifestPublisher::collect_garbage()` and
      `set_clock()`. See 02-manifest-and-processes.md "Retirement".
- [x] T7  Field file write/read (03-field-tiles.md) + `sample()` bilinear
      including periodic halo. Acceptance tests 1-4.
      Done: `fieldfile.h` (FieldFileWriter streams tiles, FieldFile with
      lazy tile CRCs, sample/value/tile/require_grid); snapshots now open
      field files (FieldEntry::from_file, Snapshot::field_file).
- [ ] T8  Raw journal + replay (02-...md acceptance test 5), group commit.
- [x] T8b Variable registry (Q4): checked-in registry file, generator for
      `include/atmo/variables.h` (constants + names + SI units), ~25
      surface/upper-air variables to start; codes never reused.
      Done (before T8, which only obs feeds need): docs/design/variables.csv
      (34 variables with GRIB2 discipline/category/number + scale/offset to
      SI), cmake/gen_variables.cmake (pure CMake), `gen_variables` target,
      `variables_header` staleness test, lookups in `variable.h`.
- [x] T9  Query engine library: `point()` combining Grid::locate, FieldFile
      sample (space, time, log-p), ObsIndex; unit tests on synthetic data.
      Done: `query.h` (FieldStore interface + SnapshotFieldStore, FieldSpec,
      point(), to_json, meta_json). Storage-agnostic for Q10. API details:
      Q11.
- [x] T10 FastCGI responder library (`include/atmo/fcgi.h`, spec and tests
      in 04-query-api.md "FastCGI front end"), then the query daemon skeleton:
      /healthz, /v1/meta, /v1/point over T9. Includes (from T5): watch
      manifest/CURRENT with inotify + 1 s polling fallback calling
      SnapshotLoader::refresh(), and `?snapshot=<seq>` pinning of a
      retained snapshot. Can start independently of T2-T9
      using a stub handler. Provide an example nginx config under `deploy/`.
      Done: `fcgi.h` (Connection state machine, Server: poll I/O thread +
      worker pool, 503 on a full queue, 504 past the queue deadline),
      `http_api.h` (QueryApi: /healthz, /v1/meta, /v1/point, ?snapshot=
      pinning with an LRU of opened snapshots, SnapshotWatcher: inotify +
      1 s poll), `tools/atmo_queryd`, `deploy/nginx-atmo.conf`. Tested
      through a real nginx (test_fcgi) and on the GFS 0.25 store (0.13 ms
      per point query through nginx). Not yet: /v1/route, /v1/tiles,
      /v1/obs, ?units=, SIGHUP config reload (no config file yet).
- [x] T11 Ingest: GRIB2 via eccodes -> GridStore + field files. Identity bytes
      = GRIB2 section 3 octets; normalise scan modes; convert to SI. eccodes
      comes from the distro package (D9). CMake: auto-detect (CONFIG package,
      then pkg-config, then find_path/find_library), honour
      `-DECCODES_ROOT=/path` (and CMAKE_PREFIX_PATH) for a hand-built copy,
      expose `ATMO_WITH_ECCODES=AUTO|ON|OFF`; with it off/absent, everything
      else (including all existing tests) must still build and pass. Test
      GRIB fixtures: generate tiny GRIB2 files with eccodes itself in a test
      helper rather than checking in large binaries.
      Done: `grib.h` (GribReader, ingest_grib, add_to_manifest),
      `tools/atmo_grib_ingest` (decode + publish, dev tool until the ingest
      daemon). Ubuntu's eccodes CMake config is broken (references the
      missing libeccodes_f90.so.0), so the config package is probed in a
      sub-configure (cmake/eccodes_probe) and pkg-config is used there.
      Message selection policy: Q9.
- [x] T10b PNG map tiles + colormaps (Q12): /v1/tiles per
      docs/api/query-api-v1.md; colormaps travel in the request (Q12).
      Done: `colormap.h`, `png.h` (zlib optional), `tiles.h` (lattice
      locate + block interpolation, seam/edge fallbacks), FieldStore::view,
      route with ETag/304. GFS 0.25: ~6 ms per warm tile.
- [x] T10c GRIB-native gridded store (Q10): index of messages in the
      original GRIB2 files + decoded-message cache (Chris's design in Q10);
      implements FieldStore so T9/T10 need no change. Design review first.
      Done: `gribstore.h` (GribIndex, GribCache, process_rss), grib.h
      index_grib/grib_decoder, manifest `gribs`, snapshot index loading,
      SnapshotFieldStore over GRIB, atmo_grib_ingest native by default,
      atmo_queryd --cache-mb/--rss-limit-mb. Details and numbers in Q10.
- [ ] T11b Derived products (D12): superseded by 06-derived-products.md
      (decided 2026-11-21, catalog-based) and tasks T20-T23 below.
- [x] T14 Grid catalog schema (D13, 07-grib-catalog.md "Tables"), in
      `../web`: Doctrine entities + ONE migration for the `grib_*` tables,
      indexes for the drill-down queries and for `ingest_seq`, the
      `grib_catalog_state` row with `schema_version` 1. Web-side tests that
      the migration applies and rolls back on MySQL (Q14). No UI yet.
      Done 2026-11-11: `src/Entity/Grib/*`, migration
      Version20261111120000, `tests/php/grib_catalog_migration_test.php`
      (needs ATMO_TEST_DATABASE_URL to a `*_test` database). Tested on
      MySQL 8.4 (production is MySQL, Q14).
- [x] T15 Signature (07 "Parameter identity"): `include/atmo/gribsig.h`
      building canonical text, hash, base_hash, stat interval and ensemble
      member from an eccodes handle (deny list as data, versioned), plus
      `tools/atmo_grib_sigcheck` (several files in, report of signatures
      that are not stable across runs / forecast hours with the differing
      keys). Tests with generated GRIB2 fixtures: templates 4.0, 4.8
      (bucket vs since-run-start), 4.1/4.11 (members), 4.48 (aerosol).
      Then run it on the GFS, HRRR and RRFS files under
      /opt/atmotensor/data/grib and record the result in QUESTIONS.md
      before T16.
      Done 2026-11-11: no instability found; results and open points in
      Q13 (GFS f000 is process 81, eccodes key names, deny-list
      additions).
- [x] T16 Catalog library + ingest: `atmo_catalog` target on a MySQL
      client library (`ATMO_WITH_MYSQL=AUTO|ON|OFF`, core builds without it),
      connection config file, schema_version check, upserts, one
      transaction per file, generation bump, latest-wins duplicates,
      unknown parameters and unusable grids catalogued with flags.
      `atmo_grib_ingest` writes the catalog instead of idx.json. Tests
      against a throwaway MySQL (skipped when none is available).
      Done 2026-11-11: `catalog.h` (+ `src/catalog.cpp` core,
      `src/catalog_mysql.cpp` = atmo_catalog), `catalog_grib()` in grib.h,
      `atmo_grib_ingest --catalog CONFIG` (old idx.json path kept as the
      default until T17/T18), `tests/test_catalog.cpp` +
      `tests/catalog_testdb.sh`. Real data: GFS 0.25 f000 catalogued in
      1.9 s; HRRR nat + RRFS 2dfld/prslev all messages, 0 flagged.
      Built and tested on MySQL 8.4 / libmysqlclient (Q14); placeholder
      grid rows for unusable grids (Q15).
- [x] T17 Query side: catalog cache in `atmo_queryd` (reload on
      generation change, LRU of message rows), `/v1/grib/*` endpoints,
      point and tiles by message id, first-bucket repair for fixed-length
      statistical parameters, generation as the grid data version. Update
      docs/api/query-api-v1.md first. Existing /v1/point and /v1/tiles
      resolve through the catalog.
      2026-11-11: all but the last sentence done (`grib_api.h` GribApi +
      CatalogQuery, CatalogReader in catalog.h, `atmo_queryd --catalog`,
      tests/test_grib_api.cpp without a DB + a reader test in
      test_catalog). API section is Draft pending review (Q17a). Parked:
      /v1/point and /v1/tiles through the catalog (Q17c -> decided in
      Q19/Q20, done in T17c). Default names
      from table 4.2 parameterName (Q17b, done 2026-11-11).
- [x] T17b Catalog v2 (Q18, Q19, Q21): second migration in ../web
      (grib_model_alias, grib_common_name, grib_model_grid.api_name
      unique, grib_message.process_id, grib_grid.viewable replacing the
      summary flag); signature version 2 denying the model-version date
      keys; alias applied at ingest + `atmo_grib_ingest --realias`
      (SQL move, Q21d); default api_name with _2.. on clashes; web
      command seeding grib_common_name from variables.csv incl. the SI
      scale onto default parameters (Q21a); eccodes pinned in
      deploy/README.md (Q18c).
      Done 2026-11-11: signature v2 (model version date denied; sigcheck
      on the real files unchanged: 0 unstable, 0 collisions); web
      migration Version20261111180000 (schema_version 2, back-fills,
      api_name CHECK) + entities GribModelAlias/GribCommonName, tested in
      tests/php/grib_catalog_migration_test.php; kCatalogSchemaVersion 2;
      ingest applies aliases, writes process_id/viewable/api_name
      (catalog_api_name(), _2.. on clashes); Catalog::realias() +
      `atmo_grib_ingest --catalog CONFIG --realias` (moves both ways,
      latest wins, cleans up empty rows; real GFS f000 moved under the
      96 model); `app:grib:seed-common-names` (tests/php/
      grib_common_names_test.php); eccodes hold in deploy/README.md (not
      run on this box: needs sudo).
- [x] T17c Query side by name (Q19, Q20, Q21): /v1/point and /v1/tiles
      resolve model (api_name) and field (common name / normalised
      parameter name) through the catalog, newest run with the field
      (Q21c), obs still on the registry (Q21b); exact / best-guess /
      ambiguous matching with warnings; /v1/meta lists api_names;
      GET /v1/grib/tree; raw data: /v1/grib/messages/{id}/raw and
      /values, /v1/grib/grids/{id} (Q21g). API doc first.
      Done 2026-11-11: FieldStore resolves names (resolve_field /
      resolve_model / param(id) with ParamRole; registry defaults keep the
      snapshot path unchanged); CatalogFieldStore in grib_api.h
      (GribApi::field_store(), one per generation), CatalogQuery
      run_offsets / messages_at, common names + parameter_id in
      CatalogTables; NameError -> 400 ambiguous_name (+candidates) /
      ambiguous_field, 404 not_found; warnings in JSON and
      X-Atmo-Warning; /v1/grib/tree, /grids/{id}, /messages/{id}/raw and
      /values (grib_values() in grib.h); web migration
      Version20261111200000 (index run_id, offset_end_s). Real data: T850
      at OKC 282.63153 K as before, HRRR rotated winds identical to the
      old path, tiles 13-32 ms, values of a GFS message 15 ms. Decisions
      for review: Q22.
- [x] T18 Remove what D13 replaced once T16/T17 pass on real data:
      idx.json sidecars, manifest `gribs`, GRIB handling in retention
      (retention deletes runs from the catalog, then the files).
      Done 2026-11-11: GribIndex / index_grib / GribEntry / Snapshot
      grib_indexes / SnapshotFieldStore GRIB slices removed (field files
      only); manifest version 2 (version 1 read: sidecars retired, GRIB
      files left alone; checked read-only on /opt/atmotensor/data);
      Catalog::expire() + `atmo_grib_ingest --expire [--keep K]
      [--keep-model NAME=K]`; gc no longer sweeps grids/ or grib/ and
      manifest retention no longer expires grids (Q23); tests ported
      (catalog vs field files cross-check kept, FakeCatalog shared in
      tests/fake_catalog.h). atmo_grib_ingest without --catalog/--tiles is
      an error; --seed-sources publishes on its own.
- [~] T19 Web (`../web`): LEADS-style grid selector (tree + Parameters /
      Levels / Forecast Times lists) creating layers from message ids;
      page of undefined signatures with an editor; `app:eccodes:export`
      writing local concept files; names for models and grids.
      Done 2026-11-11 (desktop session): web client on api_names + exact
      names (a layer the engine would have to guess at shows a problem
      instead of drawing), field units from the catalog (EngineFields:
      common name -> parameter, normalised name, variables.csv last),
      "Grid selection..." window (assets/controllers/grid_picker_controller.js)
      over /v1/grib/tree, /parameters, /levels, /offsets; Add creates or
      reuses an engine_raster / engine_wind definition by api_name +
      field@level (POST /api/engine-layers), so layers keep newest-run,
      hour stepping, point values and export. Not by message id: by name
      covers those features; what by-name cannot reach (members, grids not
      drawable natively, sub-hourly offsets, two layers on one first
      surface) is listed with the reason (Q24). `app:engine:layers
      --models / --rename-model gfs=gfs_0.25p` moves the old definitions;
      export cache key includes grib_catalog_state.generation for images
      with engine layers. Tests: tests/php/engine_layers_test.php,
      tests/js/grib_names.test.js; all web tests pass (PHP 8.4,
      MySQL 8.0); picker driven in headless Chrome against a fake engine.
      Still to do: undefined-signature page + editor, app:eccodes:export,
      naming page for models / grids (name, api_name) and common names.
- [x] T20 Products schema (06-derived-products.md "Product definitions",
      "Jobs", "Processors"), in `../web`.
      Done 2026-11-21 (desktop session): migration Version20261121200000,
      entities src/Entity/Derived: derived_product, derived_product_input,
      derived_product_output, derived_processor, derived_kernel,
      derived_kernel_revision, derived_job, derived_state (prefix because
      `product` = COG files already). derived_state.schema_version = 1 is
      what atmo_productd checks; grib_catalog_state.schema_version stays 3
      (no engine change for ingest/queryd). CHECKs on input shape, kind,
      process_id 1..254, slug. Centre 254 "AtmoTensor" / subcentre 0
      "Derived" seeded. `app:derived:seed [--example-wind gfs_0.25p]`
      seeds the wind_speed kernel (resources/kernels/wind_speed.cl,
      source_hash = FNV-1a 64 of the source, same function as GridKey) and
      a "10 m wind speed" product (u wind_u@10m, v wind_v@10m -> 0/2/1 at
      103/10 m). tests/php/derived_migration_test.php (32 checks, MySQL
      8.0); grib_catalog_migration_test.php now stops at schema 3 before
      rolling the catalog back (derived_job references grib_run).
- [x] T21 atmo_productd skeleton: config file (MySQL like
      catalog.json, OpenCL device list, workers, job timeout), arrival
      monitor (generation poll, ingest_seq cursor in derived_state.cursor_seq),
      matcher (exact by-name resolution of product_input per
      generation; offset / previous_step / own_previous rules),
      idempotent job creation, claim by conditional UPDATE, retry once,
      requeue stale running jobs, superseded on replaced inputs.
      Built-in processors only (wind_speed, wind_direction) written to
      derived_processor at start; refuse to start unless
      derived_state.schema_version == 2; heartbeat_at (UTC) every poll;
      honour derived_product.rebuild_requested_at (06 "Jobs": match
      against every retained run, compare-and-clear); compile kernels
      whose revision != built_revision (T22). Output: GRIB2 by cloning the first
      input (centre 254, process_id, parameter + levels from
      product_output, simple packing, NaN bitmap), linked under
      grib/<slug>/<run>/ and catalogued through Catalog::ingest.
      Tests: matcher against FakeCatalog-style fixtures, wind speed vs
      hand-computed values, output GRIB2 round-trip through
      describe_grib(), a chain (product B on product A) triggers.
- [x] T22 OpenCL kernels in atmo_productd: kernel ABI as in
      "Processors"; compile on revision change, program cache per
      (source_hash, device), build_status/build_log written back;
      device list with fall-through when a job does not fit (record the
      device on the job); watchdog timeout. The wind_speed kernel must
      match the built-in bit-for-bit within float rounding (CHECK_NEAR).
      OpenCL is optional at build time (ATMO_WITH_OPENCL=AUTO|ON|OFF);
      without it kernel products fail with a clear reason.
      T21 + T22 done 2026-11-21 (desktop session, at Chris's request):
      products.h (core + matcher + built-ins), products_db.h /
      src/products_mysql.cpp, productd.h / src/productd.cpp, opencl.h /
      src/opencl.cpp, tools/atmo_productd.cpp, grib_derive() in grib.h,
      CatalogFieldStore::signatures_of().  Tests: test_products (core,
      grib_derive, matcher over an in-memory catalog), test_opencl (seed
      kernel == built-in, build-time argument checks, params/alpha, device
      preference; 77 without a device), test_productd (end to end on the
      throwaway DB: rebuild requests, arrivals, chains, kernel revisions,
      broken kernel, retries, replaced inputs, edits, stale jobs; 77
      without ATMO_TEST_CATALOG).  ASan clean.  Notes for review: Q25.
- [x] T23 Web: products page (list, editor with processor / kernel,
      inputs via the grid picker, outputs, params form from the
      processor schema, Rebuild, enable), kernel editor (admin, source +
      build log, revision history), arrivals page (catalog per
      model_grid: runs, offsets, counts, last arrival; product x offset
      job grid), colormap editor over the existing colormap API.
      Cycle check on save. (Desktop session.)
      Arrivals page done 2026-11-21: /arrivals (+ nav link), GET
      /api/arrivals[?runs=N&fp=], /api/arrivals/runs/{id}, /api/arrivals/
      jobs/{id}; ArrivalsService reads grib_* and derived_* (works before
      the derived migration too); per model grid the newest N base times,
      a strip of forecast hours (gaps marked, fresh arrivals pulse), and
      per run the newest job per (product, hour); daemon status from
      derived_state.heartbeat_at (stale after 30 s: atmo_productd should
      write it every poll, UTC). Polls every 10 s with a fingerprint
      (generation + job digest + daemon state). Tests:
      tests/php/arrivals_test.php (fixture in arrivals_fixture.php),
      tests/js/arrivals.test.js.
      Products / kernels editor done 2026-11-21: /derived (+ nav link):
      product list and editor (kernel or built-in, params from the
      declared interface, inputs via the grid picker in pick mode or
      typed api_name + field@level, rules same / previous_step /
      own_previous, outputs with level presets, live server check,
      Save / Save & rebuild / Rebuild / Delete), kernel editor (live
      required signature, skeleton, history, build status/log, used
      by). API /api/derived/{products,kernels,processors,jobs/{id}/retry}
      with validation against the interface, chain-loop check, revision
      bump only for output-affecting edits, kernel interface changes
      refused if they break a product. Kernel writes need
      DERIVED_KERNEL_EDIT=1 (admin switch until accounts). New column
      derived_product.rebuild_requested_at (migration
      Version20261121220000, derived schema 2): see "Jobs" in 06 and
      T21. Tests: tests/php/derived_api_test.php, tests/js/derived.test.js.
      Colormap editor done 2026-11-21: /colormaps (+ nav link) over the
      existing /api/colormaps: globals read-only with "Make a copy",
      contiguous segments by construction (only the first start and the
      ends are typed), colour + hex + opacity per end, blend per segment,
      split / remove / add / reverse / stretch, import of engine cmap
      text (colour jumps folded back), colour bar with boundaries and
      hover readout, sample field; ramp maths as the engine
      (assets/lib/colormap_edit.js, tests/js/colormap_edit.test.js).
      T23 web side complete; the job grid and build status fill in once
      T21/T22 run.
- [ ] T12 Route sampling + corridor obs (moved here from T4): design the
      corridor query with selectable strategies for using the route points.
- [ ] T13 OpenCL ABI: flat-buffer views of Grid / FieldFile tile tables / obs
      CSR bucket grids, and `grid_sample()` / `obs_neighbors()` device
      functions. Design review with Chris first.
