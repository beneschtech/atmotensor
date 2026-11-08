# Query API (HTTP/JSON + binary tiles, served over FastCGI) - first sketch

> The client-facing contract (what is Stable vs Draft, exact parameters and
> response shapes) is `docs/api/query-api-v1.md`. Tiles are PNG with
> server-side colormaps (Q12), replacing the float32 tile sketch below.

Principles
- The query daemon is a FastCGI responder behind nginx (decision D8). nginx
  owns TLS, authentication, connection limits, rate limiting, request-size
  limits, compression and (optionally) response caching. The daemon never
  speaks TLS and never faces customers directly; it trusts the FastCGI
  parameters nginx passes (e.g. `ATMO_CUSTOMER`) and only listens on a unix
  socket (file permissions are the access control) or 127.0.0.1.
- Times ISO 8601 UTC; units SI in JSON, optional `?units=` conversion for
  common aviation/US units at the edge only.
- Every response carries `X-Atmo-Snapshot`. `?as_of=` (receipt time or
  sequence) reproduces an earlier observation view; `?snapshot=` pins one.
- Deterministic: same snapshot + same request => byte-identical response.
- Errors: JSON `{error: {code, message}}`, 4xx for caller mistakes, 503 while
  no snapshot is loaded yet.

Endpoints (v1)
- `GET /v1/point?lat&lon&alt_m|level_hpa&time&fields=...&models=...&obs=on&obs_radius_km=`
  grid values (interpolated) + nearby observations.
- `POST /v1/route` body: `[{lat, lon, alt_m|fl, time}, ...]` (or sampling spec:
  polyline + speed + departure). Returns per-sample grid values and corridor obs.
- `GET /v1/tiles/{product}/{run}/{valid}/{z}/{x}/{y}.bin` web-mercator tile;
  16-byte header (magic, w, h, dtype, flags) + w*h float32 (little-endian),
  NaN = no data. Raster PNG/COG variants are a later add-on; confirm with the
  MapLibre front-ends what they need.
- `GET /v1/obs?bbox&source&from&to` observation listing, with raw text.
- `GET /v1/meta` models, runs, valid times, fields, retention, snapshot.
- `GET /healthz`

## FastCGI front end

Implementation: a small in-house FastCGI **responder** (FastCGI spec 1.0,
responder role only), in `include/atmo/fcgi.h`, no external library. The
protocol is 8-byte record headers (version 1, type, request id, content length,
padding length) with BEGIN_REQUEST / PARAMS / STDIN in, STDOUT / STDERR /
END_REQUEST out. Fallback if that proves troublesome: link libfcgi/fcgi2 from the
distro behind the same interface.

Behaviour
- Listen on a unix socket (default `/run/atmo/query.sock`, mode 0660, group set
  by config) and optionally `127.0.0.1:<port>`. Systemd socket activation
  welcome but optional.
- nginx sends one request at a time per connection and does not multiplex, so:
  answer `FCGI_GET_VALUES` with `FCGI_MPXS_CONNS=0`; if a second request id
  arrives while one is active on a connection, reply `FCGI_CANT_MPX_CONN`.
  Honour `FCGI_KEEP_CONN` (keep the connection for the next request) and
  `FCGI_ABORT_REQUEST`.
- Worker thread pool; bounded queue. Over the limit => 503 immediately
  (`Retry-After`). Per-request deadline (default 10 s) => 504. Max request body
  (default 1 MiB; `/v1/route`) and max PARAMS size enforced in the daemon too.
- Responses: `Status: 200 OK`, `Content-Type`, `X-Atmo-Snapshot`, body on STDOUT.
  Tiles for a completed (run, valid time) are immutable, so send
  `Cache-Control: public, max-age=...` and a stable `ETag`; nginx can cache
  them with `fastcgi_cache`. Point/route/obs responses depend on the live
  snapshot: `Cache-Control: no-store` unless `?snapshot=` is given.
- Logs to stderr (journald); FCGI_STDERR is only for per-request diagnostics.
- SIGHUP reloads config; SIGTERM drains in-flight requests then exits.

Illustrative nginx side (untested sketch):

    upstream atmo_query { server unix:/run/atmo/query.sock; keepalive 32; }
    limit_req_zone $http_authorization zone=atmo:10m rate=20r/s;
    location /v1/ {
        limit_req zone=atmo burst=40 nodelay;
        client_max_body_size 1m;
        include fastcgi_params;
        fastcgi_param ATMO_CUSTOMER $http_x_customer_id;   # set by your auth layer
        fastcgi_keep_conn on;
        fastcgi_read_timeout 15s;
        fastcgi_pass atmo_query;
    }

Tests for the responder (no nginx required): hand-built record streams parsed
correctly when split at every byte boundary; name/value length encodings (1-byte
and 4-byte with high bit); PARAMS and STDIN split across many records;
zero-length terminating records; padding; sequential requests on a kept-alive
connection; abort mid-request; second concurrent request id => CANT_MPX_CONN;
garbage and truncated input never crash (fuzz with random bytes under ASan).
If `nginx` is installed, an integration test proxies a request through a
temporary config on a loopback port and checks status, headers and body (skip
otherwise).
