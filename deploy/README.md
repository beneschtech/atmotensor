# Deployment (development box)

Layout on the host:

| Path | What |
|---|---|
| `/opt/atmotensor/data` | the engine store (`--root`): grids/, fields/, obs/, stations/, manifest/, config/ |
| `/opt/atmotensor/tmp` | scratch for downloads and staging (same XFS volume as data) |
| `/opt/atmotensor/ingest/<source>` | incoming feeds, e.g. `ingest/grib/` |
| `/opt/atmotensor/src/atmotensor` | this repo; binaries in `build-cmake/` |
| `/opt/atmotensor/src/web` | the web app (Symfony + MapLibre) |

The API the web app and customers use is specified in
`docs/api/query-api-v1.md`.

## 1. Build

    cmake --preset default && cmake --build --preset default -j

## 2. Put data in (until the ingest daemon exists)

    ./build-cmake/atmo_grib_ingest --root /opt/atmotensor/data --model gfs \
        --seed-sources /opt/atmotensor/ingest/grib/gfs.t18z.pgrb2.0p25.f000

Each file is kept as published (hard-linked into `data/grib/<model>/<run>/`,
so no extra disk) next to a small index; queries decode messages on demand
into the daemon's cache. `--seed-sources` writes the default observation
source classes to `data/config/sources.json` once; later the web app
exports that file.

## 3. Query daemon (systemd)

    sudo cp deploy/atmo-queryd.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable --now atmo-queryd
    journalctl -u atmo-queryd -f

Memory: decoded GRIB messages are cached up to `--cache-mb` (default
4096); when the process RSS passes `--rss-limit-mb` (default 2 x cache +
1024) the oldest unused messages are dropped. `systemctl reload` logs the
cache statistics.

It listens on `/run/atmo/query.sock` (group www-data, mode 0660), follows
new snapshots automatically, drains on `systemctl stop`, re-reads the
manifest on `systemctl reload`.

## 4. nginx

1. Upstream at http level, e.g. `/etc/nginx/conf.d/atmo-upstream.conf`:

        upstream atmo_query {
            server unix:/run/atmo/query.sock;
            keepalive 32;
        }

2. In the `server` block of `/etc/nginx/sites-available/atmotensor`, before
   the `location /` block:

        include /opt/atmotensor/src/atmotensor/deploy/nginx-atmo-engine.conf;

3. `sudo nginx -t && sudo systemctl reload nginx`

Check:

    curl -s http://atmotensor.beneschtech.cloud/healthz
    curl -s http://atmotensor.beneschtech.cloud/v1/meta | head -c 400
    curl -s 'http://atmotensor.beneschtech.cloud/v1/point?lat=35.39&lon=-97.6&time=2026-11-08T18:00:00Z&fields=air_temperature@2m'

There is no authentication on `/v1/` yet (the web app has no accounts
either). Before the host is exposed beyond testing, add `auth_request` to
the web app's API-key check and a `limit_req` zone (see the comments in
`nginx-atmo-engine.conf`).
