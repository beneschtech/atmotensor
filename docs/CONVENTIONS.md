# Coding conventions

Agreed with Chris on 2026-11-05. Rules here override the style of older
code; existing files are being brought into line separately.
`.clang-format` at the repo root encodes the layout rules; run it on
every file you touch.

## Language and build

- CMake only (CLion is the IDE); `CMakePresets.json` defines the
  `default`, `asan` and `ci` builds. There is no Makefile.
- C++20 (`CMAKE_CXX_STANDARD 20`, `-std=c++20`). Linux, little-endian.
- The project builds a static library (`libatmo`) from `src/*.cpp`.
  Tests, benchmarks and daemons link against it.
- Warnings: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` everywhere.
  `-Werror` only in CI (CMake option `ATMO_WERROR=ON`), not in local
  builds.
- `-ffp-contract=off` stays (bit-identical results against the oracles).

## Headers versus source files

Headers hold declarations; code goes in `.cpp` files.

- `include/atmo/<name>.h` declares; `src/<name>.cpp` implements.
- Header files use the `.h` extension, for C and C++ alike. Vendored
  files keep their upstream names (`nlohmann/json.hpp`).
- Only trivial getters and setters may be defined in a header: a single
  `return member_;` or `member_ = v;`, written inside the class body.
  Returning `member_[k]`, `member_.size()` or `member_.empty()` also counts.
  Everything else, however short, goes in the `.cpp`. Columns of an mmap'd
  file are exposed by computing their pointers once (in the constructor)
  and returning those members.
- Defaulted operators (`bool operator==(const T&) const = default;`) are
  declarations and may stay in the header.
- Allowed in headers: type definitions, `enum`s, constants (`constexpr`
  values), on-disk struct layouts with their `static_assert`s.
- **Exception: templates.** A template's code may live in its header.
  Keep anything that does not depend on the template parameter in the
  `.cpp`.
- **Exception: vendored code** (`third_party/`), which is used as shipped.
- Test helpers follow the same rule. The harness macros in
  `tests/check.h` are macros, so they stay in the header.

## Layout

- Indent with 2 spaces. **No tabs**, anywhere (code, CMake, JSON,
  Markdown).
- Lines should stay within **80 columns**. Going over is allowed when
  breaking would hurt readability, but try not to.
- **Allman braces everywhere**: every opening brace goes on its own line,
  at the indentation of the statement that opens it. That covers
  functions, classes, structs, enums, namespaces and control blocks.

      class Foo
      {
       public:
        int run(int x);
      };

      int Foo::run(int x)
      {
        if (x > 0)
        {
          return x;
        }
        return 0;
      }

- Access specifiers (`public:`) are indented 1 space inside a class.
- `switch`: each `case` is indented one level inside the switch, and its
  code starts on the next line one level further in.

      switch (kind)
      {
        case Kind::metar:
          return parse_metar(text);
        default:
          throw std::invalid_argument("unknown kind");
      }

- **Function declarations and definitions** go on one line. If that
  would pass 80 columns, break after the opening parenthesis and put
  **one parameter per line**, indented one level (2 spaces) from the
  function's own indent. The closing parenthesis stays on the line of
  the last parameter.

      static std::shared_ptr<const ObsSegment> open(
        const std::filesystem::path& path,
        Verify verify);

      SealedSegment ObsSegmentWriter::seal(
        const std::filesystem::path& path,
        bool fsync_dir)
      {
        ...
      }

  Function calls that wrap use the same continuation indent of 2 spaces;
  arguments may share lines.
- Embedded SQL is written one clause per line, starting with the
  reserved word. Keywords are right-aligned so that each clause's text
  lines up after its keyword:

      const char* sql = R"(
      SELECT station_ref, valid_from, lat, lon
        FROM stations
       WHERE icao = ?
         AND valid_from <= ?
       ORDER BY valid_from DESC
      )";

- Pointers and references bind to the type: `const Grid& g`, `char* p`.
- `#pragma once`. Includes are grouped in this order, separated by blank
  lines: the matching header (in a `.cpp`), POSIX/C headers, C++ standard
  headers, vendored headers, project headers.

## Naming

- Types: `PascalCase`. Functions and variables: `snake_case`. Data
  members: `snake_case_` with a trailing underscore. Constants: `kName`.
  Enumerators: `snake_case` inside `enum class`.
- Everything is in namespace `atmo`. Internals go in `atmo::detail` or a
  component sub-namespace (`atmo::obsseg`).
- **Units in names.** Every dimensional quantity carries its unit as a
  suffix: `_m`, `_km`, `_s`, `_ms`, `_K`, `_Pa`, `_mps` (`_ms` always
  means milliseconds), `_deg`, `_rad`. Values inside the engine are SI
  (invariant 4); the suffix still prevents mix-ups at boundaries. Config
  keys from the specs (`tau_seconds`, `bucket_seconds`, ...) keep the
  spelling the spec gives until the spec renames them.
- Vertical position is always a geometric height above MSL in metres,
  never a level index or a geopotential height (Q3).

## Errors

- Exceptions for setup, configuration, I/O and corruption:
  - `std::invalid_argument`: bad caller input or config.
  - `std::runtime_error`: file and system errors, corrupt or truncated
    files.
  - `std::logic_error`: misuse of an object (for example, sealing twice).
  - `std::out_of_range`: an index or name that is not there.
- Query hot paths do not throw and are marked `noexcept`. The daemon
  catches at the request boundary and maps errors to HTTP status codes.
- Messages name the component and the object: `"ObsSegment: <path>: header
  checksum mismatch"`.

## Ownership and data

- No owning raw pointers. Immutable, mmap-backed objects are passed as
  `std::shared_ptr<const T>`. Non-owning views (`std::span`, `*View`
  classes) point into a mapping and must not outlive its owner.
- On-disk and GPU-facing data uses fixed-width types (`std::uint32_t`,
  ...). Every on-disk struct has a `static_assert` on its size. In-memory
  indices use `std::size_t`.
- **Use `f32` wherever the precision allows**, so data can go to OpenCL
  kernels as-is (positions, heights, values, offsets, tree points). f32
  lat/lon resolve about 1.7 m. Wider types need a reason, written as a
  comment where they are declared. The current exceptions:
  - times: `i64` seconds or milliseconds (f32 cannot hold epoch times;
    OpenCL 1.2 has `long`);
  - ids, keys and sequence numbers: `u32`/`u64`;
  - grid lat/lon (`GridData`, grid files): `f64`, because `Grid::locate()`
    solves within a cell and f32 would cost ~5e-4 of a 3 km cell; they
    are used on the CPU only, and kernels sample by grid index;
  - configuration parameters (`SourceClass`): `double`, they are settings,
    not data;
  - intermediate arithmetic (e.g. inside `embed()`) may use double and
    narrow the result.
- On-disk formats: magic number plus version, header CRC, a CRC for
  every section, 4 KiB-aligned sections. Readers recompute every offset
  and run structural checks even when the CRCs pass. Sealed files are
  immutable and are published with `link()`, so an existing file is
  never replaced.

## Comments

- Comments explain *why*, and state invariants. Don't narrate the code.
- Each header starts with a comment describing the component. For an
  on-disk format, that comment is the definitive description of the
  format; keep it in sync with the code.

## Tests

- One test file per component (`tests/test_<name>.cpp`), using only
  `tests/check.h`. No outside test frameworks.
- Write tests from the spec's acceptance list first. Use fixed random
  seeds, and compare against a brute-force oracle where one exists.
- Mutation check: deliberately break the core logic and confirm that a
  test fails.
- Every new test is added to the `foreach` list in `CMakeLists.txt`.
  Run the `asan` preset (`ctest --preset asan`) after touching pointers
  or file I/O.
