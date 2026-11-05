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
  src/crc32.cpp
  src/geo.cpp
  src/grid.cpp
  src/gridstore.cpp
  src/obs_segment.cpp
  src/source.cpp)
add_library(atmo::atmo ALIAS atmo)
target_include_directories(atmo PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# Vendored single-header deps (see third_party/README.md); SYSTEM so their
# warnings do not drown ours.
target_include_directories(atmo SYSTEM PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party>)
target_compile_features(atmo PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(atmo PUBLIC -ffp-contract=off)
endif()
atmo_target_defaults(atmo)

# Shared test/bench scaffolding: harness and synthetic Lambert grid.
if(ATMO_BUILD_TESTS OR ATMO_BUILD_BENCH)
  add_library(atmo_testkit STATIC tests/check.cpp tests/lambert.cpp)
  target_include_directories(atmo_testkit PUBLIC tests)
  target_link_libraries(atmo_testkit PUBLIC atmo::atmo)
  atmo_target_defaults(atmo_testkit)
endif()

if(ATMO_BUILD_TESTS)
  enable_testing()
  foreach(t kdtree grid source obs)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE atmo_testkit)
    atmo_target_defaults(test_${t})
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
endif()

if(ATMO_BUILD_BENCH)
  foreach(b kdtree grid)
    add_executable(bench_${b} bench/bench_${b}.cpp)
    target_link_libraries(bench_${b} PRIVATE atmo_testkit)
    atmo_target_defaults(bench_${b})
  endforeach()
endif()
