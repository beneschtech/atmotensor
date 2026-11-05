# atmotensor

KD Trees and spatial arithmetic for gridded weather data and observations.

## Layout

Declarations live in `include/atmo/*.hpp`, code in `src/*.cpp` (built as the
static library `atmo::atmo`); see `docs/CONVENTIONS.md`.

- `include/atmo/kdtree.hpp` - `atmo::KdTree4<T>`: static, flat-array 4D k-d tree
  (nearest / kNN / radius; deterministic `(distance, id)` ordering).
- `include/atmo/geo.hpp` - `atmo::embed()`: (lat, lon, height, time) -> 4D point.
  Position is a 3D vector at radius R + scaled height (no dateline/pole
  problems); time is `km_per_second * (t - t0)`, so each source class gets its
  own time weight by building its own tree.
- `include/atmo/grid.hpp` - grid definitions: `GridIdentity` (opaque canonical
  bytes + ni, nj, periodic flag -> 64-bit `GridKey`), shared lat/lon/rotation
  arrays, `Grid::locate()` (lat/lon -> cell + fractions) and `Grid::nearest()`.
- `include/atmo/gridstore.hpp` - `GridStore`: one immutable, checksummed,
  4 KB-aligned file per grid (`<key>.grid`), written atomically, mmap'd on read.
- `tests/` - dependency-free test harness, brute-force oracle, synthetic
  Lambert grid. `bench/` - timings at 2M points.

## Build

CMake (open the folder in CLion; it picks up `CMakePresets.json`):

    cmake --preset default && cmake --build --preset default -j
    ctest --preset default
    cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
    # options: ATMO_SANITIZE  ATMO_WERROR  ATMO_BUILD_BENCH  ATMO_BUILD_TESTS
