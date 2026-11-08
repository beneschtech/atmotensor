# Manifest, snapshots, processes, directory layout

## Directory layout (root `/opt/atmotensor/data`, configurable)

    grids/<key>.grid                         gridstore.h (done)
    stations/stations-<gen>.tbl
    obs/<source>/<bucket_start>/<seg-id>.seg
    grib/<model>/<run YYYYmmddHH>/<file>      GRIB2 as published + <file>.idx.json (T10c)
    fields/<model>/<run YYYYmmddHH>/f<fhr:03>.fld   f32 tiles (T7; optional hot set)
    journal/<source>-<YYYYmmddHH>.jrn        raw feed journal (see below)
    manifest/MANIFEST-<seq:016x>.json        immutable
    manifest/CURRENT                         tiny file: seq + CRC, replaced by rename
    manifest/LOCK                            flock held by the one publisher
    config/sources.json                      exported by the web app (05-reference-data.md)
    config/stations.json                     exported by the web app
    tmp/                                     staging; anything here is garbage after a crash
                                             (feed downloads use /opt/atmotensor/tmp, same volume)

## Manifest

JSON, immutable once written. Contains: `seq`, `created_at`, `next_receipt_seq`,
the active source classes (copied from config/sources.json, so a snapshot
records the metric it uses), station generation, and entries for grids, field files (model, run, fhr,
grid_key, path, header CRC), obs segments (source, bucket, path, receipt_seq
min/max, report count, header CRC). Sized for ~thousands of entries.

Publish protocol (ingest daemon only; holds `manifest/LOCK`):
1. Write new data files (temp -> fsync -> rename into place).
2. Write `MANIFEST-<seq+1>.json` (fsync).
3. Write `CURRENT.tmp` (seq + CRC of the manifest), fsync, rename over `CURRENT`, fsync directory.
A reader that sees CURRENT at seq N can always read MANIFEST-N and every file it
names. Crash at any point leaves the previous state intact.

Keep the last M manifests (default 100) for debugging and `as_of` by snapshot.

## Query daemon: snapshots

- Watches `CURRENT` (inotify, with 1 s polling fallback). On change: read the
  manifest, mmap + verify new files (header CRC always; full CRC on first
  open), reuse mappings for unchanged files, build a `Snapshot`
  (`shared_ptr<const Snapshot>`: immutable maps from keys to mapped files and
  loaded trees), then swap it in atomically.
- Each request takes the current `shared_ptr` once, at the start, and uses only
  that. Response header `X-Atmo-Snapshot: <seq>`. Optional `?snapshot=<seq>`
  pins an older retained snapshot while its files still exist.
- Opening all files at adoption (not lazily) means expiry can never pull a file
  out from under an in-flight or about-to-start request.

## Retirement

Ingest removes expired/compacted files from the next manifest, waits a grace
period (default 10 min, configurable), then unlinks. Unlinking a file that a
query process still has mapped is safe on Linux (the mapping stays valid until
unmapped). A `gc` pass removes files in `tmp/` and any data file not named by
a retained manifest and older than the grace period.

Implementation (T6). The grace period runs from retirement, not from the
file's mtime (a 72 h old segment expired now still gets its 10 minutes), so
retirement is recorded in the manifest itself:

- The manifest has a `retired` list of `{path, retired_at}`.
  `ManifestPublisher::publish()` maintains it: every file the current
  manifest lists and the next one does not is added with `retired_at` =
  now; a retired file that is listed again is live again. Callers never
  edit it. This also retires replaced station generations.
- `collect_garbage(grace)`: (1) retired entries older than the grace period
  are dropped from the manifest (one publish), then unlinked; (2) any
  regular file under `grids/`, `stations/`, `fields/`, `obs/` or `tmp/`
  that the current manifest neither lists nor retires and whose mtime is
  older than the grace period is unlinked (crash leftovers, temp files);
  (3) emptied data directories are removed. `manifest/`, `config/` and
  `journal/` are never touched.
- Retention itself is `apply_retention()` (retention.h), a pure function of
  (manifest, now, policy): obs buckets expire when bucket_start +
  bucket_seconds (the segment's own, recorded in its entry) < now -
  retain_seconds; fields keep the newest K runs per model; grids no
  remaining field uses expire.
- A `?snapshot=<seq>` pin works only while the files still exist, i.e.
  within the grace period of anything that snapshot needs.

## Ingest daemon

One daemon is the sole publisher (flock enforces it). Internally threaded:
fetchers -> decoders -> segment/field writers -> publisher.

Raw feed journal (decision D7): every raw message from a feed that does not
keep history is appended to `journal/<source>-<hour>.jrn` and group-committed
(fsync every <= 1 s) BEFORE acknowledgement/decoding. Format: framed records
(length, receipt_time, receipt_seq, CRC, bytes). Hourly files are sealed and
replicated off-box (rsync or similar - out of scope for the library). After a
crash, the ingest daemon replays journal records whose receipt_seq is above the
manifest's `next_receipt_seq - 1`. Grids and model fields are NOT journaled;
refetch from NOAA.

`receipt_seq` is assigned by the ingest daemon, persisted via the manifest's
`next_receipt_seq`, and never reused.

## Query daemon(s)

Stateless apart from the snapshot and in-RAM caches. Several can run on one
box (separate ports, or behind the reverse proxy) sharing the same mmapped
pages. They never write to the store (mount read-only for them if the
deployment allows).

## Acceptance tests

1. Publish/adopt: one thread publishes 100 manifests with new files while 8
   reader threads loop {take snapshot; verify every file in it opens and
   checksums}; no failure, snapshots monotonic.
2. Crash points: kill the publisher (simulated by throwing) after each step of
   the publish protocol; a fresh reader always sees a complete consistent
   snapshot (old or new, never partial).
3. Retirement: file removed from manifest and unlinked while a reader holds the
   old snapshot; reader keeps working; new snapshot works.
4. Second publisher fails to take the lock.
5. Journal replay: write N messages, truncate the last frame mid-record,
   replay recovers exactly the complete frames; CRC flip in the middle is
   detected.
