# AtmoTensor Query API v1 — contract

The promise between the engine (`atmo_queryd`) and its clients: the web app
(`/opt/atmotensor/src/web`, MapLibre front end) and later customers. Also the
reference for setting up nginx (see `deploy/README.md`).

Every endpoint is marked:

- **Stable**: implemented and tested. Within v1 it only changes by adding
  optional parameters or response keys; clients must ignore keys they do not
  know.
- **Draft**: designed, not built yet. The shape may still change until it is
  marked Stable. Build against it only behind a feature switch.

Engine design docs behind this: `docs/design/04-query-api.md`; open decisions
in `docs/QUESTIONS.md` (Q11 point details, Q12 tiles/colormaps: decided).

## 1. Transport

- Served by nginx on the web app's host (`atmotensor.beneschtech.cloud`), path
  prefix **`/v1/`**, plus `/healthz`. Same origin as the web app, so no CORS.
  The web app's own endpoints stay under `/api/...`.
- nginx owns TLS, authentication, rate limits and request-size limits. The
  daemon listens on a unix socket only.
- Requests are `GET` (and `HEAD`); `POST` only where stated.
- Responses are JSON (`application/json; UTF-8`) unless stated (tiles: PNG).

### Conventions

| Topic | Rule |
|---|---|
| Times | ISO 8601 UTC: `2026-11-08T18:00:00Z`. Inputs may omit seconds and/or `Z` (`2026-11-08T18:00`); always UTC. |
| Units | SI in every response (K, Pa, m/s, m, kg/m², fractions 0–1 rather than percent). Each value carries its `units` string. Unit conversion for display is the client's job (a `?units=` option may come later). |
| Missing data | `null` (never NaN or a sentinel number). |
| Numbers | Gridded and observed values are stored as float32 and printed at that precision (`290.94`, not `290.94000244140625`). |
| Lists in query strings | Comma separated: `fields=a,b,c` (names and specs never contain commas). `@` may be sent raw or as `%40`. |
| Determinism | The same request on the same snapshot returns byte-identical bodies. |

### Snapshots

All data is published in numbered, immutable **snapshots**. Every successful
response has the header `X-Atmo-Snapshot: <seq>`.

- Without `snapshot=` a request uses the newest snapshot; such responses have
  `Cache-Control: no-store`.
- `snapshot=<seq>` (any endpoint) pins an older retained snapshot; such
  responses never change and have `Cache-Control: public, max-age=86400`.
  A client that needs several consistent requests (e.g. a map view plus a
  point readout) should read `X-Atmo-Snapshot` from the first and pin the
  rest to it.
- `404 unknown_snapshot` if that snapshot is no longer retained;
  `410 snapshot_gone` if it is retained but its data files have expired.

### Errors

Status code plus a JSON body:

```json
{"error": {"code": "bad_request", "message": "missing time"}}
```

| Status | `code` | When |
|---|---|---|
| 400 | `bad_request` | A parameter is missing or malformed; `message` says which. |
| 404 | `not_found` | Unknown endpoint. |
| 404 | `unknown_snapshot` | `snapshot=` not retained. |
| 405 | `method_not_allowed` | Wrong method. |
| 410 | `snapshot_gone` | Pinned snapshot's files expired. |
| 413 / 431 | `body_too_large` / `params_too_large` | Request too large. |
| 500 | `internal` | Engine bug. |
| 503 | `no_snapshot` | Daemon up, nothing published yet. |
| 503 | `overloaded` | Queue full; has `Retry-After: 1`. Retry with backoff. |
| 504 | `timeout` | Request waited too long in the queue. |

## 2. Concepts

**Model** — a gridded data source by name (`gfs`, `hrrr`, `rrfs`, `rap`,
`ecmwf`, ...). **Run** — the model's reference time. **fhr** — forecast hour;
valid time = run + fhr hours.

**Field spec** — a variable at a level: `name[@level]`.

- `name` is a variable from the registry (`/v1/meta` lists what is stored;
  the full registry is `docs/design/variables.csv` in the engine repo), e.g.
  `air_temperature`, `wind_u`, `wind_v`, `relative_humidity`,
  `geopotential_height`, `pressure_msl`, `precipitation_amount`, `cape`,
  `visibility`.
- `@level`:

| Form | Meaning | Example |
|---|---|---|
| `@sfc` | ground or water surface | `cape@sfc` |
| `@msl` | mean sea level | `pressure_msl@msl` |
| `@<x>m` | x metres above ground | `air_temperature@2m`, `wind_u@10m` |
| `@<x>hPa` | isobaric surface | `air_temperature@850hPa` |
| `@L<type>=<value>` | any GRIB2 level type (code table 4.5), value in SI | `relative_humidity@L104=0.44` |

- Without `@level` the request's vertical coordinate decides (see
  `/v1/point`).
- Winds are always returned **earth-relative** (`wind_u` = eastward,
  `wind_v` = northward), whatever the model stores.

## 3. Endpoints

### GET /healthz — Stable

`200 text/plain` `ok snapshot 12` when serving; `503` `no snapshot` before
the first publish. For load balancers and monitoring.

### Names of models and fields (with the grid catalog) — Draft (T17c)

When the daemon runs with `--catalog`, `/v1/meta`, `/v1/point` and
`/v1/tiles` read gridded data from the grid catalog (observations are
unchanged):

- **Model names** are model + grid names (`api_name`, set in the web app,
  e.g. `gfs_0.25p`); `/v1/meta` lists them. Only deterministic runs
  (member −1) on viewable grids are used.
- **Field names** (the part of a field spec before `@level`) are, in this
  order: a *common name* from the web app (e.g. `air_temperature`, seeded
  from the old variable list so existing specs keep working), or the
  parameter's name lower-cased with spaces as underscores
  (`temperature`, `total_precipitation_3hr`). `/v1/meta` and the
  responses use the common name if the parameter has one, else the
  normalised name. Units are the parameter's (see the catalog section).
- **Matching** (every name argument, case-insensitive):
  1. an exact name is used silently;
  2. otherwise names *containing* the text are candidates (models: model
     names, then model slugs; fields: common names, then parameter
     names): exactly one → used as a **best guess**, reported in
     `warnings`; several → `400 ambiguous_name` with up to 20
     `candidates` in the error body; none → `404 not_found`.
  Two parameters with the same normalised name (e.g. NCEP-local and WMO
  "categorical rain") → `400 ambiguous_field`; give one a common name.
- **Warnings**: JSON responses carry
  `"warnings": [{"code": "best_guess", "argument": "models", "asked": "gfs",
  "used": "gfs_0.25p", "message": "..."}]` and use the resolved name in
  the body. Tiles carry the same as an `X-Atmo-Warning` header per guess
  and are `Cache-Control: no-store` when a guess was used. Clients that
  pick names from `/v1/meta` or `/v1/grib/tree` never see a guess.
- Times: forecast hours are whole hours; sub-hourly offsets (RRFS
  15-minute output) are only reachable through `/v1/grib/*` for now.
- First-bucket repair: a fixed-length accumulation (e.g.
  `total_precipitation_1hr`) at its first hour (f01) has no message of its
  own in GFS-style output, where the 0–1 h interval is labelled "since run
  start"; the run-total message for exactly that interval is used.
- A parameter present more than once at the same level and time in a run
  (two different signatures) → value status `ambiguous_field` (point),
  `409 ambiguous_field` (tiles).
- Tiles from the catalog are `Cache-Control: no-cache` with an `ETag` and
  `X-Atmo-Generation` (a newer copy of a message can replace the data of
  a run and hour); `immutable` stays for snapshot-only deployments.
- A snapshot is still needed (observations, `snapshot` in responses):
  without one, `/v1/meta` and `/v1/point` answer `503 no_snapshot`.

### GET /v1/meta — Stable

What is available. Parameters: `snapshot` (optional).

```json
{
  "snapshot": 12,
  "created_at": "2026-11-08T19:02:11Z",
  "models": [
    {
      "name": "gfs",
      "runs": [
        {"run": "2026-11-08T12:00:00Z", "fhr": [0, 1, 2, 3]},
        {"run": "2026-11-08T18:00:00Z", "fhr": [0, 1]}
      ],
      "fields": ["air_temperature@2m", "air_temperature@500hPa", "wind_u@10m"]
    }
  ],
  "obs_sources": [
    {"name": "metar", "retain_seconds": 259200, "L_km": 100, "tau_seconds": 3600}
  ],
  "stations_generation": 3
}
```

- `models` sorted by name; `runs` oldest first; `fields` sorted, listed for
  the newest (run, fhr) of the model.
- Fields are in canonical spec form; pass them back verbatim.

### GET /v1/point — Stable

Gridded values at one place and time from every (or chosen) model, plus
nearby observations.

| Parameter | Required | Meaning |
|---|---|---|
| `lat`, `lon` | yes | Degrees; lon may be −180..180 or 0..360. |
| `time` | yes | Valid time (ISO 8601 UTC). Required so responses are reproducible; send the time the UI shows. |
| `fields` | yes | Comma-separated field specs. |
| `models` | no | Comma-separated model names; default all. Unknown name → 400. |
| `level_hpa` | no | Vertical coordinate for specs without `@level`: interpolated in log-pressure between isobaric levels. |
| `alt_m` | no | Instead of `level_hpa`: geometric altitude above MSL in metres, located via geopotential height. Not both. |
| `obs` | no | `on` / `off` (default `off`): include nearby observations. |
| `obs_radius_km` | no | Default 50, max 2000. A radius in each observation source's **space-time** metric: time offsets count as distance (METAR: 1 h ≈ 100 km), so the nearest-in-time reports win. |
| `as_of` | no | Receipt sequence number: observations as they were known then (replays an earlier answer). |
| `snapshot` | no | Pin a snapshot. |

Rules:

- Time: per model, the newest run whose forecast hours bracket `time`;
  linear interpolation between the two hours (never across runs, never
  extrapolated).
- Space: bilinear inside the grid cell.
- A spec without `@level` and without `level_hpa`/`alt_m` uses the field's
  only stored level, else `level_required`.

```json
{
  "snapshot": 12,
  "point": {"lat": 35.39, "lon": -97.6, "time": "2026-11-08T18:00:00Z"},
  "values": [
    {"model": "gfs", "field": "air_temperature@2m", "units": "K",
     "status": "ok", "value": 290.94, "run": "2026-11-08T18:00:00Z", "fhr": 0},
    {"model": "gfs", "field": "wind_u@10m", "units": "m/s",
     "status": "ok", "value": 2.01, "run": "2026-11-08T18:00:00Z", "fhr": 0},
    {"model": "hrrr", "field": "air_temperature@2m", "units": "K",
     "status": "no_time", "value": null}
  ],
  "obs": [
    {"source": "metar", "distance": 12.4, "station_ref": 1,
     "obs_time": "2026-11-08T17:53:00Z", "lat": 35.39, "lon": -97.6,
     "height_m": 392,
     "values": [{"var": "air_temperature", "value": 291.15, "units": "K",
                 "qc": 0, "height_m": 392}],
     "raw": "KOKC 081753Z 18010KT 10SM CLR 18/10 A3000"}
  ]
}
```

- `values`: one entry per (model, field), models in request order (or by
  name), fields in request order.
- `status`: `ok` (value may still be `null` where the model has no data at
  that point, e.g. masked), `no_time` (no run covers the time),
  `outside_grid`, `no_field` (not stored), `level_required`,
  `out_of_levels` (vertical coordinate above/below the stored levels),
  `ambiguous_field` (catalog only, see "Names").
  `run`/`fhr` are present unless the status is `no_time` or `outside_grid`;
  `fhr` is fractional when interpolated in time.
- `obs`: ordered by source, then distance. `distance` is in the source's
  space-time metric (km). `raw` is the original message text.

### GET /v1/tiles/{model}/{field}/{run}/{fhr}/{z}/{x}/{y}.png — Stable

Rendered map tiles for MapLibre: `raster` source, XYZ scheme, Web
Mercator, 256 × 256 px, PNG RGBA. Colours are assigned on the server from
the colormap in the request; the client only stacks layers
(`raster-opacity`).

| Part | Meaning |
|---|---|
| `{model}` | From `/v1/meta`. Unknown → `404 not_found`. |
| `{field}` | Field spec **with** an explicit level, e.g. `air_temperature@2m`, `wind_u@850hPa` (`@` raw or `%40`). The level must be stored (no vertical interpolation in tiles). Without `@level` → 400. |
| `{run}` | `YYYYmmddHH` (UTC), e.g. `2026110818`, from `/v1/meta` runs. |
| `{fhr}` | Integer forecast hour of that run. |
| `{z}/{x}/{y}` | Tile address, `z` 0–24, `x`,`y` < 2^z. |
| `cmap` (query, required) | The colormap, see below. |
| `nodata` (query, optional) | `RRGGBB` or `RRGGBBAA` for missing values on the grid. Default: transparent. |
| `snapshot` (query, optional) | As everywhere. |

**Colormap syntax** (`cmap`): stops separated by segment types,

    stop,type,stop[,type,stop]...      2 to 64 stops, values strictly increasing
    stop = <value>:<RRGGBB or RRGGBBAA>   value in the field's SI units (see /v1/meta)
    type = lin | log | exp | step

Example (2 m temperature, K):
`cmap=233.15:2c0f60,lin,253.15:3b82f6,lin,273.15:ffffff,lin,293.15:f59e0b,lin,313.15:7f1d1d`

- Between two stops a and b, with u = (v − a)/(b − a), the colour is the
  start colour mixed toward the end colour by t (per RGBA channel):
  `lin` t = u; `log` t = log10(1 + 9u) (more detail at the low end);
  `exp` t = (10^u − 1)/9 (more detail at the high end); `step` t = 0
  (the start colour up to the next stop).
- **Below the first stop: the first colour. Above the last stop: the last
  colour.** Anything else (e.g. transparent outside a range) goes into the
  colormap itself, e.g. a transparent first stop with `step`.
- Missing values on the grid get `nodata`. Pixels off the model's grid are
  always transparent.
- More than 64 stops, or any syntax error → `400 bad_request` with the
  reason.
- The colormap travels with every request; the engine stores none. Per-user
  and per-product colormaps live in the web app, which writes them into the
  tile URLs. Equal URLs are equal images, so they cache well.

Responses:

- `200 image/png` for a tile with data, and also for a tile entirely
  outside the model's grid (fully transparent), so a regional model at low
  zoom does not produce errors.
- `Cache-Control: public, max-age=31536000, immutable` and an `ETag`;
  `If-None-Match` with that ETag → `304` without a body.
- `404 no_time` (no such run/fhr), `404 no_field` (field not stored at that
  level), `404 not_found` (unknown model or malformed path).
- Winds are earth-relative (grid-relative u/v are rotated per pixel).

MapLibre source:

```js
map.addSource('gfs-t2m', {
  type: 'raster', tileSize: 256,
  tiles: ['/v1/tiles/gfs/air_temperature@2m/2026110818/0/{z}/{x}/{y}.png'
          + '?cmap=' + encodeURIComponent(userColormap)],
});
```

Performance on GFS 0.25°: ~6 ms per tile once the message is decoded;
the first tile of a (field, run, fhr) also decodes it (~30–250 ms).

### Grid catalog: GET /v1/grib/... — Stable (T17, approved 2026-11-12)

Every GRIB message the engine has ingested, reachable by id (D13,
`docs/design/07-grib-catalog.md`). The first seven endpoints are the
LEADS-style drill-down the web selector uses:

    centre -> subcentre -> model -> grid -> run (base time, member)
           -> parameter -> level -> forecast offset -> message id

Only available when the daemon runs with `--catalog CONFIG`; otherwise
`404 not_found`.

Common rules:

- Ids are the catalog's integer ids, passed back verbatim. When a newer
  copy of a message (same run, parameter, level, offsets) with different
  bytes arrives ("latest wins"), it gets a **new id**; the old id then
  answers every `/v1/grib/messages/{id}...` request with `410 replaced`
  and `"replaced_by": <new id>` in the error body (always the live
  message, also after several replacements). A copy with identical bytes
  keeps its id. So a message id always means the same data; it can only
  disappear (`410` while its replacement exists, `404` once that expired).
- Every response has `X-Atmo-Generation: <n>`: the catalog's data version,
  bumped by every ingest. JSON bodies carry it as `generation`. These
  responses are `Cache-Control: no-store`, except tiles (below).
- `snapshot=` does not apply here.
- Lists are sorted as stated; times as everywhere (ISO 8601 UTC); offsets
  are seconds after the base time.
- Unknown parameters (no name yet) are listed and served like any other:
  `status: "unknown"`, a generic name `Unknown <discipline>/<category>/<number>`,
  values in the message's native GRIB units, `units` as eccodes reports
  them (often `"unknown"`).
- Names: `name`, `short_name` and `units` are the web app's
  grib_parameter row. The engine's defaults are the level-independent
  GRIB2 code table 4.2 name and units ("Temperature", "K" for 2 m and
  850 hPa alike), plus the interval and statistic of a statistical
  parameter ("Total precipitation 3hr", "Wind speed maximum 1hr");
  `short_name` is `""` until the web app sets one.
- `viewable: false` marks a grid the engine cannot display natively
  (reduced Gaussian, spectral, ...). Its messages are listed like any
  other and can be downloaded (raw and decoded values, below); point and
  tiles on them answer `409 not_sampleable`.
- `api_name` (on a model + grid) is the model name `/v1/point` and
  `/v1/tiles` accept, e.g. `gfs_0.25p`: unique, `[a-z0-9_.-]+`, set by the
  web app; until then a long default `<model name or slug>_<grid name>`.
- Model aliases: files whose (centre, subcentre, process id) the web app
  maps to another model are listed under that model (e.g. GFS analysis
  files, process 81, under the GFS forecast model, so runs have hour 0);
  `message.process_id` keeps the value from the file.
- Values are converted to the parameter's units (`native * si_scale +
  si_offset`); for eccodes-named parameters that is the GRIB2 unit, which
  is SI. Winds are returned **as stored** (one component per message):
  `uv_relative_to_grid: true` on the message says they are grid-relative.

| Endpoint | Parameters | Response list (sorted by) |
|---|---|---|
| `GET /v1/grib/centers` | — | `centers`: `{id, code, name, subcenters: [{id, code, name}]}` (code) |
| `GET /v1/grib/models` | `center`, `subcenter` (ids, optional filters) | `models`: `{id, center, subcenter, process_id, slug, name}` (center code, subcenter code, process id) |
| `GET /v1/grib/grids` | `model` (required) | `grids`: `{id, name, api_name, grid: {key, type, ni, nj, periodic_i, bbox: [w, s, e, n], viewable}}` (name) |
| `GET /v1/grib/runs` | `model_grid` (required) | `runs`: `{id, basetime, member, member_type}` (newest base time first, then member) |
| `GET /v1/grib/parameters` | `run` (required) | `parameters`: `{signature, name, short_name, units, status, stat_interval_s, base_signature, discipline, category, number, pdt}` (name) |
| `GET /v1/grib/levels` | `run`, `signature` (required) | `levels`: `{level1: {type, value}, level2: {type, value} or null}` (type, value) |
| `GET /v1/grib/offsets` | `run`, `signature`, `level` (required), `level2` | `offsets`: `{message, offset_start_s, offset_end_s, valid_time, from_run_total}` (offset_end_s) |

- `signature` is the 16-hex-digit parameter signature from
  `/v1/grib/parameters`; `base_signature` ties together the interval
  siblings of a statistical parameter (1 h, 3 h, run total).
  `stat_interval_s`: `null` instantaneous, `0` since run start, else the
  bucket length.
- `level` / `level2` are `<type>:<value>` (code table 4.5 type, value in
  SI), e.g. `level=100:85000`. `level2` omitted = no second surface.
- **First-bucket repair**: the first bucket of a run (0–1 h at f01, 0–3 h
  at f03) starts at the base time, so its header calls it "since run
  start". `offsets` of a fixed-length parameter of N seconds therefore
  also lists the run-total sibling's message whose interval is exactly
  0..N, with `from_run_total: true`.
- Unknown `run` / `model_grid` / `model` id → `404 not_found`; a malformed
  one → `400 bad_request`.

`GET /v1/grib/messages/{id}` — everything about one message:

```json
{
  "generation": 41,
  "message": {
    "id": 123456, "run": {"id": 9, "basetime": "2026-11-08T18:00:00Z", "member": -1, "member_type": null},
    "model": {"id": 3, "slug": "7-0-96", "name": "", "process_id": 96},
    "center": {"code": 7, "name": ""}, "subcenter": {"code": 0, "name": ""},
    "grid": {"key": "a1b2c3d4e5f60718", "type": 0, "ni": 1440, "nj": 721,
             "periodic_i": true, "bbox": [-180, -90, 179.75, 90], "viewable": true,
             "name": "0.25DEG LL WORLDWIDE", "api_name": "gfs_0.25p"},
    "process_id": 96,
    "parameter": {"signature": "0011223344556677", "name": "Temperature",
                  "short_name": "", "units": "K", "status": "identified",
                  "stat_interval_s": null, "canonical": "v2\ncentre=7\n..."},
    "level1": {"type": 100, "value": 85000}, "level2": null,
    "offset_start_s": 21600, "offset_end_s": 21600, "valid_time": "2026-11-09T00:00:00Z",
    "file": {"path": "grib/gfs/2026110818/gfs.t18z.pgrb2.0p25.f006",
             "byte_offset": 1234567, "byte_length": 712345},
    "content_hash": "8899aabbccddeeff", "flags": [], "sampleable": true
  }
}
```

`flags` lists `undecodable` and/or `grid_unsupported`; such messages have
`sampleable: false` and their point / tile requests answer
`409 not_sampleable`.

**Downloads** (any message, including those on grids that cannot be
displayed natively; `X-Atmo-Generation` as usual):

- `GET /v1/grib/messages/{id}/raw` — the GRIB2 message exactly as stored
  (`application/x-grib2`, `Content-Disposition: attachment;
  filename="<id>.grib2"`). `ETag` = content hash, `Cache-Control:
  no-cache` (`v=<content_hash>` → immutable, as for tiles).
- `GET /v1/grib/messages/{id}/values` — the decoded values in the file's
  own point order (scan order of the message, not reordered),
  little-endian float32, `NaN` where missing, converted like `point`
  (`application/octet-stream`). Headers: `X-Atmo-Count` (number of
  values), `X-Atmo-Units`, `X-Atmo-Grid` (grid id). Place them with
  `/v1/grib/grids/{id}` or the raw message. `409 not_sampleable` only for
  undecodable messages; `503 no_decoder` without eccodes.
- `GET /v1/grib/grids/{id}` — `grid`: `{id, key, type, ni, nj,
  periodic_i, bbox, viewable, summary}`; `summary` is the engine's
  description (eccodes `gridType`, increments, projection keys).

`GET /v1/grib/tree?depth=&parameters=` — the whole drill-down in one
call, from the in-memory tables (so clients can pick exact names):
`centers[] -> subcenters[] -> models[] -> grids[] -> runs[]`, where a
grid is `{id, name, api_name, viewable, grid_id}` and a run
`{id, basetime, member, member_type, parameters (count), first_offset_s,
last_offset_s}`. `depth=centers|models|grids|runs` (default `runs`) stops
early; `parameters=1` adds each run's `parameter_list`:
`{signature, name, field, common_names}` (`field` = the name `/v1/point`
uses). Sorted like the single-level lists.

`GET /v1/grib/messages/{id}/point?lat=&lon=` — bilinear value at one
place:

```json
{"generation": 41, "message": 123456, "lat": 35.39, "lon": -97.6,
 "units": "K", "status": "ok", "value": 281.25, "uv_relative_to_grid": false}
```

`status`: `ok` or `outside_grid` (value `null`); `value` may be `null`
where the message has no data (bitmap).

`GET /v1/grib/messages/{id}/tiles/{z}/{x}/{y}.png?cmap=&nodata=[&v=]` —
map tile of one message, same rendering, colormap syntax and status
codes as `/v1/tiles`. `cmap` values are in the units `point` reports.
Because a message id can be replaced by a newer copy, tiles are
`Cache-Control: no-cache` with an `ETag` (revalidate with
`If-None-Match` → `304`); pass `v=<content_hash>` from the message
details to get `Cache-Control: public, max-age=31536000, immutable`
instead (a `v` that no longer matches → `404 not_found`).

### POST /v1/route — Draft

Values along a flight route: body `[{"lat", "lon", "alt_m" | "fl", "time"}, ...]`
or a polyline + speed + departure. Same per-sample `values` as `/v1/point`,
plus corridor observations. Body ≤ 1 MiB.

### GET /v1/obs — Draft

Observation listing: `bbox=w,s,e,n`, `source`, `from`, `to`; reports with raw
text. All observation kinds (surface, aviation, later road sensors) share one
report format, distinguished by `source`.

## 4. Client checklist (web app)

1. On load: `GET /v1/meta`; remember `snapshot`; build model/run/field menus
   from it.
2. Pin follow-up requests for one view with `snapshot=<seq>`; refresh `meta`
   periodically (e.g. every 60 s) and move to the new snapshot when it changes.
3. Always send `time`; show `status` reasons for null values instead of
   blanks.
4. Treat 503 `overloaded` with backoff; show 503 `no_snapshot` as "engine
   starting".
5. Ignore unknown JSON keys.
