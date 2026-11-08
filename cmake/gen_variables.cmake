# Generates include/atmo/variables.h from docs/design/variables.csv (Q4,
# T8b).  Plain CMake so the build needs nothing else.
#
#   cmake -DIN=<csv> -DOUT=<header> -P gen_variables.cmake    write OUT
#   cmake -DIN=<csv> -DCHECK=<header> -P gen_variables.cmake  fail if stale
#
# Validates the registry: codes 1..65535 and unique, names
# [a-z][a-z0-9_]* and unique, units present, the GRIB2 columns all set or
# all empty, no ';' anywhere (CMake list separator), no '"' or '\\'.

if(NOT DEFINED IN)
  message(FATAL_ERROR "gen_variables: IN not set")
endif()

file(READ "${IN}" content)
if(content MATCHES ";")
  # CMake would split lines there (list separator).
  message(FATAL_ERROR "${IN}: ';' is not allowed anywhere in the file")
endif()
string(REPLACE "\r" "" content "${content}")
string(REPLACE "\n" ";" lines "${content}")
set(seen_header FALSE)
set(codes "")
set(names "")
set(constants "")
set(rows "")
set(lineno 0)

foreach(line IN LISTS lines)
  math(EXPR lineno "${lineno} + 1")
  string(STRIP "${line}" line)
  if(line STREQUAL "" OR line MATCHES "^#")
    continue()
  endif()
  if(NOT seen_header)
    set(want "code,name,units,grib2_discipline,grib2_category,grib2_number,grib2_scale,grib2_offset,description")
    if(NOT line STREQUAL want)
      message(FATAL_ERROR "${IN}:${lineno}: header must be: ${want}")
    endif()
    set(seen_header TRUE)
    continue()
  endif()
  if(line MATCHES "\"" OR line MATCHES "\\\\")
    message(FATAL_ERROR "${IN}:${lineno}: '\"' and '\\' are not allowed")
  endif()
  string(REPLACE "," ";" f "${line}")
  list(LENGTH f n)
  if(NOT n EQUAL 9)
    message(FATAL_ERROR "${IN}:${lineno}: need 9 columns, got ${n}")
  endif()
  list(GET f 0 code)
  list(GET f 1 name)
  list(GET f 2 units)
  list(GET f 3 gd)
  list(GET f 4 gc)
  list(GET f 5 gn)
  list(GET f 6 gs)
  list(GET f 7 go)
  list(GET f 8 desc)

  if(NOT code MATCHES "^[1-9][0-9]*$" OR code GREATER 65535)
    message(FATAL_ERROR "${IN}:${lineno}: code must be 1..65535")
  endif()
  if(code IN_LIST codes)
    message(FATAL_ERROR "${IN}:${lineno}: code ${code} used twice")
  endif()
  if(DEFINED last_code AND NOT code GREATER last_code)
    message(FATAL_ERROR "${IN}:${lineno}: codes must be in ascending order")
  endif()
  set(last_code ${code})
  list(APPEND codes ${code})
  if(NOT name MATCHES "^[a-z][a-z0-9_]*$")
    message(FATAL_ERROR "${IN}:${lineno}: bad name '${name}'")
  endif()
  if(name IN_LIST names)
    message(FATAL_ERROR "${IN}:${lineno}: name ${name} used twice")
  endif()
  list(APPEND names ${name})
  if(units STREQUAL "")
    message(FATAL_ERROR "${IN}:${lineno}: units missing")
  endif()

  if(gd STREQUAL "" AND gc STREQUAL "" AND gn STREQUAL "" AND gs STREQUAL ""
     AND go STREQUAL "")
    set(gd -1)
    set(gc -1)
    set(gn -1)
    set(gs 1)
    set(go 0)
  else()
    foreach(v gd gc gn)
      if(NOT ${v} MATCHES "^[0-9]+$" OR ${v} GREATER 191)
        message(FATAL_ERROR
          "${IN}:${lineno}: GRIB2 discipline/category/number must be 0..191")
      endif()
    endforeach()
    foreach(v gs go)
      if(NOT ${v} MATCHES "^-?[0-9]+(\\.[0-9]+)?([eE]-?[0-9]+)?$")
        message(FATAL_ERROR "${IN}:${lineno}: bad GRIB2 scale/offset")
      endif()
    endforeach()
  endif()

  # air_temperature -> kAirTemperature
  string(REPLACE "_" ";" parts "${name}")
  set(camel "k")
  foreach(p IN LISTS parts)
    string(SUBSTRING "${p}" 0 1 first)
    string(SUBSTRING "${p}" 1 -1 rest)
    string(TOUPPER "${first}" first)
    string(APPEND camel "${first}${rest}")
  endforeach()

  string(APPEND constants "constexpr std::uint16_t ${camel} = ${code};\n")
  string(APPEND rows
    "  {${code}, \"${name}\", \"${units}\", ${gd}, ${gc}, ${gn}, ${gs}, ${go}, \"${desc}\"},\n")
endforeach()

if(NOT seen_header)
  message(FATAL_ERROR "${IN}: no header line")
endif()

set(text "// GENERATED from docs/design/variables.csv by cmake/gen_variables.cmake.
// Do not edit: change the CSV and build the gen_variables target.  Codes are
// written into data files and are never reused (see the CSV).
#pragma once

#include <cstdint>

#include \"atmo/variable.h\"

namespace atmo
{

namespace var
{

// clang-format off
${constants}// clang-format on

}  // namespace var

// Sorted by code.
// clang-format off
inline constexpr VariableInfo kVariables[] = {
${rows}};
// clang-format on

}  // namespace atmo
")

if(DEFINED CHECK)
  file(READ "${CHECK}" current)
  if(NOT current STREQUAL text)
    message(FATAL_ERROR
      "${CHECK} is stale: build the gen_variables target to regenerate it")
  endif()
elseif(DEFINED OUT)
  file(WRITE "${OUT}" "${text}")
else()
  message(FATAL_ERROR "gen_variables: set OUT or CHECK")
endif()
