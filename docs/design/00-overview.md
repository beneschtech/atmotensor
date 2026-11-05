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
| D10 | Reference data | **MySQL is the authoritative, editable store for rarely-written reference data** (stations, customers/API keys, variable registry, model/product config). The engine never queries it for snapshot data: ingest exports e.g. stations as immutable generation files named by the manifest. Customer/auth data may be read directly with an in-RAM TTL cache. | SQL fits relational, hand-edited data; exporting keeps snapshots reproducible and query daemons independent of MySQL. Client library optional like eccodes (core builds without it). Decided 2026-11-05. |
| D7 | Durability | Raw feed journal fsynced + replicated off-box; sealed files immutable | Observations are the only irreplaceable data; grids are re-fetchable from NOAA. |

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
    /atmotensor  (files)  <--mmap--  [query daemon(s)]  <--FastCGI--  nginx  <--HTTPS-->  customers
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
