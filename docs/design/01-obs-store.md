# Observation store

Covers surface obs (METAR/SYNOP/buoy/ship), soundings, aircraft reports,
PIREPs. NOT covered: TAF, SIGMET and other products with a validity interval or
spatial extent - those need an interval/R-tree structure (separate spec later).

## Source classes

A *source class* fixes one k-d tree metric. Config (JSON):

    { "name": "metar", "bucket_seconds": 3600, "L_km": 100, "tau_seconds": 3600,
      "vertical_scale": 10, "retain_seconds": 259200 }

- lambda (km per second) = L_km / tau_seconds. Meaning: tau seconds of time
  difference counts as much as L_km of horizontal distance.
- Embedding is `atmo::embed()` with t0 = bucket start.
- Suggested starting values (tune later): metar L=100 km, tau=1 h; synop L=150,
  tau=3 h; sounding L=250, tau=6 h, bucket 12 h; pirep L=100, tau=1 h;
  aircraft (AMDAR/ACARS) L=100, tau=30 min.
- Never put two source classes with different lambda in one tree.

## Data model

Two tables per segment, because one report yields ~a dozen values but has ONE
position. The k-d tree indexes reports; values hang off reports.

reports (one row per decoded report), struct-of-arrays:
- `report_key u64`   hash(source class, station_ref or platform id, obs_time_sec, report_type). Identifies "the same report" across corrections.
- `receipt_seq u64`  global monotonically increasing ingest sequence (persisted in the manifest). Orders versions: latest = max receipt_seq per report_key.
- `receipt_time i64` unix milliseconds. For `as_of` by wall clock.
- `obs_time i64`     unix seconds.
- `lat f64, lon f64, height_m f32`  position AT observation time (mobile platforms differ per report).
- `station_ref u32`  index into the station table (0 = none/mobile).
- `flags u16`        bit0 = correction/amendment, bit1 = decoder warnings; rest reserved.
- `first_value u32, n_values u16`  slice of the values table.
- `raw_off u64, raw_len u32`      slice of the segment's raw section.

values (struct-of-arrays, contiguous per report):
- `var_code u16`  canonical registry (docs to be added in `variables.md`; e.g. air_temperature_K, dewpoint_K, wind_u_ms ...)
- `value f32`     SI units
- `qc u16`        0 = unchecked/ok; nonzero reserved for QC flags

raw: the original message bytes, stored ONCE per report; a dozen values point
at it via the report row. Sounding profiles: one report whose values are a
level-ordered profile: every value carries the geometric height of its
level (`height_m`, metres above MSL), see Q3.

## Segment file (`*.seg`)

Immutable, written by ingest: temp file -> fsync -> rename -> fsync dir ->
then listed in a manifest. Same conventions as `gridstore.hpp` (4 KiB header
page with magic `ATOBSG01`, version, source class, bucket start, counts,
section offsets, per-section CRC-32, header CRC; 4 KiB-aligned sections).

Sections: reports columns | values columns | raw blob | tree (prebuilt
`KdTree4<float>` arrays: points, ids, axes) so loading is mmap + CRC, no rebuild.
Tree ids = row index into reports.

## Buckets, segments, compaction

- Bucket = floor(obs_time / bucket_seconds) per source class. A bucket holds
  one or more sealed segments: new arrivals create small segments (seal every
  <= 60 s or N reports).
- Corrections for a report share its obs_time, so they land in the same bucket
  as the original (possibly in a different segment).
- Compaction merges a bucket's segments into one, keeping ALL versions
  (history is part of the contract inside the retention window), and rebuilds
  the tree. Publish new manifest; old segments retire (see 02).

## Visible-version queries

`as_of` is a receipt_seq (default: snapshot's max). Visible record = for each
report_key, the row with max receipt_seq <= as_of.
- Search all segments of the buckets overlapping [t - W, t + W]
  (W = 3 * tau default), each with its own tree.
- Merge candidate lists by (d2, report_key, receipt_seq); drop superseded rows.
- Because dropped rows can leave fewer than k, run knn with k' = 2k and double
  k' until k visible results or the tree is exhausted.
- Results are ordered by (d2, report_key) - deterministic.

API sketch (library): `ObsIndex::knn(source, point, k, as_of)`,
`radius(...)`, `corridor(source, polyline_with_times, half_width_km, as_of)`;
return `ReportView` (position, times, values span, raw span, flags).

## Station table

Implemented in T3 (`include/atmo/stations.hpp`); decisions D10 and Q6.

- Authoritative list in MySQL (D10); ingest exports a full copy as an
  immutable generation `stations/stations-<gen:016x>.tbl` referenced by the
  manifest, so each snapshot pins one generation.
- `station_ref` (u32, > 0) identifies the station, not one of its periods,
  and is never reused: a generation contains every ref of the previous one
  and new refs are above its `max_ref` (MySQL AUTO_INCREMENT values or
  assigned by the builder). 0 = no station.
- Identifiers are typed (icao, wmo, wigos, platform) and bound to a station
  for a validity interval; for one identifier the intervals never overlap
  across the table, so (identifier, time) names at most one station.
- Fixed stations carry position periods [from, to) for moves and
  re-surveys. Mobile platforms (flag `mobile`) and provisional stations
  (flag `provisional`: seen in a report, not yet in the authoritative list;
  ingest creates them rather than dropping data) may have no periods;
  positions then come from each report.

## Retention

Per source class `retain_seconds` (default 72 h). A bucket expires when
bucket_end < now - retain; expiry = remove from manifest, unlink after grace.
All versions inside a retained bucket stay.

## Acceptance tests (write these first)

1. Round trip: write segment, reopen, every column bit-equal; CRC flip in each
   section -> load throws; truncated file -> throws.
2. knn/radius over a segment == brute force over the same rows (use
   `KdTree4::dist2` for the oracle), including duplicate positions.
3. Corrections: report A v1 in segment 1, v2 in segment 2: default query sees
   only v2; `as_of` = seq(v1) sees only v1; both never appear together.
4. Visible-k: dataset where most nearest rows are superseded; knn(k) still
   returns k visible reports when they exist.
5. Mobile platform: same station, positions differing per report are indexed
   at their own positions.
6. Raw sharing: report with 12 values stores raw bytes once; ReportView raw
   span matches the original message.
