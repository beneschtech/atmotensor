// Variable registry (Q4, T8b): the generated table and its lookups.
#include <cmath>
#include <cstring>
#include <set>
#include <string>

#include "atmo/variable.h"
#include "atmo/variables.h"
#include "check.h"

using namespace atmo;

TEST(variables_table_is_consistent)
{
  const auto all = all_variables();
  CHECK(all.size() >= 25);
  std::set<std::string> names;
  std::set<std::tuple<int, int, int>> grib;
  std::uint16_t prev = 0;
  for (const auto& v : all)
  {
    CHECK(v.code > prev);  // sorted, unique, no 0
    prev = v.code;
    CHECK(names.insert(v.name).second);
    CHECK(std::strlen(v.units) > 0);
    CHECK(std::isfinite(v.grib2_scale) && v.grib2_scale != 0.0);
    if (v.grib2_discipline >= 0)
    {
      CHECK(v.grib2_category >= 0 && v.grib2_number >= 0);
      // One GRIB2 parameter maps to one variable.
      CHECK(grib.insert({v.grib2_discipline, v.grib2_category, v.grib2_number})
              .second);
    }
    else
    {
      CHECK(v.grib2_category == -1 && v.grib2_number == -1);
    }
  }
}

TEST(variables_lookups)
{
  // Codes are part of the file formats: these must never change.
  CHECK(var::kAirTemperature == 1);
  CHECK(var::kWindU == 5 && var::kWindV == 6);
  CHECK(var::kGeopotentialHeight == 13);

  const VariableInfo* t = find_variable(var::kAirTemperature);
  CHECK(t && std::string(t->name) == "air_temperature");
  CHECK(t && std::string(t->units) == "K");
  CHECK(find_variable("wind_v") == find_variable(var::kWindV));
  CHECK(find_variable(std::uint16_t{0}) == nullptr);
  CHECK(find_variable(std::uint16_t{65535}) == nullptr);
  CHECK(find_variable("no_such_variable") == nullptr);

  CHECK(find_grib2_variable(0, 0, 0) == t);
  CHECK(find_grib2_variable(0, 2, 2) == find_variable(var::kWindU));
  const VariableInfo* rh = find_grib2_variable(0, 1, 1);
  CHECK(rh && rh->code == var::kRelativeHumidity && rh->grib2_scale == 0.01);
  CHECK(find_grib2_variable(0, 16, 196) == nullptr);  // local, unmapped
  CHECK(find_grib2_variable(-1, -1, -1) == nullptr);  // "no GRIB2" rows
  const VariableInfo* alt = find_variable(var::kAltimeterSetting);
  CHECK(alt && alt->grib2_discipline == -1);
}

int main()
{
  return tst::run_all();
}
