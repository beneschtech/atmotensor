// Variable registry (Q4, T8b): one code per physical quantity, shared by
// obs segments (var_code) and field files (param_id).  The table lives in
// the generated include/atmo/variables.h (from docs/design/variables.csv);
// this header declares the entry type and the lookups.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace atmo
{

struct VariableInfo
{
  std::uint16_t code;  // never reused
  const char* name;    // e.g. "air_temperature"
  const char* units;   // of the stored values (SI; "1" = dimensionless)
  // WMO GRIB2 discipline / category / number, or -1 if none.
  std::int16_t grib2_discipline;
  std::int16_t grib2_category;
  std::int16_t grib2_number;
  // stored = grib2 value * grib2_scale + grib2_offset
  double grib2_scale;
  double grib2_offset;
  const char* description;
};

// Every registered variable, sorted by code.
std::span<const VariableInfo> all_variables();

// nullptr if unknown.
const VariableInfo* find_variable(std::uint16_t code);
const VariableInfo* find_variable(std::string_view name);
const VariableInfo*
find_grib2_variable(int discipline, int category, int number);

}  // namespace atmo
