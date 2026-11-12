# AtmoTensor architecture overview

Status: draft v1, written by the architect session (Chris + Claude in the
desktop app). Implementation sessions: build to these specs; if one is
ambiguous or wrong, STOP and add an entry to `docs/QUESTIONS.md` instead of
deciding silently.

## Decisions log

| # | Decision | Chosen | Why |
|---|----------|--------|-----|
| D1 | Retention | **Latest only**: newest K runs per model, N hours of obs. No cold tier. | Simplest; everything stays on NVMe; expiry = dropping whole files. Defaults: obs 72 h, K=4 runs/model (all configurable). |
| D2 | Customer API | **HTTP/JSON + binary tiles** | Works from anywhere, easy to debug. See `04-query-api.md`. |
| D8 | Serving | **FastCGI responder behind nginx**, unix socket; no TLS in the daemon | nginx handles TLS, auth, connection/rate limits, compression and caching; the daemon stays small. Small in-house FastCGI implementation, libfcgi as fallback. |
| D9 | eccodes | **Distro package** (EPEL `eccodes`/`eccodes-devel` on Alma; `libeccodes-dev` on Debian/Ubuntu); build must also accept `-DECCODES_ROOT=` for a hand-built copy | Matches how the servers are managed; hand-built stays possible. Core library and tests must build without eccodes. |
| D3 | Processes | **Separate ingest daemon and query daemon(s) sharing files** | Write-once data: ingest writes immutable files and publishes a manifest; query processes mmap them. See `02-manifest-and-processes.md`. |
| D4 | Corrections | **Latest version wins by default, older versions kept** (within retention) | Queries see the newest version; `as_of` can reproduce an earlier answer. See `01-obs-store.md`. |
| D5 | Time weighting | Per-source-class lambda, one k-d tree per source class | Settled earlier; see CLAUDE.md. |
| D6 | Storage substrate | Immutable files on XFS first; raw block device only if profiling demands | Settled earlier; layout identical either way. |
| D10 | Reference data | **MySQL, owned by the Symfony web app (`../web`, Doctrine migrations), is the authoritative store for rarely-written reference data**: source-class config, stations, customers/API keys, later the variable registry and model/product config. **Only the web app talks to MySQL**: it exports files to `/opt/atmotensor/data/config/`, which ingest validates and publishes through the manifest. nginx `auth_request` asks the web app to check API keys. See `05-reference-data.md`. | SQL fits relational, hand-edited data; exporting keeps snapshots reproducible, the engine free of a MySQL dependency, and queries independent of MySQL being up. Decided 2026-11-05. |
| D7 | Durability | Raw feed journal fsynced + replicated off-box; sealed files immutable | Observations are the only irreplaceable data; grids are re-fetchable from NOAA. |
| D13 | Grid catalog | **Gridded data is catalogued in MySQL, and the engine reads and writes those tables directly** (schema still owned by the web app's Doctrine migrations). Every GRIB message is identified and reachable by id; hierarchy as in LEADS: centre, subcentre, model (generating process), grid, base time (+ ensemble member), parameter, level, forecast offset. Parameter identity is a hash of the header keys that say what the field is. Overrides D10 for grids only; replaces the idx.json sidecars, the manifest `gribs` list and the variable registry as the ingest gate. Observations are unchanged. See `07-grib-catalog.md`. | Keeping the catalog out of a database was costing more work and performance than the dependency; LEADS users expect this selector. Decided 2026-11-11. |
| D11 | Rendered products | **No customer code runs on our servers.** Images are made through an API to create and fetch products: the engine renders the base image and returns its colour scale (colormap, value range, units), projection and bounds as data; a declarative JSON template (colour bar, logo, title, timestamps, attribution) handles the common layouts. Anything more unusual the customer does on their own machine (e.g. Pillow), helped by a client package that fetches data and colour scales. | A scripting language at the centre of a product becomes a pain point (the incumbent's experience); sandboxing customer Python is a large security and cost burden. Decided 2026-11-06; details after T10. |
| D12 | Derived products | **Ingest derives and stores additional products from what arrives**, as ordinary immutable field files with their own variable codes: a registry of derivations (inputs, algorithm, version) runs each one when all its inputs for a run/valid time are present. Simple ones first (wind speed/direction from u/v), complex ones later (EDR, spatial front detection). Outputs record their inputs and algorithm version so they can be rebuilt. Features that are lines or areas (fronts) are not grid fields: they go in a feature store (interval/R-tree, like TAF/SIGMET), not KdTree4. | Queries read precomputed results at the same cost as raw fields; derivations are deterministic and re-runnable because inputs are re-fetchable. Decided 2026-11-06; design in T11b. |

Defaults chosen by the architect (not asked, cheap to change): config as JSON
(vendored single-header parser), field tiles 256x256 float32 with 1-cell halo,
canonical SI units and an internal variable registry (not GRIB codes).

## Components

    feeds (NOAA GRIB, METAR/SYNOP/..., PIREP, soundings)
        |
    [ingest daemon]  --journal-->  raw journal (fsync, replicated)
        |  decode (eccodes / text decoders)
        |  write immutable files: grids, field files, obs segments, stations
        |  publish manifest (atomic)
        v
    /opt/atmotensor/data (files)  <--mmap--  [query daemon(s)]  <--FastCGI--  nginx  <--HTTPS-->  customers
                                         |
                                  Snapshot (pinned per request)

Libraries (header-only today, may gain .cpp files): `atmo::` kdtree, geo, grid,
gridstore (done); obs segment/index, stations, manifest/snapshot, field files,
query engine (to do); daemons are thin executables over the libraries.

## Query model

One query layer, two engines. Every request pins a Snapshot (consistent view
of all files at manifest sequence N) and returns `X-Atmo-Snapshot: N`.
- Point: grid index + interpolation (space bilinear, time linear, vertical in
  log-p) and k-d search of observations.
- Flight route: sampled space-time trajectory (lat, lon, altitude, time per
  sample); grid lookups are point lookups; obs via corridor search per time bucket.
- Area product: computation over grid tiles, cached by
  (product, model run, valid time, tile).

## Invariants (do not break)

1. Files are immutable once published. Never modify in place.
2. A file is visible only if a published manifest names it.
3. Queries are deterministic for a given snapshot: ordering by (distance, id),
   no atomics-order dependence, fixed-shape reductions.
4. Units inside the engine are SI; conversion happens at the API edge.
5. Every on-disk section is CRC-protected and 4 KiB aligned.
