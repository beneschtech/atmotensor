// Internal helpers for strict JSON parsing (exports, manifests).  Not
// installed; include only from src/*.cpp.  Every error is
// std::invalid_argument("<what>: <problem>").
#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace atmo::detail
{

// `j` is an object whose keys are exactly `keys`.
void check_keys(
  const nlohmann::json& j,
  std::string_view what,
  std::initializer_list<const char*> keys);

std::uint64_t
get_u64(const nlohmann::json& j, const char* key, std::string_view what);
std::uint32_t
get_u32(const nlohmann::json& j, const char* key, std::string_view what);
std::int64_t
get_i64(const nlohmann::json& j, const char* key, std::string_view what);
// Finite number (integer or float).
double
get_double(const nlohmann::json& j, const char* key, std::string_view what);
std::string
get_str(const nlohmann::json& j, const char* key, std::string_view what);
const nlohmann::json&
get_array(const nlohmann::json& j, const char* key, std::string_view what);

// `j` has "format" == format and "version" == version.
void check_format(
  const nlohmann::json& j,
  std::string_view what,
  const char* format,
  int version);

// Parses JSON text; std::invalid_argument("<what>: invalid JSON: ...").
nlohmann::json parse_json(std::string_view text, std::string_view what);

// Whole file as a string; std::runtime_error("<what>: cannot read <path>").
std::string read_text_file(
  const std::filesystem::path& p,
  std::string_view what);

}  // namespace atmo::detail
