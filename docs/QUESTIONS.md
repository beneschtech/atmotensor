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
Q10 [decided by Chris 2026-11-08; implemented in T10c] Store GRIB natively.
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
Q12 [decided by Chris 2026-11-08; implemented in T10b] PNG map tiles,
    coloured on the server. Colormaps are per user and per product, owned
    by the web app; they travel IN EACH TILE REQUEST (`cmap=` query
    parameter), not via SQL (D10: engine never talks to MySQL; tiles stay
    immutable and cacheable; no colormap state in the engine). Syntax:
    value:RRGGBB[AA] stops joined by lin|log|exp|step segment types, 2-64
    stops ("if someone has a 256 entry colormap, they need to rethink
    their life"). Below the first stop -> first colour, above the last ->
    last colour; anything else goes into the colormap itself. Single
    optional `nodata` colour, default transparent. No built-in colormap
    registry and no /v1/colormaps. Contract: docs/api/query-api-v1.md.
Q13 [answered by Q18a-c, implemented in T17b] T15 sigcheck results (07-grib-catalog.md "Parameter identity",
    deny list v1), 2026-11-11. Run on:
      [0] gfs.t18z.pgrb2.0p25.f000 (2026-11-08 18Z, 696 msgs, process 81)
      [1] gfs.t00z.pgrb2.0p25.f210 (2026-01-23 00Z, 743 msgs, process 96)
      [2] gfs.t18z.pgrb2.0p25.f216 (2026-01-22 18Z, 743 msgs, process 96)
      [3] hrrr.t00z.wrfnatf00 (2026-11-09 00Z, 1133 msgs, process 83)
      [4] rrfs.t18z.2dfld.3km.f000.conus (262 msgs, process 134)
      [5] rrfs.t18z.prslev.3km.f000.conus (675 msgs, process 134)
    ([1],[2] are from ~/Downloads: /opt/atmotensor/data/grib holds only one
    f000 per model.)  Result: 0 signatures unstable across files, 0
    catalog-key collisions within a file, no hash collisions. GFS f000 vs
    f210/f216 (analysis vs forecast, three runs) give identical signatures
    for every shared parameter/level. 61 signatures are unknown to stock
    eccodes 2.45 (NCEP local parameters: 0/1/82, 0/6/28, 0/20/0 pdt 48 RRFS
    aerosols, 3/192/* HRRR satellite, ...) - the T19 "undefined" page will
    have work from day one. The tool does detect problems: with
    forecastTime removed from the deny list it reports all 743 GFS
    signatures as UNSTABLE with "forecastTime: 216 -> 210", and a doubled
    message shows as a collision.
    Coverage gaps, please supply files if you want them checked before T16
    lands on real data: no consecutive forecast hours (so the 1 h / 3 h /
    6 h / run-total accumulation siblings and the first-bucket rule are
    only covered by unit tests), only one HRRR/RRFS hour, no ensemble
    (GEFS/HREF) and no wrfsfc/wrfprs HRRR files.
    Points for the architect:
    a) GFS f000 is generatingProcessIdentifier 81 (analysis), every
       forecast hour is 96. With "model = process id" (Q1 of 07), GFS
       f000 becomes a separate model whose runs have a single offset, and
       the GFS (96) tree has no hour 0. LEADS probably shows the same
       split. Options: accept; or a web-editable "model alias" mapping
       (process 81 -> model 96) applied at ingest. T16 is implemented as
       specified (81 and 96 are separate models) meanwhile; a later
       mapping needs a re-ingest of the affected runs only.
    b) Key names come from eccodes' definition files, not the WMO octets:
       eccodes 2.45 already calls aerosolType "constituentType". An
       eccodes upgrade that renames a section 4 key changes those
       signatures. Recommend: treat an eccodes upgrade like a deny-list
       change (run sigcheck on stored files before and after; bump the
       signature version if anything moved), and pin the distro eccodes
       version in deploy/README.md.
    c) Deny list additions beyond 07's list, all eccodes spellings or
       obvious time bookkeeping (src/gribsig.cpp kDenyV1):
       indicatorOfUnitForForecastTime, numberOfTimeRanges (eccodes names
       of the spec's keys), wallClockInitialTimeOfExecution*,
       *OfForecastUsedInLocalTime. Kept (they say what the field is):
       typeOfTimeIncrement, reference/verification period keys
       (climatologies), ModelVersion date keys in the templates that have
       them (a model upgrade would then make new parameters - say if you
       want those denied).
    d) Signature details decided while implementing (in gribsig.h):
       repeated keys (one per time range) are numbered key, key#2, ...;
       missing values are written "missing"; calendar interval units
       (month, year) are written "<n>u<code>" with stat_interval_s a
       nominal length (30 d / 365 d); offset_end = forecastTime + length
       of the first range, as eccodes' endStep (the end-of-interval date
       is only used for calendar units); an unknown template is detected
       by eccodes returning a handle without section 5.
Q14 [answered 2026-11-11, Chris: production is MySQL, not MariaDB]
    Done: the SQL now uses the MySQL 8.0.19+ row alias in ON DUPLICATE
    KEY UPDATE (`... AS new ... x = new.x`) instead of the deprecated
    VALUES() (Q16e); 07 "New dependency" and TASKS wording corrected;
    tests/catalog_testdb.sh runs mysqld only. CMake still accepts either
    client library (libmariadb speaks to MySQL too). Original entry:
    MySQL vs MariaDB (T14/T16). 07 says MariaDB Connector/C and
    "tests on MariaDB", but the web app's DATABASE_URL is MySQL
    (serverVersion=8.0.32 at 172.16.2.2) and this workstation has MySQL
    8.4 server + libmysqlclient-dev, no MariaDB packages. Done meanwhile:
    T14's migration is tested on a throwaway MySQL 8.4 instance (private
    mysqld in a temp dir; never the configured server); the engine code
    uses only the C API both libraries share (mysql.h), and CMake accepts
    either libmariadb or libmysqlclient (ATMO_WITH_MYSQL). Please confirm
    the production server is MySQL 8 and correct 07's "New dependency"
    wording, or say if MariaDB is the target.
Q15 [answered by Q18d, implemented in T17b/T17c] Unusable grids in the tree (T16). 07 wants a message with an
    unusable grid (reduced Gaussian, spectral, ...) catalogued with
    grid_id NULL, but its run needs a model_grid, and grib_model_grid.grid_id
    is NOT NULL. Done meanwhile: such messages (and undecodable ones) get a
    placeholder grib_grid row keyed by the GridIdentity of their raw
    section 3 (ni/nj where eccodes gives them, else 0) with
    summary {"supported": false} and no GridStore file; model_grid/run
    hang off it; grib_message.grid_id stays NULL and flags say why
    (1 undecodable, 2 grid unsupported; GribMessage::FLAG_* in the web
    entity). Confirm, or prefer a nullable model_grid.grid_id.
Q16 [b answered 2026-11-11, Chris; a, c, d open] T16 details decided
    while implementing, for review:
    a) ingest_seq = the generation of the commit that wrote the row; the
       generation is bumped FIRST in each file's transaction, so its row
       lock serialises ingest commits and "ingest_seq <= generation seen"
       has no gaps for the future matcher.
    b) ANSWER (Chris): every newer copy gets a new id; old ids answer
       410 with replaced_by. Done: schema version 3 (web migration
       Version20261111220000, table grib_message_replaced old_id ->
       new_id, kept flat), ingest stages each file in a temporary table
       (the ingest user needs CREATE TEMPORARY TABLES), deletes rows with
       the same key and other bytes, inserts, records old -> new;
       identical bytes keep their id (re-ingesting a file does not churn
       ids); realias records the dropped duplicate the same way; expire
       drops mappings whose target is gone. A changed copy's old row is
       not kept as history (one row per key). Messages a corrected file
       no longer contains stay (they belong to the older file); only a
       file re-catalogued at the SAME path drops its rows the new
       content lacks (an overwrite in place).
       Original: Latest wins keeps the message id (INSERT ... ON DUPLICATE
       KEY UPDATE), so an API handle follows the newest data for its key;
       a re-ingested file's rows that the new version lacks are deleted.
    c) Re-ingesting the same path is allowed when the file on disk is
       byte-identical (the link is reused); different content under the
       same name is refused.
    d) Default names the engine writes once (web owns them afterwards):
       model slug "<centre>-<subcentre>-<process>", empty centre/model
       names, model_grid "0.25DEG LL WORLDWIDE" / "LAMBERT 3KM
       1799x1059" / "<GRIDTYPE> <ni>x<nj>", parameter
       "<eccodes name> 3hr|15min|run total". eccodes-identified
       signatures reuse a grib_parameter with the same (short_name,
       name, units, origin eccodes).
    e) [resolved by Q14] VALUES() in ON DUPLICATE KEY UPDATE replaced by
       the row alias (MySQL 8.0.19+; the web app's server is 8.0.32).
Q17 [answered 2026-11-12: a) shapes approved by Chris (API section no longer Draft), b) Q17b, c) Q19/Q20, d) informational] T17 (query side), 2026-11-11. Built: /v1/grib/* drill-down,
    message details, point and tiles by message id, first-bucket repair,
    X-Atmo-Generation; CatalogReader (MySQL) behind a CatalogQuery
    interface so libatmo stays MySQL-free; atmo_queryd --catalog CONFIG.
    Verified on real GFS 0.25 f000: T850 at OKC through the API =
    282.6315 K, equal to a hand bilinear of grib_get's four corners;
    first point 280 ms (decode), tile 18 ms after. Please review:
    a) The JSON shapes in docs/api/query-api-v1.md "Grid catalog"
       (marked Draft): ids for filters (center=, model=, ...), level as
       `level=<type>:<value>` (+ `level2=`), signatures as 16 hex digits,
       tiles by id `no-cache` + ETag unless `v=<content_hash>` pins them
       (immutable), 409 not_sampleable for flagged messages, values
       scaled with grib_parameter si_scale/si_offset, winds as stored
       with `uv_relative_to_grid`.
    b) [answered 2026-11-11, Chris: use parameterName] Done:
       GribSignature.table_name/table_units = eccodes parameterName /
       parameterUnits; default grib_parameter name/units come from them
       and rows are matched on (name, units) (case-sensitive), so GFS's
       one temperature signature is "Temperature", K. Found on real
       data, decided meanwhile (please review):
       - eccodes answers parameterName with the bare number where table
         4.2 has no entry (NCEP local 192-254, and some WMO numbers its
         table lacks, e.g. 0/0/21): those fall back to eccodes `name`
         (level-dependent risk remains only for them) and `units` with
         "**" dropped ("kg m**-2" -> "kg m-2", the table's spelling).
       - grib_parameter.short_name is written '' for engine-created rows:
         eccodes has no level-independent short name. The API returns
         "" until the web app sets one; eccodes' shortName of the first
         message stays in grib_signature.ecc_short_name.
       - The table name lacks the statistic ("Time-maximum 10 metre wind
         speed" becomes "Wind speed"), and the old "run total" suffix was
         wrong for maxima/averages anyway; since parameters are matched by
         name, a maximum and a minimum could have merged. Default names
         now carry code table 4.10 for non-accumulations: "Wind speed
         maximum 1hr", "Hail maximum since run start", "... average 6hr";
         accumulations unchanged ("Total precipitation 3hr", "... run
         total"). GribSignature.stat_process holds the code.
       - Real data after re-ingest (GFS f000, HRRR nat, RRFS 2dfld +
         prslev): 141 parameters, no numeric names; the only repeated
         names are categorical rain/snow/ice pellets/freezing rain, which
         exist both as NCEP local 0/1/192-195 and WMO 0/1/33-36 (units
         "(Code table 4.222)" vs "Code table 4.222").
       No migration needed: no catalog with the old names exists outside
       throwaway test databases.
       Original entry: IMPORTANT, seen on real data: eccodes short names/names are
       level-dependent ("2t" = 2 metre temperature, "t" = temperature,
       likewise 2d, 10u/10v, msl...), but a signature excludes the level
       by design. The signature row takes what eccodes said for the first
       message seen, so GFS's one temperature signature (2 m and 41
       isobaric levels) is labelled "2 metre temperature" / "2t". Same
       risk for the grib_parameter rows. Recommend: default names from
       the level-independent code table 4.2 keys (eccodes
       `parameterName`, `parameterUnits`: "Temperature", "K") and keep
       shortName/paramId as informational ecc_* columns only. Not changed
       (07 says shortName/name/units); a fix needs a re-ingest or an
       UPDATE of unedited rows.
    c) PARKED: "existing /v1/point and /v1/tiles resolve through the
       catalog". Needs two decisions the spec does not make: (1) which
       catalog model(s) an API model name like "gfs" means (catalog
       models are 7-0-96, and GFS f000 is a separate model 7-0-81, Q13a;
       grib_model.slug is web-editable and not unique - should several
       models with the same slug merge for /v1/point?), and (2) how a
       registry variable maps to a signature (grib_parameter.variable_code
       is NULL until the web app sets it; or match the registry's GRIB2
       triple with pdt 0 / statInterval=run as the T10c index does).
       /v1/point and /v1/tiles keep working on the manifest meanwhile, so
       T18 (removing idx.json / manifest gribs) is blocked on this.
    d) The reader reads the small tables (centres ... runs, all
       signatures with their parameters) in one consistent snapshot per
       generation; message rows are fetched per (run, signature) and by
       id afterwards and may be newer than those tables (cleared at the
       next generation change).
Q18 [decided by Chris 2026-11-11] Answers to Q13a, Q13b, Q13c, Q15:
    a) (Q13a) Add the model alias: a web-editable mapping
       (centre, subcentre, process id) -> model, applied at ingest, so
       GFS process 81 (analysis, f000) files land under the GFS model 96
       and its runs have hour 0. Table `grib_model_alias` (migration in
       ../web, written by the web app only); default: none. Changing an
       alias re-ingests only the affected runs. The message keeps the
       raw process id (new column `process_id` on grib_message) so
       nothing is lost.
    b) (Q13c) Deny the model-version date keys: a model upgrade must not
       make new parameters ("GFS 0.25 degree" stays one tree; old data
       is replaced and purged). Bump the signature version.
    c) (Q13b) eccodes is upgraded deliberately, not with routine
       updates: pin/hold the package (deploy/README.md), and an upgrade
       is a planned step that runs sigcheck on the stored files before
       and after; renamed keys and names are handled then.
    d) (Q15) Keep the data. Messages on a grid we cannot display natively
       stay catalogued and readable (decoded values in file order) so
       derived products can use them. The placeholder grid row is
       confirmed; make "natively viewable" an explicit boolean column on
       grib_grid (not only summary JSON), returned by /v1/grib/grids and
       /v1/grib/messages/{id}; point/tiles on such a message keep
       answering 409 not_sampleable.
    Q13 and Q15 are closed by this entry; Q17c is still open.
    Implemented 2026-11-11 (T17b): a) grib_model_alias keyed by codes
    (centre, subcentre, process), applied at ingest, `--realias` for
    existing data (Q21d); b) signature version 2; c) deploy/README.md
    (the hold itself needs sudo: not done on this box); d) column
    grib_grid.viewable (API key `viewable`, was `supported`).
Q19 [decided by Chris 2026-11-11] Answer to Q17c: how /v1/point and
    /v1/tiles (by name) resolve through the catalog. Unblocks T18.
    a) Model names address a model + grid, not a model: `gfs_0.25p`,
       `rrfs_na`, `rrfs_conus` instead of `gfs`. New web-owned column
       `api_name` on grib_model_grid (unique, [a-z0-9_.-]). Engine default
       when it creates the row: model name (slug if the name is empty) +
       "_" + grid name, lower-cased, spaces -> underscores
       ("gfs_0.25deg_ll_worldwide"); the web app shortens it. The model
       alias (Q18a) is applied before this, so process 81 data is under
       the same name. /v1/meta lists these names.
    b) Field names: new web-owned table `grib_common_name`
       (name unique -> grib_parameter id), the "common names". Lookup
       order for the name part of a field spec (before `@level`):
       1. grib_common_name;
       2. else the parameter whose name, lower-cased with spaces ->
          underscores, equals it ("total_precipitation_3hr",
          "temperature"). Two parameters normalising to the same name
          (categorical rain: NCEP local and WMO) -> 400 ambiguous_field,
          fixed by adding a common name.
       Matching is case-insensitive. The level syntax (`@2m`, `@850hPa`,
       `@L<type>=<value>`) and the time / vertical interpolation of
       /v1/point are unchanged. Within a run, a parameter with several
       signatures resolves to whichever is present; more than one
       present at the same level and offset -> ambiguous_field.
    c) The variable registry (variables.csv) is no longer consulted by
       /v1/point and /v1/tiles; its names can be seeded into
       grib_common_name by a web command so existing field specs
       (`air_temperature@2m`) keep working.
    Assumptions by the architect, say if wrong: case-insensitive
    matching; `api_name` is edited by hand rather than derived (nothing
    in the GRIB says "0.25p" or "na").
Q20 [decided by Chris 2026-11-11] Name matching and the grid tree
    (extends Q19).
    a) Every text argument that names something in the catalog (model
       name, field name, and any later one) is resolved the same way:
       1. exact match (case-insensitive) -> use it, no warning;
       2. else substring match, wildcards on both sides (`%text%`):
          model names against grib_model_grid.api_name, then the model
          slug; field names against grib_common_name, then the
          normalised parameter names (Q19b);
          - exactly one match -> use it as a best guess AND say so;
          - more than one -> 400 `ambiguous_name` with the candidates
            (at most 20) in the error body;
          - none -> 404 `not_found`.
    b) A best guess is always reported. JSON responses carry
       `warnings: [{code: "best_guess", argument: "models",
       asked: "gfs", used: "gfs_0.25p", message: "..."}]` and echo the
       resolved name wherever the name appears in the body. Non-JSON
       responses (tiles) carry the same as `X-Atmo-Warning` headers and
       are `Cache-Control: no-store` when a guess was used (a guess can
       change when a new model arrives).
    c) No `match=exact` switch (Chris, 2026-11-11): guessing with a
       warning is the friendly default for people exploring the API.
       The web app does not need it: it takes exact names from
       /v1/grib/tree, so it never triggers a guess; if it ever receives
       a `best_guess` warning it logs that as its own bug.
    d) New endpoint `GET /v1/grib/tree`: the current grid tree in one
       call, so a client can pick exact names deliberately:
       centres -> subcentres -> models -> grids (with `api_name`,
       natively-viewable flag) -> runs (base time, member, number of
       parameters, first/last offset). `depth=` cuts it short
       (centers|models|grids|runs, default runs); `parameters=1` adds
       each run's parameter list (signature, name, common names).
       Served from the in-memory tables, `X-Atmo-Generation` as usual.
Q21 [answered 2026-11-11, Chris: a-f and h as recommended; g changed,
    see the end of g] Follow-ups to Q18-Q20 before implementing them.
    a) Units (Q19c vs the API's "SI everywhere, fractions not percent"):
       variables.csv carries the GRIB -> SI scale (relative_humidity
       0.01). Once /v1/point resolves through grib_parameter, values use
       its si_scale, which is 1 for eccodes-created rows, so
       `relative_humidity@2m` would answer 45 (%) instead of 0.45.
       Recommend: the web command that seeds grib_common_name from
       variables.csv also sets si_scale/si_offset/units on the target
       parameter when that row still has the engine defaults (1, 0,
       eccodes units); otherwise the API doc's units rule needs changing.
    b) Q19c says variables.csv is "no longer consulted by /v1/point":
       I read that as the gridded part only. Observations in /v1/point
       are keyed by the registry's var_code (written into obs segment
       files), so obs keep using the registry.
    c) Which run /v1/point and /v1/tiles use by name: as now, the newest
       run (of the model_grid, member -1) that has the field at the
       requested time; no run= argument (the by-id endpoints cover
       pinning).
    d) Changing an alias (Q18a) "re-ingests only the affected runs":
       since grib_message keeps the raw process_id, I would make it a
       pure SQL move (messages of the aliased model's runs re-parented
       to the target model's runs, generation bumped) done by an engine
       tool `atmo_grib_ingest --realias`, not by re-reading files. The
       web app only writes grib_model_alias, then runs that tool (or
       the future ingest daemon notices the alias table changed).
    e) api_name default collisions (Q19a): the default is built from
       names that are not unique (slug is web-editable; two grids can
       share a default grid name). On a clash the engine appends "_2",
       "_3", ...
    f) Schema changes (grib_model_alias, grib_common_name,
       grib_model_grid.api_name, grib_message.process_id, a boolean on
       grib_grid for Q18d): a SECOND migration in ../web rather than
       editing Version20261111120000, in case that one has already been
       applied anywhere. Column name for Q18d: `viewable` (TINYINT(1),
       NOT NULL); it replaces the summary JSON "supported" flag.
    g) Q18d "readable (decoded values in file order) so derived products
       can use them": no HTTP endpoint for that now (point/tiles stay
       409); the engine side gets a decode-by-message-id call returning
       the values and the raw grid description, for T11b. Say if you
       want an HTTP endpoint (e.g. /v1/grib/messages/{id}/values).
       ANSWER (Chris): users must be able to get the data. Decided with
       Chris, by message id (a grid row has no values):
       - GET /v1/grib/messages/{id}/raw: the GRIB2 message bytes as
         stored, any message (even undecodable), application/x-grib2,
         immutable (ETag = content_hash).
       - GET /v1/grib/messages/{id}/values: decoded values in file
         (scan) order, little-endian float32, NaN = missing, native
         units scaled like point/tiles; any decodable message incl.
         non-viewable grids; 409 only for undecodable ones.
       - GET /v1/grib/grids/{id}: grid description (type, ni, nj,
         viewable, bbox, section 3 as hex, eccodes gridType) so the
         values can be placed.
    h) Signature version 2 (Q18b) changes every hash: existing rows are
       not migrated (only test databases have v1 rows). If a real
       catalog exists somewhere, it needs to be emptied and re-ingested.
Q22 [answered 2026-11-12, Chris: all as implemented; c) first-bucket
    repair added to /v1/point and /v1/tiles (CatalogFieldStore)] T17c
    details decided while implementing, for review
    (2026-11-11):
    a) Roles: point() and the tiles need to know which fields are the
       u/v pair (rotation) and geopotential height (alt_m). Catalog
       parameters get the role from the GRIB2 numbers of their
       instantaneous signatures (0/2/2 u, 0/2/3 v, 0/3/5 gh); the partner
       of a u is any v parameter stored at the same level and time.
       Statistical signatures (max wind etc.) never get a role.
    b) Whole hours only: ModelTime is (run, integer hour), so 15-minute
       RRFS offsets are not reachable through /v1/point and /v1/tiles
       (they are through /v1/grib/*). Widening it touches field files and
       the meta format; say if you want it.
    c) First-bucket repair (Q17) is applied in /v1/grib/offsets only, not
       in /v1/point: "total_precipitation_1hr" at f01 is reported
       no_field when only the run total 0-1 h exists. Could be added.
    d) Only member -1 runs and viewable grids appear as /v1/point models.
    e) Matching order follows Q20 literally, so common names are searched
       before parameter names: with the seeded names, "temp" is
       ambiguous (air_temperature, dewpoint_temperature) although the
       parameter name "temperature" contains it as well. Exact names
       are unaffected.
    f) Gridded data comes from the catalog only once --catalog is given
       (field files from --tiles are then not served); a snapshot is
       still required for /v1/point and /v1/meta (observations).
    g) /values ETag is the content hash prefixed with "v" (different from
       /raw's for the same message).
Q23 [answered 2026-11-12, Chris: a) no catalog-side grid cleanup for
    now; b) as implemented; c) the ~1 s window is fine; d) expiry runs
    with every ingest call: `--expire` now also takes files (ingest,
    then expire)] T18 decisions, 2026-11-11, for review:
    a) SAFETY: ManifestPublisher::collect_garbage() swept grids/ and grib/
       for files the manifest does not name, and catalog-ingested files
       never were named: the first gc run would have deleted the
       catalog's GRIB and grid files (nothing ran gc in production yet).
       Now gc never touches grids/ or grib/, and manifest retention no
       longer expires grids (the core cannot see whether the catalog
       still uses one; there are only a handful of grid files). Grid
       files are therefore never deleted automatically; say if you want
       catalog-side grid cleanup (grib_grid rows without messages).
    b) Manifest version 2 (no "gribs"); version 1 is read: its index
       sidecars become retired entries (gc unlinks them after the grace
       period), its GRIB files are left on disk (the catalog may use the
       same paths). The live store needs its GRIB files catalogued in
       place after deploying (deploy/README.md "Upgrading").
    c) Catalog::expire(): K newest distinct base times per model grid
       (all members of a kept base time stay), K overridable by api_name;
       emptied grib_file rows deleted in the same transaction, files
       unlinked right after the commit (not after a grace period): a
       query daemon still on the previous generation can fail to read
       such a file for up to its 1 s poll. A grace period would need a
       "retired" column on grib_file.
    d) atmo_grib_ingest: the old default (sidecar + manifest) is gone;
       modes are --catalog (ingest | --realias | --expire), --tiles, and
       --seed-sources (also alone: writes config/sources.json if missing
       and publishes it). No ingest daemon calls --expire yet.
Q24 [open] From T19 (web grid picker), 2026-11-11, for the engine side:
    a) The picker needs each parameter's by-name `field`. /v1/grib/tree
       ?parameters=1 has it but only for the whole catalog at once, and
       /v1/grib/parameters?run= does not, so the web app recomputes it
       (first common name from its own database, else the normalised
       name: same rule as CatalogFieldStore). Suggest adding `field` and
       `common_names` to /v1/grib/parameters so the rule lives in one
       place; the web would then drop GET /api/grib/runs/{id}/common-names.
    b) Layers from the picker go through /v1/tiles by name, not by message
       id, because by name keeps newest-run following, hour stepping,
       /v1/point and rotated winds. What that cannot reach today is shown
       in the picker as not addable, with the reason: ensemble members
       (Q22d), grids that are not viewable, sub-hourly offsets (Q22b), and
       a level whose first surface is shared by another layer (by-name
       identifies a level by its first surface only). If any of these
       matter for users, the options are a by-id layer kind in the web
       (pinned to one message, no hour stepping) or widening the by-name
       API (member=, minute offsets, level2 in the field spec).
