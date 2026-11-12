# Grid catalog: every GRIB message identified, catalog in MySQL

Decision D13, 2026-11-11 (Chris + architect Claude). Tasks T14-T19 in
TASKS.md. The deny list has not yet been run against real files: T15's
sigcheck tool does that, and its findings go to QUESTIONS.md.

Supersedes, for gridded data only:
- D10 "the engine never connects to MySQL" (05-reference-data.md). The
  engine now reads and writes the grid catalog tables directly.
  Observations, stations and source classes are unchanged (files +
  manifest, k-d trees).
- The variable registry (variables.csv) as the gate for ingest. A message
  that is not in the registry is no longer skipped.
- The `<file>.idx.json` sidecars and the manifest `gribs` list (T10c). The
  catalog tables replace them. GRIB files on disk, the GridStore and the
  GribCache stay exactly as they are.

## Goal of this step

Every message of every incoming GRIB file is catalogued: what it is
(parameter + units), where it is (grid = location definition), when
(base time, forecast offset) and at which level, and it is reachable
through the API by id. Nothing is skipped because we have no name for it:
an unnamed message is still catalogued, served, and shown as "unknown"
until someone defines it.

Later, not designed here: a job queue that matches incoming messages
(derived products, exports). The hook it needs is in "Ingest".

## Hierarchy (as in LEADS)

    centre -> subcentre -> model -> grid -> base time [-> member]
           -> parameter -> level -> offset

This is the LEADS "Grid to Image" selector: the Source tree reads
NCEP -> NCEP OPERATIONS -> GFS -> 0.25DEG LL WORLDWIDE (GRID 2833) ->
2026111000Z, then Parameters, Levels and Forecast Times as three lists.
Model = generatingProcessIdentifier; the grid is its own tier directly
below the model, so GFS 0.25 and GFS 0.5 are one model with two grids,
each with its own base times (decided by Chris 2026-11-11).

Ensemble members are different runs (decided by Chris 2026-11-11): the
member number lives on the run, not in the parameter, so an ensemble
model's parameter list is as short as a deterministic model's and the
member is picked under the base time.

Small lookup tables for the top five tiers (the web UI needs them as a
tree and they carry human names). Parameter, level and offset are columns
of one flat `grib_message` row: no level table, no offset table.

## Parameter identity: the signature

A message's *signature* is everything in its headers that says what the
field is, with time, level, grid, packing and process bookkeeping removed.
Two messages with the same signature are the same parameter at a
different time or level.

Built from eccodes keys, not raw octets:

1. `discipline`, `centre` (local-use code values mean different things per
   centre, so the centre is always part of it).
2. `productDefinitionTemplateNumber`.
3. Every coded integer key of section 4 (keys iterator, skipping computed,
   duplicate and read-only keys), EXCEPT the deny list:
   - time: forecastTime, indicatorOfUnitOfTimeRange (and ...ForTimeRange,
     ...ForTimeIncrement), hoursAfterDataCutoff, minutesAfterDataCutoff,
     every end-of-interval date/time key, lengthOfTimeRange,
     timeIncrement, numberOfTimeRange, numberOfMissingInStatisticalProcess
   - level: typeOf/scaleFactorOf/scaledValueOf First and Second FixedSurface
   - ensemble member: perturbationNumber, typeOfEnsembleForecast,
     numberOfForecastsInEnsemble (stored on the run instead). Derived
     ensemble products (mean, spread, probability: derivedForecast etc.)
     are NOT denied: they are parameters of the run with member = -1.
   - process: typeOfGeneratingProcess, generatingProcessIdentifier,
     backgroundProcess, analysisInstant..., NV / numberOfCoordinatesValues
   This is a deny list on purpose: a template we have never seen (new
   aerosol, chemical, probability, satellite templates) contributes all of
   its distinguishing keys without code changes. RRFS aerosol sizes
   (aerosolType, typeOfSizeInterval, scaled first/second size, wavelength
   interval) fall out of this automatically, as do
   typeOfStatisticalProcessing, probability limits, percentiles.
4. Statistical interval (templates 4.8 and relatives; decided by Chris
   2026-11-11): the interval length is part of the parameter, so "Accum
   Precip 1hr" and "Accum Precip 3hr" are independent parameters instead
   of overwriting each other as in LEADS. The raw length keys stay on the
   deny list; one synthetic key is added instead:
   - `statInterval=<seconds>` when the interval starts after the base
     time (a fixed-length bucket), length normalised to seconds;
   - `statInterval=run` when the interval starts at the base time
     (forecastTime = 0): accumulated/max/min since run start. Its length
     grows with the forecast hour, so hashing it would make one parameter
     per hour.
   Messages with more than one time range hash every range's length in
   order.
   The signature *without* this key is also hashed (`base_hash`), which
   ties the siblings together: 1hr, 3hr and run-total precipitation share
   one base_hash.
   Ambiguity that the header cannot resolve: the first bucket of a run
   (0-1 h at f01, 0-3 h at f03) starts at the base time, so it is filed
   as `run`. Queries repair this without duplicate rows: the offsets of a
   fixed-length parameter of N seconds also include its `run` sibling's
   row with offset_end = N, when one exists.
5. Canonical text = `key=value` lines sorted by key, LF separated, prefixed
   with `v1\n`. Hash = FNV-1a 64 of that text (same function as GridKey),
   stored as 16 hex chars.

The canonical text is stored next to the hash, so a signature is always
explainable ("why are these two different?") and a hash collision is
detected instead of aliasing two parameters. 64 bits rather than CRC-32:
with tens of thousands of signatures CRC-32 would collide eventually, and
FNV-1a 64 is already in the code base.

The deny list is validated the way Chris did it for GFS by hand: a tool
(`atmo_grib_sigcheck`) runs over several runs and forecast hours of a
model and reports any signature that appears in only some of them, with
the keys that differ. A key that causes that goes on the deny list and the
signature version is bumped (v2, ...). Old rows keep their version;
re-ingest regenerates.

GRIB1 (if ever needed): same idea with centre, table2Version,
indicatorOfParameter, timeRangeIndicator.

## Tables (Doctrine migrations in ../web own the schema)

Names are final only once the migration is written. All `grib_` prefixed.

    grib_center      id, code (WMO, unique), name
    grib_subcenter   id, center_id, code, name          unique(center, code)
    grib_model       id, subcenter_id, process_id, slug, name
                     unique(subcenter, process_id)
    grib_model_grid  id, model_id, grid_id, name          unique(model, grid)
                     (name = the tree label, e.g. "0.25DEG LL WORLDWIDE";
                     generated from the grid until someone edits it)
    grib_run         id, model_grid_id, basetime (UTC), member (-1 = not
                     an ensemble member, 0 = control, 1.. = perturbed),
                     member_type (typeOfEnsembleForecast, NULL if none)
                     unique(model_grid, basetime, member)
    grib_grid        id, grid_key CHAR(16) unique, grid_type, ni, nj,
                     periodic_i, west/south/east/north, summary JSON
                     (the GridStore file is grids/<grid_key>.grid as now)
    grib_parameter   id, short_name, name, units, si_scale, si_offset,
                     variable_code NULL (engine registry, for derived
                     products), origin ENUM(eccodes, local)
    grib_signature   id, hash CHAR(16) unique, sig_version, canonical TEXT,
                     base_hash CHAR(16) (indexed), stat_interval_s NULL
                     (NULL = not statistical, 0 = since run start),
                     center_code, discipline, category, number, pdt,
                     ecc_short_name, ecc_name, ecc_units, ecc_param_id,
                     parameter_id NULL, status ENUM(identified, unknown,
                     overridden), first_seen
    grib_file        id, path (unique, relative to the store root), size,
                     messages_total, ingested_at
    grib_message     id, run_id, signature_id, grid_id, file_id,
                     level1_type, level1_value, level2_type, level2_value,
                     offset_start_s, offset_end_s, valid_time,
                     byte_offset, byte_length, content_hash CHAR(16),
                     flags, ingest_seq
                     unique(run, signature, level1_type, level1_value,
                            level2_type, level2_value, offset_end_s,
                            offset_start_s)
    grib_catalog_state   single row: generation BIGINT (bumped by every
                     ingest commit), schema_version

Notes:
- Levels: GRIB code table 4.5 types, values in SI as doubles (scaled value
  / 10^scale); missing second surface = type 255, value 0.
- Offsets are seconds (RRFS has 15-minute output). Instantaneous fields
  have start = end. Statistically processed fields keep their interval
  here too (offset_start_s .. offset_end_s), but what separates "last
  hour" from "since run start" is the signature (above), so both are kept
  (lifts the Q9 restriction).
- Default display name = the level-independent code table 4.2 name
  (eccodes `parameterName`/`parameterUnits`, Q17b; eccodes `name` only
  where the table has no entry), for a statistical parameter + the
  interval ("Total precipitation 1hr", "... run total"; other statistics
  name it: "Wind speed maximum 1hr"); editable in the web app like any
  other.
- `grib_message.id` is the API handle. `content_hash` stays the GribCache
  key.
- A duplicate (same unique key) replaces the older row: latest wins. A
  copy with other bytes gets a new message id; the old id is kept in
  `grib_message_replaced` and the API answers it with 410 + replaced_by
  (Q16b, decided by Chris 2026-11-11; schema version 3).
- Size: GFS 0.25 is ~700 messages x 209 hours per run; with K retained
  runs of a handful of models this is single-digit millions of rows.
  Retention deletes a run and cascades.

Column ownership (so the two writers never fight):
- Engine writes: every row of center/subcenter/model/run/grid/file/message,
  new signature rows and their `ecc_*` columns, catalog_state.
- Web app writes: `name`/`slug` on center/subcenter/model/model_grid,
  all of
  grib_parameter, `grib_signature.parameter_id` and `status`.
- The engine checks `schema_version` at start and refuses to run against a
  schema it does not know.

## Identification and our own eccodes definitions

At ingest, a signature seen for the first time is inserted with what
eccodes says about it (`shortName`, `name`, `units`, `paramId`). If
eccodes knows it, status = identified and a `grib_parameter` row is
created or reused. If eccodes returns "unknown", status = unknown and
`parameter_id` stays NULL; the message is catalogued and served anyway
(native GRIB units, labelled by discipline/category/number).

The web app has a page listing unknown signatures (canonical text, sample
message, model). Defining one there sets `parameter_id`. The database is
the authority.

`app:eccodes:export` then writes local concept files from the database
into `/opt/atmotensor/data/eccodes/grib2/localConcepts/<centre>/`
(`shortName.def`, `name.def`, `units.def`, `paramId.def`), and every
engine process runs with
`ECCODES_DEFINITION_PATH=/opt/atmotensor/data/eccodes:<system path>`, so
eccodes itself, grib_ls and anything else on the box give the same
answers as the catalog. Missing *templates* (eccodes cannot even walk the
keys) cannot be expressed as rows: those are hand-written `.def` files in
the same overlay directory, kept in git under `deploy/eccodes/`.

A message whose template eccodes cannot decode is catalogued with a
signature built from discipline, centre, template number and the raw
section 4 bytes (status unknown, flag `undecodable`). Time and level are
then inside the hash, so each such message is its own signature until a
template definition is added and the file is re-ingested; it is at least
visible and counted.

## Ingest

Per file, one transaction after the file is linked into
`grib/<model>/<run>/`:
1. For each message: grid (GridStore put, `grib_grid` upsert), signature
   (upsert), center/subcenter/model/model_grid/run (upsert), message row.
2. Bulk insert, bump `grib_catalog_state.generation`, commit.

A file is visible to queries when its transaction commits. `ingest_seq`
(monotonic) is the hook for the future job queue: a matcher reads
`grib_message` rows with `ingest_seq > last seen`.

Only unusable *grids* still cause a skip (reduced Gaussian, spectral,
unstructured): the message row is written with `grid_id` NULL and a flag,
so it shows in the catalog but cannot be sampled yet.

## Query side

- `atmo_queryd` connects to MySQL read-only. It never runs SQL per tile:
  the lookup tables and signatures are held in memory and reloaded when
  `generation` changes (polled once a second); message rows are fetched
  per (run, signature) and cached in an LRU.
- `generation` replaces the manifest sequence number as the grid data
  version in responses and export cache keys.
- API (additions to query-api-v1.md):
      GET /v1/grib/centers
      GET /v1/grib/models?center=&subcenter=
      GET /v1/grib/grids?model=
      GET /v1/grib/runs?model_grid=          (base times, with members)
      GET /v1/grib/parameters?run=          (signatures present + names)
      GET /v1/grib/levels?run=&signature=
      GET /v1/grib/offsets?run=&signature=&level=
      GET /v1/grib/messages/{id}             (full header description)
      GET /v1/grib/messages/{id}/point?lat=&lon=
      GET /v1/grib/messages/{id}/tiles/{z}/{x}/{y}.png?cmap=
  The first seven are the LEADS-style drill-down the web selector uses. The
  existing /v1/point and /v1/tiles (by model/variable/level/time) resolve
  through the catalog to a message id and stay for products that are in
  the variable registry.
- The web app can also read the same tables with Doctrine (read-only
  entities) for pages that do not need the engine.

## Other imagery

Radar, satellite and similar follow the same pattern later: a catalog row
per image with its own identity signature and a location definition; only
the decoder differs. Observations stay on the k-d tree path.

## New dependency

A MySQL client library (the server is MySQL 8.0.19+, Q14; CMake takes
`libmysqlclient-dev` or MariaDB Connector/C `libmariadb-dev`), linked only into the ingest and query daemons and a
new `atmo_catalog` library target; `libatmo` core and the obs path still
build without it (`ATMO_WITH_MYSQL=AUTO|ON|OFF`). Connection settings
come from a file readable only by the service user, not from the web
app's .env.

## Questions (all decided)

Q1 [decided 2026-11-11] Model = generating process id; grid is the tier
   below it. See "Hierarchy".
Q2 [decided 2026-11-11] Interval length is part of the parameter
   (signature step 4). Confirmed with a LEADS user the same day
   (mockups: separate parameters preferred over an extra Interval list).
Q3 [decided 2026-11-11] Ensemble members are different runs: member
   column on grib_run, member keys on the deny list.
Q4 [decided 2026-11-11] Unknown parameters are shown and served (native
   units, generic label from discipline/category/number, marked as
   undefined) so users can see that a definition needs fixing.
