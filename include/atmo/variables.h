// GENERATED from docs/design/variables.csv by cmake/gen_variables.cmake.
// Do not edit: change the CSV and build the gen_variables target.  Codes are
// written into data files and are never reused (see the CSV).
#pragma once

#include <cstdint>

#include "atmo/variable.h"

namespace atmo
{

namespace var
{

// clang-format off
constexpr std::uint16_t kAirTemperature = 1;
constexpr std::uint16_t kDewpointTemperature = 2;
constexpr std::uint16_t kRelativeHumidity = 3;
constexpr std::uint16_t kSpecificHumidity = 4;
constexpr std::uint16_t kWindU = 5;
constexpr std::uint16_t kWindV = 6;
constexpr std::uint16_t kWindSpeed = 7;
constexpr std::uint16_t kWindDirection = 8;
constexpr std::uint16_t kWindGust = 9;
constexpr std::uint16_t kVerticalVelocityPressure = 10;
constexpr std::uint16_t kVerticalVelocity = 11;
constexpr std::uint16_t kAbsoluteVorticity = 12;
constexpr std::uint16_t kGeopotentialHeight = 13;
constexpr std::uint16_t kPressure = 14;
constexpr std::uint16_t kPressureMsl = 15;
constexpr std::uint16_t kPrecipitationAmount = 16;
constexpr std::uint16_t kPrecipitationRate = 17;
constexpr std::uint16_t kPrecipitableWater = 18;
constexpr std::uint16_t kSnowDepth = 19;
constexpr std::uint16_t kSnowWaterEquivalent = 20;
constexpr std::uint16_t kCloudCoverTotal = 21;
constexpr std::uint16_t kCloudCoverLow = 22;
constexpr std::uint16_t kCloudCoverMedium = 23;
constexpr std::uint16_t kCloudCoverHigh = 24;
constexpr std::uint16_t kCloudCeiling = 25;
constexpr std::uint16_t kVisibility = 26;
constexpr std::uint16_t kCape = 27;
constexpr std::uint16_t kCin = 28;
constexpr std::uint16_t kLiftedIndex = 29;
constexpr std::uint16_t kTemperatureMax = 30;
constexpr std::uint16_t kTemperatureMin = 31;
constexpr std::uint16_t kShortwaveDown = 32;
constexpr std::uint16_t kLongwaveDown = 33;
constexpr std::uint16_t kAltimeterSetting = 34;
// clang-format on

}  // namespace var

// Sorted by code.
// clang-format off
inline constexpr VariableInfo kVariables[] = {
  {1, "air_temperature", "K", 0, 0, 0, 1, 0, "Air temperature"},
  {2, "dewpoint_temperature", "K", 0, 0, 6, 1, 0, "Dew point temperature"},
  {3, "relative_humidity", "1", 0, 1, 1, 0.01, 0, "Relative humidity as a fraction"},
  {4, "specific_humidity", "kg/kg", 0, 1, 0, 1, 0, "Specific humidity"},
  {5, "wind_u", "m/s", 0, 2, 2, 1, 0, "Wind u component (grid- or earth-relative: see the field flags)"},
  {6, "wind_v", "m/s", 0, 2, 3, 1, 0, "Wind v component (grid- or earth-relative: see the field flags)"},
  {7, "wind_speed", "m/s", 0, 2, 1, 1, 0, "Wind speed"},
  {8, "wind_direction", "deg", 0, 2, 0, 1, 0, "Wind direction (from) in degrees true"},
  {9, "wind_gust", "m/s", 0, 2, 22, 1, 0, "Wind speed of gusts"},
  {10, "vertical_velocity_pressure", "Pa/s", 0, 2, 8, 1, 0, "Vertical velocity (omega)"},
  {11, "vertical_velocity", "m/s", 0, 2, 9, 1, 0, "Vertical velocity (geometric)"},
  {12, "absolute_vorticity", "1/s", 0, 2, 10, 1, 0, "Absolute vorticity"},
  {13, "geopotential_height", "m", 0, 3, 5, 1, 0, "Geopotential height (gpm)"},
  {14, "pressure", "Pa", 0, 3, 0, 1, 0, "Pressure"},
  {15, "pressure_msl", "Pa", 0, 3, 1, 1, 0, "Pressure reduced to mean sea level"},
  {16, "precipitation_amount", "kg/m2", 0, 1, 8, 1, 0, "Total precipitation (accumulated)"},
  {17, "precipitation_rate", "kg/m2/s", 0, 1, 7, 1, 0, "Precipitation rate"},
  {18, "precipitable_water", "kg/m2", 0, 1, 3, 1, 0, "Precipitable water"},
  {19, "snow_depth", "m", 0, 1, 11, 1, 0, "Snow depth"},
  {20, "snow_water_equivalent", "kg/m2", 0, 1, 13, 1, 0, "Water equivalent of accumulated snow depth"},
  {21, "cloud_cover_total", "1", 0, 6, 1, 0.01, 0, "Total cloud cover as a fraction"},
  {22, "cloud_cover_low", "1", 0, 6, 3, 0.01, 0, "Low cloud cover as a fraction"},
  {23, "cloud_cover_medium", "1", 0, 6, 4, 0.01, 0, "Medium cloud cover as a fraction"},
  {24, "cloud_cover_high", "1", 0, 6, 5, 0.01, 0, "High cloud cover as a fraction"},
  {25, "cloud_ceiling", "m", 0, 6, 13, 1, 0, "Ceiling"},
  {26, "visibility", "m", 0, 19, 0, 1, 0, "Visibility"},
  {27, "cape", "J/kg", 0, 7, 6, 1, 0, "Convective available potential energy"},
  {28, "cin", "J/kg", 0, 7, 7, 1, 0, "Convective inhibition"},
  {29, "lifted_index", "K", 0, 7, 0, 1, 0, "Parcel lifted index (to 500 hPa)"},
  {30, "temperature_max", "K", 0, 0, 4, 1, 0, "Maximum temperature"},
  {31, "temperature_min", "K", 0, 0, 5, 1, 0, "Minimum temperature"},
  {32, "shortwave_down", "W/m2", 0, 4, 7, 1, 0, "Downward shortwave radiation flux"},
  {33, "longwave_down", "W/m2", 0, 5, 3, 1, 0, "Downward longwave radiation flux"},
  {34, "altimeter_setting", "Pa", -1, -1, -1, 1, 0, "Altimeter setting (METAR) - no GRIB2 source"},
};
// clang-format on

}  // namespace atmo
