// GRIB2 parameter signatures (T15, 07-grib-catalog.md "Parameter
// identity").  Fixtures are generated with eccodes from its GRIB2 sample.
#include <eccodes.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "atmo/gribsig.h"
#include "atmo/grid.h"
#include "check.h"

using namespace atmo;

namespace
{

using Bytes = std::vector<unsigned char>;
using Sets = std::vector<std::pair<const char*, long>>;

void ck(int err, const char* what)
{
  if (err != 0)
  {
    throw std::runtime_error(
      std::string("eccodes test helper: ") + what + ": " +
      codes_get_error_message(err));
  }
}

// The sample with the template set first, then `sets` in order.
Bytes make(long pdt, const Sets& sets, long date = 20261110, long time = 0)
{
  codes_handle* h =
    codes_grib_handle_new_from_samples(nullptr, "regular_ll_sfc_grib2");
  if (!h)
  {
    throw std::runtime_error("no sample");
  }
  ck(codes_set_long(h, "productDefinitionTemplateNumber", pdt), "pdt");
  ck(codes_set_long(h, "dataDate", date), "dataDate");
  ck(codes_set_long(h, "dataTime", time), "dataTime");
  for (const auto& [k, v] : sets)
  {
    if (v == CODES_MISSING_LONG)
    {
      ck(codes_set_missing(h, k), k);
    }
    else
    {
      ck(codes_set_long(h, k, v), k);
    }
  }
  const void* buf = nullptr;
  std::size_t n = 0;
  ck(codes_get_message(h, &buf, &n), "get_message");
  Bytes out(
    static_cast<const unsigned char*>(buf),
    static_cast<const unsigned char*>(buf) + n);
  codes_handle_delete(h);
  return out;
}

GribHeader desc(const Bytes& b)
{
  return describe_grib(b.data(), b.size());
}

const Sets kTemp = {
  {"discipline", 0},
  {"parameterCategory", 0},
  {"parameterNumber", 0},
  {"typeOfFirstFixedSurface", 103},
  {"scaleFactorOfFirstFixedSurface", 0},
  {"scaledValueOfFirstFixedSurface", 2}};

Sets with(Sets s, const Sets& more)
{
  s.insert(s.end(), more.begin(), more.end());
  return s;
}

// 4.8 accumulated precipitation over [start, start + len] hours.
Bytes precip(long start_h, long len, long unit = 1, long date = 20261110)
{
  return make(
    8,
    {{"discipline", 0},
     {"parameterCategory", 1},
     {"parameterNumber", 8},
     {"typeOfFirstFixedSurface", 1},
     {"typeOfStatisticalProcessing", 1},
     {"indicatorOfUnitOfTimeRange", 1},
     {"forecastTime", start_h},
     {"indicatorOfUnitForTimeRange", unit},
     {"lengthOfTimeRange", len}},
    date);
}

bool contains_line(const std::string& canon, const std::string& line)
{
  return canon.find("\n" + line + "\n") != std::string::npos;
}

}  // namespace

TEST(canonical_text_is_sorted_versioned_and_hashed)
{
  const GribHeader h = desc(make(0, with(kTemp, {{"forecastTime", 6}})));
  const std::string& c = h.sig.canonical;
  CHECK(c.rfind("v2\n", 0) == 0);
  CHECK(c.back() == '\n');
  CHECK(h.sig.hash == fnv1a64(c.data(), c.size()));
  CHECK(h.sig.hash == h.sig.base_hash);  // not statistical
  CHECK(!h.sig.stat_interval_s);
  CHECK(contains_line(c, "centre=98"));  // sample centre (ECMWF)
  CHECK(contains_line(c, "discipline=0"));
  CHECK(contains_line(c, "productDefinitionTemplateNumber=0"));
  CHECK(contains_line(c, "parameterCategory=0"));
  CHECK(contains_line(c, "parameterNumber=0"));
  // Lines are sorted by key and no denied key made it in.
  std::vector<std::string> keys;
  std::size_t i = 3;
  while (i < c.size())
  {
    const std::size_t e = c.find('\n', i);
    const std::string line = c.substr(i, e - i);
    keys.push_back(line.substr(0, line.find('=')));
    i = e + 1;
  }
  CHECK(std::is_sorted(keys.begin(), keys.end()));
  for (const auto& k : keys)
  {
    CHECK(!signature_denies(k));
  }
  CHECK(h.sig.centre == 98 && h.sig.pdt == 0 && h.sig.category == 0);
}

TEST(time_level_process_and_date_do_not_change_the_signature)
{
  const GribHeader a = desc(make(0, with(kTemp, {{"forecastTime", 6}})));
  const GribHeader b = desc(make(
    0,
    with(
      kTemp, {{"forecastTime", 90},
              {"typeOfFirstFixedSurface", 100},
              {"scaleFactorOfFirstFixedSurface", 0},
              {"scaledValueOfFirstFixedSurface", 85000},
              {"generatingProcessIdentifier", 81},
              {"typeOfGeneratingProcess", 0},
              {"hoursAfterDataCutoff", 3}}),
    20261111, 1800));
  CHECK(a.sig.hash == b.sig.hash);
  CHECK(a.sig.canonical == b.sig.canonical);
  // ... but the header fields carry them.
  CHECK(a.offset_start_s == 6 * 3600 && a.offset_end_s == 6 * 3600);
  CHECK(b.offset_start_s == 90 * 3600);
  CHECK(a.base_time_s == 1794268800);  // 2026-11-10 00Z
  CHECK(b.base_time_s == 1794268800 + 86400 + 18 * 3600);
  CHECK(a.valid_time_s == a.base_time_s + 6 * 3600);
  CHECK(a.level1.type == 103 && a.level1.value == 2.0);
  CHECK(b.level1.type == 100 && b.level1.value == 85000.0);
  CHECK(a.level2.type == 255 && a.level2.value == 0.0);
  CHECK(b.process == 81);
  CHECK(a.member == -1 && !a.member_type);
}

TEST(a_different_parameter_has_a_different_signature)
{
  const GribHeader t = desc(make(0, kTemp));
  const GribHeader rh = desc(make(0, with(kTemp, {{"parameterNumber", 1}})));
  const GribHeader stat = desc(make(0, with(kTemp, {{"discipline", 10}})));
  CHECK(t.sig.hash != rh.sig.hash);
  CHECK(t.sig.hash != stat.sig.hash);
  const auto d = signature_diff(t.sig.canonical, rh.sig.canonical);
  CHECK(d.size() == 1 && d[0] == "parameterNumber: 0 -> 1");
}

TEST(table_names_do_not_depend_on_the_level)
{
  // eccodes' shortName/name are level-dependent (2 m: "2t"), the code
  // table 4.2 parameterName/parameterUnits are not (Q17b).
  const GribHeader at2m = desc(make(0, kTemp));
  const GribHeader at850 = desc(make(
    0, with(
         kTemp, {{"typeOfFirstFixedSurface", 100},
                 {"scaleFactorOfFirstFixedSurface", 0},
                 {"scaledValueOfFirstFixedSurface", 85000}})));
  CHECK(at2m.sig.hash == at850.sig.hash);
  CHECK(at2m.sig.ecc_short_name != at850.sig.ecc_short_name);
  CHECK(at2m.sig.table_name == "Temperature");
  CHECK(at850.sig.table_name == "Temperature");
  CHECK(at2m.sig.table_units == "K" && at850.sig.table_units == "K");
  // A centre-local number has no table 4.2 entry: eccodes gives only the
  // number, which is not kept.
  const GribHeader local = desc(make(
    0, with(
         kTemp, {{"centre", 7},
                 {"parameterCategory", 1},
                 {"parameterNumber", 192},
                 {"typeOfFirstFixedSurface", 1}})));
  CHECK(local.sig.table_name.empty());
  CHECK(local.sig.table_units.empty());
  CHECK(local.sig.ecc_short_name == "crain");
}

TEST(level_values_are_si)
{
  const GribHeader h = desc(make(
    0, with(
         kTemp, {{"typeOfFirstFixedSurface", 106},
                 {"scaleFactorOfFirstFixedSurface", 2},
                 {"scaledValueOfFirstFixedSurface", 10},
                 {"typeOfSecondFixedSurface", 106},
                 {"scaleFactorOfSecondFixedSurface", 1},
                 {"scaledValueOfSecondFixedSurface", 4}})));
  CHECK(h.level1.type == 106 && h.level1.value == 0.1);
  CHECK(h.level2.type == 106 && h.level2.value == 0.4);
}

TEST(statistical_buckets_versus_since_run_start)
{
  CHECK(desc(precip(3, 3)).sig.stat_process == 1u);
  CHECK(!desc(make(0, kTemp)).sig.stat_process);
  const GribHeader b3a = desc(precip(3, 3));       // 3-6 h
  const GribHeader b3b = desc(precip(9, 3));       // 9-12 h
  const GribHeader b1 = desc(precip(5, 1));        // 5-6 h
  const GribHeader r3 = desc(precip(0, 3));        // 0-3 h: run
  const GribHeader r6 = desc(precip(0, 6));        // 0-6 h: run
  const GribHeader m60 = desc(precip(3, 180, 0));  // 3 h in minutes

  CHECK(b3a.sig.hash == b3b.sig.hash);
  CHECK(b3a.sig.stat_interval_s && *b3a.sig.stat_interval_s == 10800);
  CHECK(contains_line(b3a.sig.canonical, "statInterval=10800"));
  CHECK(b3a.sig.hash != b1.sig.hash);
  CHECK(r3.sig.hash == r6.sig.hash);  // run total grows, one parameter
  CHECK(r3.sig.stat_interval_s && *r3.sig.stat_interval_s == 0);
  CHECK(contains_line(r3.sig.canonical, "statInterval=run"));
  CHECK(r3.sig.hash != b3a.sig.hash);
  CHECK(m60.sig.hash == b3a.sig.hash);  // units normalised to seconds
  // Siblings share base_hash, which differs from every full hash.
  CHECK(b3a.sig.base_hash == b1.sig.base_hash);
  CHECK(b3a.sig.base_hash == r3.sig.base_hash);
  CHECK(b3a.sig.base_hash != b3a.sig.hash);
  // Offsets: start of the interval .. its end.
  CHECK(b3a.offset_start_s == 3 * 3600 && b3a.offset_end_s == 6 * 3600);
  CHECK(r6.offset_start_s == 0 && r6.offset_end_s == 6 * 3600);
  CHECK(b3a.valid_time_s == b3a.base_time_s + 6 * 3600);
  // A different statistic is a different parameter.
  const GribHeader mx = desc(make(
    8, {{"parameterCategory", 1},
        {"parameterNumber", 8},
        {"typeOfFirstFixedSurface", 1},
        {"typeOfStatisticalProcessing", 2},
        {"forecastTime", 3},
        {"lengthOfTimeRange", 3}}));
  CHECK(mx.sig.hash != b3a.sig.hash);
  CHECK(mx.sig.base_hash != b3a.sig.base_hash);
}

TEST(ensemble_members_are_runs_not_parameters)
{
  auto member = [](long pdt, long type, long pert, long ft)
  {
    return desc(make(
      pdt, with(
             kTemp, {{"typeOfEnsembleForecast", type},
                     {"perturbationNumber", pert},
                     {"numberOfForecastsInEnsemble", 31},
                     {"forecastTime", ft}})));
  };
  const GribHeader c = member(1, 1, 0, 6);
  const GribHeader p1 = member(1, 3, 1, 6);
  const GribHeader p7 = member(1, 3, 7, 12);
  CHECK(c.sig.hash == p1.sig.hash && p1.sig.hash == p7.sig.hash);
  CHECK(c.member == 0 && p1.member == 1 && p7.member == 7);
  CHECK(c.member_type && *c.member_type == 1);
  CHECK(p7.member_type && *p7.member_type == 3);
  // Not the same parameter as the deterministic template 4.0 one.
  CHECK(c.sig.hash != desc(make(0, kTemp)).sig.hash);

  // 4.11: ensemble + statistical.
  auto acc = [](long pert, long start, long len)
  {
    return desc(make(
      11, {{"parameterCategory", 1},
           {"parameterNumber", 8},
           {"typeOfFirstFixedSurface", 1},
           {"typeOfEnsembleForecast", 3},
           {"perturbationNumber", pert},
           {"numberOfForecastsInEnsemble", 31},
           {"typeOfStatisticalProcessing", 1},
           {"forecastTime", start},
           {"lengthOfTimeRange", len}}));
  };
  const GribHeader a = acc(2, 6, 6);
  const GribHeader b = acc(5, 12, 6);
  const GribHeader r = acc(5, 0, 12);
  CHECK(a.sig.hash == b.sig.hash);
  CHECK(a.member == 2 && b.member == 5);
  CHECK(a.sig.stat_interval_s && *a.sig.stat_interval_s == 6 * 3600);
  CHECK(r.sig.hash != a.sig.hash && r.sig.base_hash == a.sig.base_hash);

  // 4.2 derived (ensemble mean): member -1, derivedForecast kept.
  const GribHeader mean = desc(make(
    2,
    with(
      kTemp, {{"derivedForecast", 0}, {"numberOfForecastsInEnsemble", 31}})));
  const GribHeader spread = desc(make(
    2,
    with(
      kTemp, {{"derivedForecast", 4}, {"numberOfForecastsInEnsemble", 31}})));
  CHECK(mean.member == -1);
  CHECK(mean.sig.hash != spread.sig.hash);
  CHECK(contains_line(mean.sig.canonical, "derivedForecast=0"));
}

TEST(aerosol_template_keys_come_in_without_code_changes)
{
  auto aer = [](long type, long size, long ft)
  {
    return desc(make(
      48, {{"discipline", 0},
           {"parameterCategory", 20},
           {"parameterNumber", 102},
           {"aerosolType", type},
           {"typeOfSizeInterval", 0},
           {"scaleFactorOfFirstSize", 8},
           {"scaledValueOfFirstSize", size},
           {"typeOfWavelengthInterval", 255},
           {"typeOfFirstFixedSurface", 105},
           {"scaledValueOfFirstFixedSurface", 1},
           {"forecastTime", ft}}));
  };
  const GribHeader dust = aer(62001, 2, 0);
  const GribHeader dust_later = aer(62001, 2, 9);
  const GribHeader smoke = aer(62012, 2, 0);
  const GribHeader dust_big = aer(62001, 20, 0);
  CHECK(dust.sig.pdt == 48);
  CHECK(dust.sig.hash == dust_later.sig.hash);
  CHECK(dust.sig.hash != smoke.sig.hash);
  CHECK(dust.sig.hash != dust_big.sig.hash);
  // eccodes 2.45 calls aerosolType constituentType (07: key names come
  // from the definition files).
  CHECK(
    contains_line(dust.sig.canonical, "constituentType=62001") ||
    contains_line(dust.sig.canonical, "aerosolType=62001"));
  const auto d = signature_diff(dust.sig.canonical, dust_big.sig.canonical);
  CHECK(d.size() == 1 && d[0] == "scaledValueOfFirstSize: 2 -> 20");
}

TEST(model_version_date_does_not_make_a_new_parameter)
{
  // Q18b (signature v2): a model upgrade keeps the parameter.  Template
  // 4.60 (reforecast member) carries the model version date.
  const Sets base = with(
    kTemp, {{"typeOfEnsembleForecast", 3},
            {"perturbationNumber", 1},
            {"numberOfForecastsInEnsemble", 10}});
  const GribHeader old_model = desc(make(
    60,
    with(base, {{"YearOfModelVersion", 2024}, {"MonthOfModelVersion", 3}})));
  const GribHeader new_model = desc(make(
    60,
    with(base, {{"YearOfModelVersion", 2026}, {"MonthOfModelVersion", 11}})));
  CHECK(old_model.sig.pdt == 60);
  CHECK(old_model.sig.hash == new_model.sig.hash);
  CHECK(old_model.sig.canonical.find("ModelVersion") == std::string::npos);
  CHECK(old_model.sig.version == 2 && kSignatureVersion == 2);
}

TEST(deny_list_matching)
{
  CHECK(signature_denies("YearOfModelVersion"));
  CHECK(signature_denies("yearOfModelVersion"));
  CHECK(signature_denies("SecondOfModelVersion"));
  CHECK(signature_denies("forecastTime"));
  CHECK(signature_denies("lengthOfTimeRange#2"));
  CHECK(signature_denies("wallClockInitialTimeOfExecutionYear"));
  CHECK(signature_denies("perturbationNumber"));
  CHECK(!signature_denies("parameterNumber"));
  CHECK(!signature_denies("typeOfStatisticalProcessing"));
  CHECK(!signature_denies("aerosolType"));
  CHECK(!signature_denies("forecastTimeX"));
  for (const auto& r : signature_deny_list())
  {
    const std::string g = r.group;
    CHECK(
      g == "time" || g == "level" || g == "member" || g == "process" ||
      g == "model");
  }
}

TEST(undecodable_template_gets_a_raw_signature)
{
  Bytes m = make(0, with(kTemp, {{"forecastTime", 6}}));
  // Section 4 template number -> one eccodes has no definition for.
  std::size_t i = 16;
  std::size_t s4 = 0;
  while (i + 5 < m.size())
  {
    const std::size_t len = (std::size_t(m[i]) << 24) |
                            (std::size_t(m[i + 1]) << 16) |
                            (std::size_t(m[i + 2]) << 8) | m[i + 3];
    if (m[i + 4] == 4)
    {
      s4 = i;
      break;
    }
    i += len;
  }
  CHECK(s4 != 0);
  m[s4 + 7] = 0xEE;  // pdt 61000-ish
  m[s4 + 8] = 0x11;
  const GribHeader raw = describe_grib_raw(m.data(), m.size());
  CHECK(raw.sig.undecodable);
  CHECK(raw.sig.pdt == 0xEE11);
  CHECK(raw.sig.category == 0 && raw.sig.number == 0);
  CHECK(raw.sig.centre == 98);
  CHECK(raw.base_time_s == 1794268800);
  CHECK(contains_line(
    raw.sig.canonical,
    "productDefinitionTemplateNumber=" + std::to_string(0xEE11)));
  CHECK(raw.sig.canonical.find("\nraw4=") != std::string::npos);
  // describe_grib falls back to it.
  const GribHeader auto_ = desc(m);
  CHECK(auto_.sig.undecodable && auto_.sig.hash == raw.sig.hash);
  // Time is inside the raw bytes: another hour is another signature.
  Bytes m2 = make(0, with(kTemp, {{"forecastTime", 7}}));
  m2[s4 + 7] = 0xEE;
  m2[s4 + 8] = 0x11;
  CHECK(desc(m2).sig.hash != raw.sig.hash);
}

TEST(non_grib2_is_rejected)
{
  const Bytes junk(64, 'x');
  bool threw = false;
  try
  {
    describe_grib(junk.data(), junk.size());
  }
  catch (const std::runtime_error&)
  {
    threw = true;
  }
  CHECK(threw);
}

TEST(message_spans)
{
  const Bytes a = make(0, kTemp);
  const Bytes b = precip(3, 3);
  Bytes buf = {'x', 'G', 'R'};
  buf.insert(buf.end(), a.begin(), a.end());
  buf.insert(buf.end(), {'G', 'R', 'I', 'B', 0});  // a false start
  buf.insert(buf.end(), b.begin(), b.end());
  const auto spans = grib_message_spans(buf.data(), buf.size());
  CHECK(spans.size() == 2);
  if (spans.size() == 2)
  {
    CHECK(spans[0] == (GribSpan{3, a.size(), 2}));
    CHECK(spans[1].offset == 3 + a.size() + 5 && spans[1].length == b.size());
    CHECK(
      desc(b).sig.hash ==
      describe_grib(buf.data() + spans[1].offset, spans[1].length).sig.hash);
  }
  Bytes cut(a.begin(), a.end() - 10);
  bool threw = false;
  try
  {
    grib_message_spans(cut.data(), cut.size());
  }
  catch (const std::runtime_error&)
  {
    threw = true;
  }
  CHECK(threw);
}

int main()
{
  return tst::run_all();
}
