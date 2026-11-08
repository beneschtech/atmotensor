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
in `docs/QUESTIONS.md` (Q11 point details, Q12 tiles/colormaps).

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
  `out_of_levels` (vertical coordinate above/below the stored levels).
  `run`/`fhr` are present unless the status is `no_time` or `outside_grid`;
  `fhr` is fractional when interpolated in time.
- `obs`: ordered by source, then distance. `distance` is in the source's
  space-time metric (km). `raw` is the original message text.

### GET /v1/tiles/{model}/{field}/{run}/{fhr}/{z}/{x}/{y}.png — Draft

Rendered map tiles for MapLibre (`raster` source, XYZ scheme, Web Mercator,
256 × 256 px).

- `{field}`: a field spec with an explicit level (`air_temperature@2m`).
- `{run}`: `YYYYmmddHH` (`2026110818`), from `/v1/meta`.
- `{fhr}`: integer forecast hour.
- Query `colormap=<name>` (default: the variable's default colormap).
- PNG, RGBA. Pixels with no data (outside the grid, masked) are fully
  transparent. Colours come from the colormap: the engine assigns colours,
  the client only stacks layers (`raster-opacity`).
- A (run, fhr) tile never changes once published: `Cache-Control: public,
  max-age=31536000, immutable` and a stable `ETag`; nginx may cache them.
- `404 no_field` / `no_time` for data that does not exist.

MapLibre source example (when Stable):

```js
map.addSource('gfs-t2m', {
  type: 'raster', tileSize: 256,
  tiles: ['/v1/tiles/gfs/air_temperature@2m/2026110818/0/{z}/{x}/{y}.png?colormap=temperature'],
});
```

### GET /v1/colormaps and /v1/colormaps/{name} — Draft

The colour scale behind tiles, for legends (decision D11: the engine returns
the colour scale as data).

```json
{"name": "temperature", "units": "K",
 "stops": [{"value": 233.15, "color": "#2c0f60"}, {"value": 313.15, "color": "#ffef9c"}],
 "interpolation": "linear", "below": "#2c0f60", "above": "#ffef9c"}
```

Colormaps are defined in the web app (product manifest) and exported to the
engine like the other reference data (`config/colormaps.json`, format to be
fixed in Q12).

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
