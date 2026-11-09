cmake_minimum_required(VERSION 3.25)
project(atmotensor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators with no build type given: optimise, keep symbols.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ATMO_BUILD_TESTS "Build unit tests" ON)
option(ATMO_BUILD_BENCH "Build benchmarks" ON)
option(ATMO_SANITIZE "Build with ASan + UBSan" OFF)
option(ATMO_WERROR "Treat warnings as errors (CI)" OFF)

function(atmo_target_defaults tgt)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${tgt} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    if(ATMO_WERROR)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
  endif()
  if(ATMO_SANITIZE)
    target_compile_options(${tgt} PRIVATE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# The library: declarations in include/atmo, code in src (docs/CONVENTIONS.md).
# -ffp-contract=off keeps floating point results bit-identical between the
# tree and the brute-force oracles in the tests (and between
# compilers/targets that have FMA); it is PUBLIC because templates such as
# KdTree4 and embed() are compiled in the users' translation units.
add_library(atmo STATIC
  src/colormap.cpp
  src/crc32.cpp
  src/fcgi.cpp
  src/fieldfile.cpp
  src/geo.cpp
  src/grid.cpp
  src/gribstore.cpp
  src/gridstore.cpp
  src/http_api.cpp
  src/jsonutil.cpp
  src/manifest.cpp
  src/fileio.cpp
  src/obs_index.cpp
  src/obs_segment.cpp
  src/png.cpp
  src/query.cpp
  src/refdata.cpp
  src/retention.cpp
  src/snapshot.cpp
  src/source.cpp
  src/stations.cpp
  src/tiles.cpp
  src/variable.cpp)
add_library(atmo::atmo ALIAS atmo)
target_include_directories(atmo PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Vendored single-header deps (see third_party/README.md); SYSTEM so their
# warnings do not drown ours.
target_include_directories(atmo SYSTEM PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
target_compile_features(atmo PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(atmo PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(atmo PUBLIC -ffp-contract=off)
endif()
atmo_target_defaults(atmo)

# PNG tiles (T10b): zlib compresses them when available; without it PNGs
# are written with stored (uncompressed) deflate blocks.
option(ATMO_WITH_ZLIB "Compress PNG tiles with zlib if found" ON)
if(ATMO_WITH_ZLIB)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    target_link_libraries(atmo PUBLIC ZLIB::ZLIB)
    target_compile_definitions(atmo PRIVATE ATMO_HAVE_ZLIB=1)
    message(STATUS "AtmoTensor: zlib ${ZLIB_VERSION_STRING} for PNG tiles")
  else()
    message(STATUS "AtmoTensor: zlib not found, PNG tiles uncompressed")
  endif()
endif()

# GRIB2 ingest (T11, D9): eccodes from the distro package, or a hand-built
# copy via -DECCODES_ROOT=/prefix (or CMAKE_PREFIX_PATH).  Detection order:
# CMake config package, pkg-config, plain find_path/find_library.  With
# OFF, or AUTO and nothing found, everything else builds without it.
set(ATMO_WITH_ECCODES AUTO CACHE STRING "GRIB2 ingest with eccodes: AUTO|ON|OFF")
set_property(CACHE ATMO_WITH_ECCODES PROPERTY STRINGS AUTO ON OFF)
set(ECCODES_ROOT "" CACHE PATH "Install prefix of a hand-built eccodes")
set(ATMO_ECCODES_TARGET "")
if(NOT ATMO_WITH_ECCODES STREQUAL "OFF")
  if(ECCODES_ROOT)
    list(PREPEND CMAKE_PREFIX_PATH "${ECCODES_ROOT}")
    set(ENV{PKG_CONFIG_PATH}
      "${ECCODES_ROOT}/lib/pkgconfig:${ECCODES_ROOT}/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  endif()
  # Config package first, but only if it loads cleanly in a sub-configure
  # (see cmake/eccodes_probe): a broken one is a fatal error even QUIET.
  if(NOT DEFINED ATMO_ECCODES_CONFIG_OK)
    string(REPLACE ";" "\\;" atmo_prefix_path "${CMAKE_PREFIX_PATH}")
    execute_process(
      COMMAND ${CMAKE_COMMAND}
        -S ${CMAKE_CURRENT_SOURCE_DIR}/cmake/eccodes_probe
        -B ${CMAKE_CURRENT_BINARY_DIR}/eccodes_probe
        "-DCMAKE_PREFIX_PATH=${atmo_prefix_path}"
      RESULT_VARIABLE atmo_probe_rc OUTPUT_QUIET ERROR_QUIET)
    if(atmo_probe_rc EQUAL 0)
      set(ATMO_ECCODES_CONFIG_OK TRUE CACHE INTERNAL "")
    else()
      set(ATMO_ECCODES_CONFIG_OK FALSE CACHE INTERNAL "")
      message(STATUS
        "AtmoTensor: eccodes CMake config package missing or broken")
    endif()
  endif()
  if(ATMO_ECCODES_CONFIG_OK)
    find_package(eccodes CONFIG QUIET)
  endif()
  if(eccodes_FOUND AND TARGET eccodes)
    set(ATMO_ECCODES_TARGET eccodes)
    set(atmo_eccodes_how "CMake config (${eccodes_DIR})")
  else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(ATMO_ECCODES_PC QUIET IMPORTED_TARGET eccodes)
    endif()
    if(ATMO_ECCODES_PC_FOUND)
      set(ATMO_ECCODES_TARGET PkgConfig::ATMO_ECCODES_PC)
      set(atmo_eccodes_how "pkg-config")
    else()
      find_path(ECCODES_INCLUDE_DIR eccodes.h
        HINTS ${ECCODES_ROOT} PATH_SUFFIXES include)
      find_library(ECCODES_LIBRARY eccodes
        HINTS ${ECCODES_ROOT} PATH_SUFFIXES lib lib64)
      if(ECCODES_INCLUDE_DIR AND ECCODES_LIBRARY)
        add_library(atmo_eccodes INTERFACE IMPORTED)
        target_include_directories(atmo_eccodes INTERFACE ${ECCODES_INCLUDE_DIR})
        target_link_libraries(atmo_eccodes INTERFACE ${ECCODES_LIBRARY})
        set(ATMO_ECCODES_TARGET atmo_eccodes)
        set(atmo_eccodes_how "find_library (${ECCODES_LIBRARY})")
      endif()
    endif()
  endif()
  if(ATMO_ECCODES_TARGET)
    message(STATUS "AtmoTensor: eccodes found via ${atmo_eccodes_how}")
  elseif(ATMO_WITH_ECCODES STREQUAL "ON")
    message(FATAL_ERROR
      "ATMO_WITH_ECCODES=ON but eccodes was not found (set ECCODES_ROOT)")
  else()
    message(STATUS "AtmoTensor: eccodes not found, GRIB2 ingest disabled")
  endif()
endif()
if(ATMO_ECCODES_TARGET)
  target_sources(atmo PRIVATE src/grib.cpp)
  target_link_libraries(atmo PUBLIC ${ATMO_ECCODES_TARGET})
  target_compile_definitions(atmo PUBLIC ATMO_HAVE_ECCODES=1)
  # Development tool until the ingest daemon exists.
  add_executable(atmo_grib_ingest tools/atmo_grib_ingest.cpp)
  target_link_libraries(atmo_grib_ingest PRIVATE atmo::atmo)
  atmo_target_defaults(atmo_grib_ingest)
endif()

# Shared test/bench scaffolding: harness and synthetic Lambert grid.
if(ATMO_BUILD_TESTS OR ATMO_BUILD_BENCH)
  add_library(atmo_testkit STATIC tests/check.cpp tests/lambert.cpp)
  target_include_directories(atmo_testkit PUBLIC tests)
  target_link_libraries(atmo_testkit PUBLIC atmo::atmo)
  atmo_target_defaults(atmo_testkit)
endif()

# Query daemon (T10).
add_executable(atmo_queryd tools/atmo_queryd.cpp)
target_link_libraries(atmo_queryd PRIVATE atmo::atmo)
atmo_target_defaults(atmo_queryd)

if(ATMO_BUILD_TESTS)
  enable_testing()
  foreach(t kdtree grid source obs obs_index stations manifest refdata retention fieldfile variables query fcgi http_api gribstore colormap tiles)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE atmo_testkit)
    atmo_target_defaults(test_${t})
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
  if(ATMO_ECCODES_TARGET)
    add_executable(test_grib tests/test_grib.cpp)
    target_link_libraries(test_grib PRIVATE atmo_testkit)
    atmo_target_defaults(test_grib)
    add_test(NAME grib COMMAND test_grib)
  endif()
endif()

# Variable registry (Q4): docs/design/variables.csv -> include/atmo/variables.h
# (checked in).  `gen_variables` regenerates it; the variables_header test
# fails while it is stale.
add_custom_target(gen_variables
  COMMAND ${CMAKE_COMMAND}
    -DIN=${CMAKE_CURRENT_SOURCE_DIR}/docs/design/variables.csv
    -DOUT=${CMAKE_CURRENT_SOURCE_DIR}/include/atmo/variables.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_variables.cmake
  COMMENT "Generating include/atmo/variables.h")
if(ATMO_BUILD_TESTS)
  add_test(NAME variables_header
    COMMAND ${CMAKE_COMMAND}
      -DIN=${CMAKE_CURRENT_SOURCE_DIR}/docs/design/variables.csv
      -DCHECK=${CMAKE_CURRENT_SOURCE_DIR}/include/atmo/variables.h
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_variables.cmake)
endif()

if(ATMO_BUILD_BENCH)
  foreach(b kdtree grid)
    add_executable(bench_${b} bench/bench_${b}.cpp)
    target_link_libraries(bench_${b} PRIVATE atmo_testkit)
    atmo_target_defaults(bench_${b})
  endforeach()
endif()
