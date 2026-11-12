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

First manifest (observation source classes; the web app exports
`data/config/sources.json` later):

    ./build-cmake/atmo_grib_ingest --root /opt/atmotensor/data --seed-sources

GRIB files go into the grid catalog (below). Per downloaded file (one
forecast hour), ingest and retention in one call:

    ./build-cmake/atmo_grib_ingest --root /opt/atmotensor/data --model gfs \
        --catalog /opt/atmotensor/etc/catalog.json --expire --keep 5 \
        /opt/atmotensor/ingest/grib/gfs.t18z.pgrb2.0p25.f003

- The file is hard-linked into `data/grib/<model>/<run YYYYmmddHH>/` (no
  extra disk; download into `/opt/atmotensor/ingest` or `tmp`, same
  volume) and every message is catalogued in one transaction. The
  download can be deleted afterwards. `--model` only names that folder:
  the catalog's model comes from the GRIB (centre, subcentre, process id)
  and the web app's aliases.
- Then the newest K base times per model grid are kept, older runs and
  their files deleted. K counts a run that is still arriving, so use one
  more than the complete runs wanted (`--keep 5` = 4 complete + the
  current one); `--keep-model gfs_0.25p=3` overrides per model.
- The query daemon sees the new data within about a second (it polls the
  catalog generation); no reload or restart.
- Calls may run in parallel (they take turns on the generation row).
  Re-sending an identical file is harmless; a different file under a name
  already used in that run folder is refused.
- New kind of data (a new model or process id): in the web app, alias it
  if it belongs to an existing model (GFS f000 is process 81, the
  forecast hours 96) and run `--realias` once; give the model grid a
  short `api_name`; re-run `app:grib:seed-common-names`.

**Upgrading a store from before T18** (manifest version 1 with a `gribs`
list, as on this box until 2026-11-11): the new build reads it, retires
the `.idx.json` sidecars (unlinked by gc after the grace period) and no
longer serves those GRIB files unless they are catalogued. Catalogue them
in place, then run the query daemon with `--catalog`:

    find /opt/atmotensor/data/grib -name '*.idx.json' -prune -o -type f -print |
      while read f; do ./build-cmake/atmo_grib_ingest --root /opt/atmotensor/data \
        --model "$(echo "$f" | cut -d/ -f6)" --catalog /opt/atmotensor/etc/catalog.json "$f"; done

### Grid catalog (D13)

The catalog lives in the web app's MySQL database (`grib_*` tables, created
by its Doctrine migrations: `php bin/console doctrine:migrations:migrate`
in `../web`). The engine connects with its own settings file, readable only
by the service user (it refuses anything group/world-readable):

    install -m 600 -o atmo /dev/null /opt/atmotensor/etc/catalog.json
    # {"host": "127.0.0.1", "port": 3306, "user": "atmo_ingest",
    #  "password": "...", "database": "atmotensor"}

Ingest needs INSERT/UPDATE/DELETE/SELECT on `grib_*` plus CREATE TEMPORARY
TABLES on the database (it stages each file's messages); the query daemon only
SELECT (give it a separate read-only user and file). Then:

    ./build-cmake/atmo_grib_ingest --root /opt/atmotensor/data --model gfs \
        --catalog /opt/atmotensor/etc/catalog.json FILE...
    # query daemon: add --catalog /opt/atmotensor/etc/catalog-ro.json

Every message of the file is catalogued (one transaction per file). With
`--catalog`, the query daemon serves `/v1/point`, `/v1/tiles`, `/v1/meta`
and `/v1/grib/*` from the catalog (model names = the web app's
`api_name`, field names = common names; see the API doc); it still needs
a published manifest for observations.

The catalog needs a MySQL 8.0.19+ server (schema version 2: run the web
app's migrations first; the engine refuses any other version).

Model aliases (QUESTIONS.md Q18a): after adding, changing or removing a
`grib_model_alias` row in the web app, move what is already catalogued:

    ./build-cmake/atmo_grib_ingest --catalog /opt/atmotensor/etc/catalog.json --realias

New files use the aliases at ingest anyway. Common names for `/v1/point`
field specs (`air_temperature@2m`) come from the variable registry
(Q19c, Q21a); re-run after new models were ingested:

    (cd ../web && php bin/console app:grib:seed-common-names)

### eccodes is pinned (Q18c)

Parameter signatures are built from eccodes' key names, so eccodes must
not change with routine updates. Hold the packages (2.45.0 at the time
of writing):

    sudo apt-mark hold libeccodes0 libeccodes-dev libeccodes-data libeccodes-tools

An upgrade is a planned step: run `atmo_grib_sigcheck --all` over the
stored files before and after (`apt-mark unhold`, upgrade, rebuild); if
any signature or name changed, bump `kSignatureVersion` (gribsig.h) and
re-ingest, as for a deny-list change.

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
