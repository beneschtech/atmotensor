# Open questions (implementation -> architect)

Format: `Qn [status] question`, then what you tried, what you recommend, and
what you did meanwhile. The architect (Chris, in the desktop-app session)
answers by editing the entry to `[answered]` with the decision, and copies
durable decisions into docs/design/00-overview.md.

Q1 [answered] HTTP serving: FastCGI behind nginx (nginx does TLS and
    connection control). In-house responder, libfcgi as fallback. See D8 and
    04-query-api.md.
Q2 [answered] eccodes: distro package; CMake must also accept a hand-built
    copy via ECCODES_ROOT. See D9 and T11. (Not installed on the dev VM, so
    the implementing session may need to install it or test with the option
    off.)
Q3 [answered] Profile (sounding) storage. Decision (Chris): levels are a
    geometric value - a distance - so vertical searching and weighting is
    consistent with the other spatial dimensions (time stays separate). Where
    a level has no height (pressure only), use geopotential height from a
    recent GRIB as the lookup.
    Impl: one report per sounding; the values table has a `height_m f32`
    column = geometric height above MSL (same meaning as the report's
    height_m; NaN on input = the report's height, stored finite). Ingest
    converts reported geopotential heights to geometric and fills
    pressure-only levels from the GRIB geopotential-height field before
    add_report(). The tree still indexes reports (one point per sounding at
    its launch position/time). Tests: `segment_profile_levels`,
    `segment_round_trip`.
    Follow-ups, answered 2026-11-05: (a) yes - values carry dlat_deg /
    dlon_deg f32 offsets from the report position (0 for single-level
    reports); (b) no - the tree indexes the sounding once, queries
    interpolate within the profile by height; a separate level tree can be
    added later if corridor search needs it. Original text: (a) balloon drift - levels sit tens of km
    downwind of the launch site at the top; add per-level lat/lon (or a drift
    offset) now while the format is unreleased? (b) should profile levels
    ever be individual tree points (e.g. corridor search for a flight route at
    altitude), or does T4 always find the sounding and then interpolate
    within the profile by height?

Q4 [answered 2026-11-05] Variable registry (`var_code`): the source of
    truth is a checked-in file in this repo (docs/design/variables.md or a
    CSV next to it) with a generated C++ header of constants; the web app
    imports it into MySQL for display and joins. Codes are never reused or
    renumbered (they are written into segment files). Unlike stations it is
    not edited in MySQL, because decoders need the codes at compile time.
    Start with ~25 common surface/upper-air variables. Task: T8b.
Q5 Small choices made in T1/T2 that the spec did not pin down.
    a) [answered 2026-11-05] Source-class config lives in MySQL, edited in
       the web app, exported as config/sources.json (D10,
       05-reference-data.md). The loader is now strict: format/version
       marker required, every field required, unknown keys rejected, names
       `[a-z][a-z0-9_-]*` <= 31 chars. Built-in defaults are seed data.
    b) [answered 2026-11-05] A segment whose stored metric differs from the
       current source class is rebuilt in RAM with the current metric when
       a snapshot is adopted (not refused, not mixed). For T4.
    c) [answered 2026-11-05: keep] Sealing uses link() to the final name instead of rename(), so
       a sealed file can never be replaced (EEXIST -> error). Directories
       created on the way are fsynced too.
    d) [answered 2026-11-05: keep; the report_key hash is defined in
       ingest, T11] Rows keep insertion order (tree ids = row index); report_key
       is supplied by the caller - no hashing helper yet (belongs to
       ingest, needs the report_type definition).
Q6 [answered] Station table and reference data (2026-11-05, in the
    implementation session). Decisions: MySQL holds the authoritative
    reference data (D10) and the engine reads exported immutable
    generations; identifiers are typed (icao, wmo, wigos, platform) with
    validity intervals; unknown stations become provisional entries instead
    of being dropped; each generation is a full copy; a ref names the
    station, not a period; refs only grow (explicit refs must exceed the
    previous max_ref). Implemented in T3. Update: only the web app talks to
    MySQL (D10, option B), so the engine needs no client library and the
    schema is the web app's Doctrine migrations; the contract is
    05-reference-data.md.
Q7 [answered 2026-11-05] Ref allocation for provisional stations. Both the web app
    (admin creates a station) and ingest (unknown station in a report)
    create stations, and refs must never collide or be reused. Proposal:
    disjoint ranges. The web app assigns refs in [1, 2^31); ingest assigns
    provisional refs in [2^31, 2^32), counting up from its own high-water
    mark kept in the manifest. A provisional station keeps its ref forever
    (reports already point at it): confirming it clears the flag; if it
    duplicates a real station it is marked as an alias of that ref (needs a
    `merged_into` field). Ingest reports new provisional stations by writing
    config/outbox/provisional-<seq>.json for the web app to import.
    Consequence for T3: the "new refs must exceed max_ref" rule becomes per
    range (the builder tracks both high-water marks).
    Decision: disjoint ranges as proposed. Implemented: kProvisionalRefBase
    = 2^31, max_ref() / max_provisional_ref(), Station::merged_into (target
    must exist and not be merged itself). Test:
    `stations_ref_ranges_and_merge`. The outbox file format is part of T5b.
Q8 [open] Reference-data import policy (T5b). The spec did not pin these
    down; implemented as below (conservative), please confirm or change:
    a) Removing or renaming a source class in sources.json while segments
       of it are still in the manifest: the export is rejected (previous
       classes stay) until those segments expire. Alternative: drop the
       segments (loses irreplaceable observations) or keep a hidden copy
       of the old class.
    b) A web-app ref ([1, 2^31)) of the current generation missing from
       stations.json rejects the export; a provisional ref missing from it
       is kept (not imported by the web app yet); the export may not
       contain provisional refs ingest never assigned.
    c) Outbox: one file per station generation listing all provisional
       stations not yet in the last valid stations.json, so the web-app
       import must be idempotent by ref (documented in
       05-reference-data.md). Chosen because it heals itself after a crash
       between publishing and writing the file.
    d) Nothing is published until a valid sources.json exists (the web app
       seeds it from SourceRegistry::defaults()); Manifest::validate now
       requires at least one source class.
    e) Errors are only returned to the caller (RefImportReport.errors); the
       web app cannot see them. Should ingest write e.g.
       config/outbox/import-status.json so the admin UI can show "export
       rejected: ..."?
Q9 [open] GRIB2 message selection (T11). A field file holds one field per
    (param_id, level_type, level_value), but NCEP files can carry several
    messages that map to the same key. Implemented meanwhile (skips are
    reported with a reason, never silent):
    a) Statistically processed fields (template 4.8): only intervals that
       start at the run time are kept (HRRR f03 has APCP 0-3 h and 2-3 h;
       the 2-3 h one is skipped). Should hourly/bucket accumulations be
       kept too, e.g. as their own variable codes or with the interval
       recorded in the field directory?
    b) Only the first fixed surface defines the level; layers (second
       surface: 0-6 km shear, 180-0 hPa above ground CAPE vs 255-0) that
       collide on the first surface keep the first message. Add a second
       level to FieldInfo?
       Seen in real data (GFS 0.25 f000, 2026-11-08): RH on sigma layers
       0.44-0.72 and 0.44-1.0 share the first surface; the second was
       skipped. 210 of the other 211 skips were parameters not yet in the
       registry (hydrometeor mixing ratios, ozone, soil/land, waves).
    c) WMO-standard parameters only (< 192 in table 4.2). NCEP-local ones
       (composite reflectivity 0.16.196, HPBL 0.3.196, MSLMA 0.3.198) are
       skipped until the registry gets centre-specific GRIB mappings.
    d) Other templates (ensembles 4.1/4.11, probabilities 4.5/4.9) and
       sub-hourly steps (HRRR subh files) are skipped.
    e) Messages on a second grid in the same input are skipped (one grid
       per field file); messages for another run/hour are an error.
Q10 [decided by Chris 2026-11-08, design open] Store GRIB natively.
    The GFS 0.25 test (CLAUDE.md benchmarks) showed f32 tiles are ~4.4x
    the GRIB2 size; with RAP, HRRR, RRFS, ECMWF and more that is tens of
    TB/day before obs. Decision: keep the GRIB2 files as published and
    maintain an index of "useful pointers" into them instead of
    re-encoding. Needs a design in the architect session, replacing or
    narrowing 03-field-tiles.md. Points to settle:
    a) Index contents per message: file, byte offset/length, grid key,
       (param, level, second level, stat interval), run, fhr, packing.
       Grids stay in GridStore (section 3 identity as now).
    b) Query cost: GRIB2 complex/JPEG2000 packing must be unpacked whole
       to read one point (milliseconds for HRRR), so a decoded-message
       LRU in RAM and coalescing of identical in-flight decodes are
       required, as CLAUDE.md already plans for tiles. Maybe keep f32
       tiles for a small hot set (latest run, popular fields).
    c) Scan-mode normalisation and unit conversion move from ingest to
       decode time (same code: GribReader).
    d) Retention/gc work on whole GRIB files referenced by the index.
    Meanwhile T9 reads gridded data through a FieldStore interface
    (query.h), so a GRIB-native store can replace the T7 field files
    without touching the query code.
    Chris's design direction (2026-11-08): GRIB messages are not generic
    nodes. Keep a map { message id (string or other unique id) ->
    { decoded message, last accessed } }; recently used messages are ready
    for iteration/indexing. When a new entry is added, purge by total map
    size and last access, oldest unused first. The size of the data is not
    known up front, so check the process size in /proc to decide when to
    purge. Never purge in the middle of a query (memory may spike, then
    recover: GRIB queries are not continuous); possibly a forced cleanup
    cycle if memory gets out of control.
    Implementation notes (to confirm before building):
    - Cache the DECODED, normalised float array (immutable, shared_ptr)
      rather than only the codes_handle*: eccodes unpacks the data section
      on every codes_get_double_array call (milliseconds for complex /
      JPEG2000 packing), and one handle cannot be used by several query
      threads at once. The handle (or its metadata) can be kept alongside
      for key lookups. shared_ptr pinning also gives "no purge mid-query":
      an entry in use is only freed when the last query releases it.
    - Account entry bytes exactly (ni*nj*4 per decoded message) as the
      primary purge trigger; keep /proc/self/statm RSS as the safety net
      for the forced cleanup, followed by malloc_trim(0), because glibc
      often keeps freed memory and RSS alone may not drop after a purge.
    - Surface obs, aviation messages and future road sensors are treated
      equally (one report model, distinguished by source class) - as the
      obs store already does.
Q11 [open] /v1/point details the sketch left open (T9, query.h). As
    implemented, please confirm or change:
    a) Field syntax `name[@level]` with @sfc, @msl, @<x>m (above ground),
       @<x>hPa, @L<type>=<value>; without @level the query's level_hpa or
       alt_m applies, else the variable's only stored level.
    b) Time: newest run whose forecast hours bracket the time, linear
       interpolation between the two hours of that run (never across
       runs). No extrapolation.
    c) Vertical: log-p between isobaric levels; alt_m via geopotential
       height on isobaric levels converted to geometric (R = 6371008.8 m);
       below the lowest/above the highest level -> status out_of_levels
       (no extrapolation to the surface yet).
    d) Each value carries a status (ok, no_time, outside_grid, no_field,
       level_required, out_of_levels) so the web UI can explain gaps.
    e) obs_radius_km is a radius in each source class's space-time metric
       (time offsets count as distance via lambda), not pure distance.
    f) (T10) `time` is required on /v1/point (no "now" default), so equal
       requests on one snapshot give byte-identical responses as the spec
       demands; the web UI passes the time it shows.
    g) (T10) ?snapshot=<seq> responses are Cache-Control: public,
       max-age=86400; live ones no-store. A pinned snapshot whose files
       expired answers 410, one no longer retained 404.
Q12 [decided by Chris 2026-11-08, format open] Map tiles are PNG,
    coloured on the back end with colormaps (the product manifest's
    colormap type: ../product_manifest.json). Contract draft in
    docs/api/query-api-v1.md (/v1/tiles/{model}/{field}/{run}/{fhr}/
    {z}/{x}/{y}.png?colormap=, /v1/colormaps). To settle: the colormap
    export format (config/colormaps.json from the web app, like
    sources.json: name, units, stops value->colour, interpolation
    linear|log|exponential|step, below/above colours), default colormap
    per variable, tile resampling (bilinear from the grid per pixel),
    and whether the run segment accepts "latest" (would make tiles
    mutable, so probably not).
