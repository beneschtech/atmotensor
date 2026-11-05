# Gridded field files

One file per (model, run, forecast hour): `fields/<model>/<run>/f<fhr:03>.fld`,
mirroring how NOAA delivers GRIB (one file per forecast hour), holding only the
fields that are configured for that model. Written once by ingest after
decoding via eccodes. Retention: newest K runs per model (default K=4); expiry
drops whole run directories.

## Why tiles, why this size

Point and route queries should touch one or a few tiles; area products stream
tiles. Tile = 256 x 256 grid points + a 1-cell halo on every side (so bilinear
interpolation inside a tile never needs a neighbour tile). float32, so
258 * 258 * 4 = 266,256 B, rounded up to a 4 KiB multiple (270,336 B) per tile
slot. Halo width and tile size are in the file header, so they can change per
file later (bicubic would want halo 2). Edge tiles are padded; cells outside
the grid are filled with NaN. For periodic-in-i grids (global lat/lon) the halo
wraps across the seam.

HRRR-size grid (1799 x 1059): 8 x 5 = 40 tiles per field, ~10.8 MB/field.
GFS 0.25 (1440 x 721): 6 x 3 = 18 tiles per field. A model run's disk use is
(configured fields) x (forecast hours) x that, so field selection is a config
decision, not "everything in the GRIB".

## File layout

Same conventions as `gridstore.hpp`: 4 KiB header page (magic `ATFLD001`,
version, model, run time, forecast hour, `grid_key`, tile size, halo, field
count, section offsets, CRCs), then:
- field directory: per field `param_id u16` (internal registry, SI units),
  `level_type u8`, `level_value f32`, `packing u8` (0 = float32; 1 = float16
  reserved), `min f32`, `max f32`, `tile_table_off u64`
- tile tables: per field, `tiles_x * tiles_y` entries (offset u64, crc u32)
- tile slots, 4 KiB aligned, written in field then row-major tile order

Units: converted to SI at ingest (K, m/s, Pa, kg/m2, ...). Winds are stored as
the grid-relative components as published; the file records the GRIB component
flag so the query layer knows whether to `rotate_to_earth` (see CLAUDE.md).

## Reading

`FieldFile::open(path)` -> mmap, verify header/directory CRC; tile CRCs are
verified on first touch (cache a bitmap in memory) so opening a 1 GB file is
cheap but corruption is still caught before use. API:
`sample(field, Grid::Cell)` -> bilinear value; `tile(field, tx, ty)` -> span.

Time interpolation uses two files (fhr and fhr+1, or across runs); vertical
interpolation between pressure levels is log-p, done in the query layer.

## Acceptance tests

1. Synthetic field f(lat, lon) = smooth analytic function on the Lambert test
   grid and on the global periodic grid: write file, reopen, `sample()` at 1000
   random points matches the analytic value within the grid's known
   interpolation error; points across tile boundaries and across the
   periodic seam are continuous.
2. Bit-exact round trip of every tile; CRC flip in a tile is caught on first
   touch of that tile only.
3. Edge tiles: values outside the grid are NaN; `locate()` outside the grid
   returns nullopt before any tile is read.
4. Truncated file, wrong magic, wrong grid_key vs the GridStore -> clear error.
