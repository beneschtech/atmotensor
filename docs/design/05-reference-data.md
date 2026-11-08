# Reference data: web app (MySQL) -> engine (files)

Decision D10, option B (2026-11-05). This is the contract between the
Symfony web app (`../web`) and the engine; keep both sides in step with it.

## Who owns what

- **The web app owns MySQL.** The schema is defined by its Doctrine
  migrations. Admins edit reference data there: source classes, stations,
  customers and API keys, later the variable registry and model/product
  config.
- **The engine never connects to MySQL.** It has no MySQL client
  dependency. It reads files that the web app exports, and publishes them
  through the manifest (02-manifest-and-processes.md), so every query
  snapshot pins exactly one version of each.
- **Customer authentication** is done by nginx `auth_request` calling a
  web-app endpoint that checks the API key (with its own cache). Query
  daemons never see keys or customer rows.

## Export rules (web app side)

- Files are written to `/opt/atmotensor/data/config/`, atomically: write a temp
  file in the same directory, fsync, rename over the target. A reader must
  never see a partial file.
- Every file is a JSON object with `"format"` and `"version"` keys first.
  The engine rejects a file whose format or version it does not know. Other
  top-level keys (`exported_at`, `exported_by`, ...) are informational.
- Exports are complete snapshots, not deltas.
- Validation rules below are enforced on both sides: in the web app for
  good error messages, and again by the engine, which keeps the previous
  version if a new export is invalid.

## config/sources.json

    {
      "format": "atmo-sources", "version": 1,
      "exported_at": "2026-11-05T12:00:00Z",
      "sources": [
        { "name": "metar", "bucket_seconds": 3600, "L_km": 100,
          "tau_seconds": 3600, "vertical_scale": 10,
          "retain_seconds": 259200 }
      ]
    }

Every field of every class is required; unknown keys are errors.

| Field | Type | Rule |
|---|---|---|
| name | string | `[a-z][a-z0-9_-]*`, at most 31 characters, unique; becomes a directory name |
| bucket_seconds | integer | > 0 |
| L_km | number | finite, > 0 |
| tau_seconds | number | finite, > 0 |
| vertical_scale | number | finite, >= 0 |
| retain_seconds | integer | > 0 |

Seed values (first migration) are `SourceRegistry::defaults()`: metar,
synop, sounding, pirep, aircraft (01-obs-store.md).

### Changing a metric (Q5b)

`bucket_seconds`, `L_km`, `tau_seconds` and `vertical_scale` define the
k-d tree metric. Editing them is allowed:

- New segments are written with the new metric.
- Each segment stores the metric it was built with. When a query daemon
  adopts a snapshot, any segment whose stored metric differs from the
  snapshot's source class gets its tree rebuilt in RAM from the segment's
  stored columns (lat, lon, height, obs time) with the current metric. Every
  tree a query searches then uses one metric, so results merged across
  buckets stay comparable. A change of `bucket_seconds` alone does not
  change distances, so those segments keep their prebuilt trees.
- Old segments age out within the retention window, so the rebuild cost
  disappears on its own.
- Changing `bucket_seconds` additionally means old and new buckets have
  different lengths. The index must handle both while old buckets are
  retained.

## config/stations.json (draft, for T5b)

    {
      "format": "atmo-stations", "version": 1,
      "exported_at": "2026-11-05T12:00:00Z",
      "stations": [
        { "ref": 1, "name": "Oklahoma City Will Rogers", "flags": [],
          "merged_into": null,
          "ids": [ { "type": "icao", "value": "KOKC",
                     "valid_from": null, "valid_to": null } ],
          "periods": [ { "valid_from": null, "valid_to": 1577836800,
                         "lat_deg": 35.39, "lon_deg": -97.60,
                         "elevation_m": 392.0 } ] }
      ]
    }

- Times are unix seconds (integers); `null` means open-ended. `valid_to`
  is exclusive.
- `flags`: any of `"provisional"`, `"mobile"`.
- `ref`: refs the web app creates are in [1, 2^31). Provisional stations
  that came from ingest keep their ref from [2^31, 2^32), also after an
  admin confirms them.
- `merged_into`: `null`, or the ref of the station this one duplicates.
  The target must exist and must not itself be merged.
- `ids[].type`: `icao` (4 characters, A-Z 0-9), `wmo` (5 digits), `wigos`,
  or `platform` (1-31 printable characters, no spaces).
- Every rule in `include/atmo/stations.h` applies: refs are never
  reused or removed; periods are sorted and do not overlap; one identifier
  is never bound to two stations at the same time; a fixed station
  (neither flag) needs at least one period.
- Ingest turns the export into the next `stations-<gen>.tbl` generation
  (`RefDataImporter`, T5b), starting from the previous generation:
  stations in the export replace theirs; a web-app ref missing from the
  export rejects the export (refs are never removed); a provisional ref
  missing from it is kept (the web app has not imported it yet); the
  export may only contain provisional refs that ingest assigned. An export
  equal to the current generation (identifier order ignored) publishes
  nothing. An invalid export is rejected as a whole and the previous
  version stays published (Q8).

## Write-back: provisional stations (Q7)

When ingest decodes a report from an unknown station it creates a
provisional station instead of dropping the report, and tells the web app
so an admin can confirm or merge it.

- Refs come from two disjoint ranges, so neither side has to ask the other:
  the web app assigns [1, 2^31) (e.g. AUTO_INCREMENT on its own table, kept
  below 2^31), ingest assigns [2^31, 2^32) counting up from the provisional
  high-water mark stored in each station generation. Within a range refs
  only grow and are never reused.
- Ingest writes new provisional stations to
  `/opt/atmotensor/data/config/outbox/provisional-<gen:016x>.json`, one file per
  station generation that has any (format below). The web app imports it,
  keeping the ref, and deletes the file. The engine never deletes outbox
  files.
- In the admin, a provisional station is either confirmed (the flag is
  cleared, the ref stays) or merged into an existing station (it stays as
  an alias with `merged_into` set, so reports that point at it still
  resolve). It is never deleted.

### Outbox file format (T5b)

    {
      "format": "atmo-provisional-stations", "version": 1,
      "generation": 12,
      "stations": [
        { "ref": 2147483648, "name": "unknown WDC1234",
          "flags": ["provisional", "mobile"], "merged_into": null,
          "ids": [ { "type": "platform", "value": "WDC1234",
                     "valid_from": null, "valid_to": null } ],
          "periods": [] }
      ]
    }

- Station objects are exactly those of `config/stations.json`.
- A file lists every provisional station of that generation that the last
  valid `stations.json` does not contain yet, so a station can appear in
  several files (also after an ingest restart, which re-announces pending
  stations if the current generation's file is missing). **The import must
  be idempotent by ref**: skip refs that already exist.
