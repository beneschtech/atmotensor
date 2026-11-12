// GRIB2 parameter signatures (D13, docs/design/07-grib-catalog.md
// "Parameter identity"; task T15).  Only built with eccodes
// (ATMO_HAVE_ECCODES); this header does not include eccodes.h.
//
// A message's signature is everything in its headers that says what the
// field is: discipline, centre, and every coded integer key of section 4
// except the deny list (time, level, ensemble member, process
// bookkeeping), plus one synthetic key `statInterval` for statistically
// processed templates:
//   statInterval=<seconds>  the interval starts after the base time
//                           (a fixed-length bucket),
//   statInterval=run        it starts at the base time (since run start).
// Calendar units (month, year, ...) are written as `<n>u<code>` (code
// table 4.4) instead of seconds; with several time ranges every range is
// listed in order, comma separated.
//
// Canonical text = "v1\n" + `key=value` lines sorted by key, each ending
// in LF.  hash = FNV-1a 64 of that text; base_hash = the same without the
// statInterval line.  Repeated keys (one per time range) are numbered
// `key`, `key#2`, `key#3`, ...; a missing value is written `missing`.
//
// A message eccodes cannot decode gets a raw signature: discipline,
// centre, template number and the hex of section 4 from octet 10 on
// (time and level included, so each such message is its own signature).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct grib_handle;  // eccodes' codes_handle

namespace atmo
{

// 1: 07-grib-catalog.md's list (+ eccodes spellings, Q13c).
// 2: + the model version date (Q18b: a model upgrade keeps its parameters).
constexpr int kSignatureVersion = 2;

// The deny list of kSignatureVersion: key names, or prefixes ending in '*'.
// Changing it means bumping kSignatureVersion (old rows keep theirs).
struct SignatureDenyRule
{
  const char* key;
  const char* group;  // time, level, member, process, model
};
const std::vector<SignatureDenyRule>& signature_deny_list();
bool signature_denies(std::string_view key);

struct GribSignature
{
  int version = kSignatureVersion;
  std::string canonical;
  std::uint64_t hash = 0;
  std::uint64_t base_hash = 0;
  // nullopt: not statistically processed; 0: since run start; else the
  // (first) interval length in seconds (calendar units: nominal 30 d month,
  // 365 d year).
  std::optional<std::uint32_t> stat_interval_s;
  // Code table 4.10 (0 average, 1 accumulation, 2 maximum, ...) of the
  // first range of a statistical parameter; nullopt otherwise.
  std::optional<std::uint32_t> stat_process;
  std::uint32_t centre = 0;
  std::uint32_t discipline = 0;
  std::uint32_t category = 255;
  std::uint32_t number = 255;
  std::uint32_t pdt = 0;
  bool undecodable = false;
  // What eccodes says (not part of the identity).  An unknown parameter
  // has ecc_short_name "unknown" and no ecc_param_id.
  std::string ecc_short_name;
  std::string ecc_name;
  std::string ecc_units;
  std::optional<long> ecc_param_id;
  // Code table 4.2 name and units (eccodes parameterName/parameterUnits):
  // unlike shortName/name they do not depend on the level ("2t" vs "t").
  // Empty when the table has no entry (eccodes then answers the bare
  // parameter number, e.g. for centre-local numbers 192-254).
  std::string table_name;
  std::string table_units;
  bool identified() const;
};

// Code table 4.5 type; value in SI (scaled value / 10^scale factor).
// Missing surface: type 255, value 0.
struct GribLevel
{
  std::uint32_t type = 255;
  double value = 0.0;
  bool operator==(const GribLevel&) const = default;
};

// Everything the catalog needs to know about one message's header.
struct GribHeader
{
  GribSignature sig;
  std::uint32_t edition = 2;
  std::uint32_t subcentre = 0;
  std::uint32_t process = 255;   // generatingProcessIdentifier
  std::int64_t base_time_s = 0;  // unix seconds, UTC
  std::int64_t offset_start_s = 0;
  std::int64_t offset_end_s = 0;  // = start for instantaneous fields
  std::int64_t valid_time_s = 0;  // base + offset_end
  GribLevel level1;
  GribLevel level2;
  int member = -1;  // -1 not an ensemble member, 0 control, 1.. perturbed
  std::optional<int> member_type;  // typeOfEnsembleForecast
};

// From a decoded handle.  std::runtime_error if it is not GRIB2.
GribHeader describe_grib(grib_handle* h);

// From the bytes of one message: decodes it with eccodes, and falls back
// to raw_signature() if that fails.  std::runtime_error if the bytes are
// not a GRIB2 message at all.
GribHeader describe_grib(const void* msg, std::size_t len);

// The undecodable path on its own (header fields that can be read from
// sections 0, 1 and the common start of section 4 are filled in).
// std::runtime_error if the bytes are not a GRIB2 message.
GribHeader describe_grib_raw(const void* msg, std::size_t len);

// Canonical text -> signature fields (hash, base_hash).
std::uint64_t signature_hash(std::string_view canonical);

// Keys whose lines differ between two canonical texts ("k: a -> b",
// "k: a -> (absent)"), sorted by key.
std::vector<std::string> signature_diff(std::string_view a, std::string_view b);

struct GribSpan
{
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
  std::uint32_t edition = 0;
  bool operator==(const GribSpan&) const = default;
};

// The messages in a buffer, found by their "GRIB" header and length
// (edition 1 and 2); bytes between messages are skipped.  A truncated last
// message is a std::runtime_error.
std::vector<GribSpan> grib_message_spans(const void* data, std::size_t len);

}  // namespace atmo
