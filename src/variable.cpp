#include "atmo/variable.h"

#include <algorithm>

#include "atmo/variables.h"

namespace atmo
{

std::span<const VariableInfo> all_variables()
{
  return kVariables;
}

const VariableInfo* find_variable(std::uint16_t code)
{
  const auto it = std::lower_bound(
    std::begin(kVariables), std::end(kVariables), code,
    [](const VariableInfo& v, std::uint16_t c) { return v.code < c; });
  return it != std::end(kVariables) && it->code == code ? &*it : nullptr;
}

const VariableInfo* find_variable(std::string_view name)
{
  for (const auto& v : kVariables)
  {
    if (name == v.name)
    {
      return &v;
    }
  }
  return nullptr;
}

const VariableInfo*
find_grib2_variable(int discipline, int category, int number)
{
  if (discipline < 0 || category < 0 || number < 0)
  {
    return nullptr;  // -1 marks "no GRIB2 source" in the table
  }
  for (const auto& v : kVariables)
  {
    if (
      v.grib2_discipline == discipline && v.grib2_category == category &&
      v.grib2_number == number)
    {
      return &v;
    }
  }
  return nullptr;
}

}  // namespace atmo
