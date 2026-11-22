// atmo_productd end to end (T21, T22) on a throwaway catalog database
// (tests/catalog_testdb.sh; ATMO_TEST_CATALOG=<config>; exit 77 without):
// synthetic GFS u/v are catalogued, products are defined the way the web
// app writes them, and ProductDaemon::step() is driven pass by pass:
// rebuild requests, arrivals, chained products, kernel revisions and
// builds, failures with retries, replaced inputs, output naming.
#include <eccodes.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/catalog.h"
#include "atmo/grib.h"
#include "atmo/productd.h"
#include "atmo/products_db.h"
#include "check.h"
#ifdef ATMO_HAVE_OPENCL
#include "atmo/opencl.h"
#endif

using namespace atmo;
namespace fs = std::filesystem;

namespace
{

const char* kWindSpeed = R"(__kernel void wind_speed(__global const float* u,
                         __global const float* v,
                         __global float* speed,
                         const int ni,
                         const int nj)
{
  const int k = get_global_id(0);
  if (k >= ni * nj)
  {
    return;
  }
  speed[k] = hypot(u[k], v[k]);
}
)";

CatalogConfig g_cfg;
fs::path g_root;
std::unique_ptr<Catalog> g_db;
bool g_have_cl = false;

std::string q(const std::string& sql, int col = 0)
{
  const auto r = g_db->query(sql);
  return r.empty() || !r[0][col] ? std::string() : *r[0][col];
}

long n(const std::string& sql)
{
  const std::string s = q(sql);
  return s.empty() ? 0 : std::stol(s);
}

void ck(int err, const char* what)
{
  if (err != 0)
  {
    throw std::runtime_error(std::string("eccodes: ") + what);
  }
}

// One 10 m wind component (number 2 = u, 3 = v) on a 6x4 grid scanning
// north to south, GFS-like (centre 7, process 96), base 2026-11-21 00Z.
std::vector<unsigned char> wind(int number, int fhr, double scale = 1.0)
{
  codes_handle* h =
    codes_grib_handle_new_from_samples(nullptr, "regular_ll_sfc_grib2");
  ck(h ? 0 : 1, "sample");
  ck(codes_set_long(h, "centre", 7), "centre");
  ck(codes_set_long(h, "subCentre", 0), "sub");
  ck(codes_set_long(h, "generatingProcessIdentifier", 96), "gpi");
  ck(codes_set_long(h, "typeOfGeneratingProcess", 2), "type");
  ck(codes_set_long(h, "dataDate", 20261121), "date");
  ck(codes_set_long(h, "dataTime", 0), "time");
  ck(codes_set_long(h, "Ni", 6), "Ni");
  ck(codes_set_long(h, "Nj", 4), "Nj");
  ck(codes_set_long(h, "jScansPositively", 0), "jpos");
  ck(codes_set_double(h, "latitudeOfFirstGridPointInDegrees", 41.0), "la1");
  ck(codes_set_double(h, "latitudeOfLastGridPointInDegrees", 38.0), "la2");
  ck(codes_set_double(h, "longitudeOfFirstGridPointInDegrees", 250.0), "lo1");
  ck(codes_set_double(h, "longitudeOfLastGridPointInDegrees", 255.0), "lo2");
  ck(codes_set_double(h, "iDirectionIncrementInDegrees", 1.0), "di");
  ck(codes_set_double(h, "jDirectionIncrementInDegrees", 1.0), "dj");
  ck(codes_set_long(h, "discipline", 0), "disc");
  ck(codes_set_long(h, "parameterCategory", 2), "cat");
  ck(codes_set_long(h, "parameterNumber", number), "num");
  ck(codes_set_long(h, "typeOfFirstFixedSurface", 103), "lt");
  ck(codes_set_long(h, "scaleFactorOfFirstFixedSurface", 0), "sf");
  ck(codes_set_long(h, "scaledValueOfFirstFixedSurface", 10), "sv");
  ck(codes_set_long(h, "indicatorOfUnitOfTimeRange", 1), "unit");
  ck(codes_set_long(h, "forecastTime", fhr), "ft");
  std::size_t len = std::strlen("grid_ieee");
  ck(codes_set_string(h, "packingType", "grid_ieee", &len), "packing");
  std::vector<double> v(24);
  for (std::size_t k = 0; k < v.size(); ++k)
  {
    v[k] = scale * (number == 2 ? 3.0 + static_cast<double>(k) * 0.5
                                : 4.0 - static_cast<double>(fhr));
  }
  ck(codes_set_double_array(h, "values", v.data(), v.size()), "values");
  const void* buf = nullptr;
  std::size_t blen = 0;
  ck(codes_get_message(h, &buf, &blen), "message");
  std::vector<unsigned char> out(
    static_cast<const unsigned char*>(buf),
    static_cast<const unsigned char*>(buf) + blen);
  codes_handle_delete(h);
  return out;
}

// Catalogue u and v of one forecast hour as one file.
void ingest_hour(int fhr, double uscale = 1.0, const std::string& tag = "")
{
  const fs::path src = g_root.parent_path() /
                       ("gfs.t00z.f" + std::to_string(fhr) + tag + ".grib2");
  {
    std::ofstream f(src, std::ios::binary | std::ios::trunc);
    for (int num : {2, 3})
    {
      const auto b = wind(num, fhr, num == 2 ? uscale : 1.0);
      f.write(
        reinterpret_cast<const char*>(b.data()),
        static_cast<std::streamsize>(b.size()));
    }
  }
  g_db->ingest(catalog_grib(g_root, src, "gfs"));
}

// Common names as app:grib:seed-common-names would set them.
void common_names()
{
  for (auto [name, num] : std::vector<std::pair<std::string, int>>{
         {"wind_u", 2}, {"wind_v", 3}, {"wind_speed", 1}})
  {
    g_db->query(
      "INSERT IGNORE INTO grib_common_name (name, parameter_id) SELECT '" +
      name +
      "', parameter_id FROM grib_signature WHERE discipline = 0 AND category = "
      "2 AND number = " +
      std::to_string(num) + " AND parameter_id IS NOT NULL LIMIT 1");
  }
}

std::uint64_t add_kernel(
  const std::string& name,
  const std::string& src,
  const std::string& entry)
{
  g_db->query(
    "INSERT INTO derived_kernel (name, revision, source, entry, inputs, "
    "outputs, params, source_hash, build_status, updated_at) "
    "VALUES (" +
    g_db->quote(name) + ", 1, " + g_db->quote(src) + ", " + g_db->quote(entry) +
    ", "
    "'[{\"role\":\"u\",\"units\":\"m/s\"},{\"role\":\"v\",\"units\":\"m/"
    "s\"}]', '[{\"role\":\"speed\",\"units\":\"m/s\"}]', '[]', "
    "'0123456789abcdef', 'pending', UTC_TIMESTAMP())");
  return std::stoull(q("SELECT LAST_INSERT_ID()"));
}

// A product as the web app writes it (rebuild requested, as on create).
std::uint64_t add_product(
  const std::string& slug,
  int pid,
  const std::string& kind,
  const std::string& proc_or_kernel,
  const std::vector<std::vector<std::string>>&
    inputs,  // role, rule, model, field, own, step
  int out_num)
{
  const bool kernel = kind == "kernel";
  g_db->query(
    "INSERT INTO derived_product (slug, name, processor_kind, processor, "
    "kernel_id, params, trigger_kind, enabled, process_id, "
    "revision, rebuild_requested_at, updated_at) VALUES (" +
    g_db->quote(slug) + ", " + g_db->quote(slug + " product") + ", " +
    g_db->quote(kind) + ", " + (kernel ? "NULL" : g_db->quote(proc_or_kernel)) +
    ", " + (kernel ? proc_or_kernel : "NULL") + ", '{}', 'offset', 1, " +
    std::to_string(pid) + ", 1, UTC_TIMESTAMP(), UTC_TIMESTAMP())");
  const std::string id = q("SELECT LAST_INSERT_ID()");
  for (const auto& i : inputs)
  {
    const bool own = i[1] == "own_previous";
    g_db->query(
      "INSERT INTO derived_product_input (product_id, role, offset_rule, "
      "model_grid_api, field, own_output_role, step_s, required) VALUES (" +
      id + ", " + g_db->quote(i[0]) + ", " + g_db->quote(i[1]) + ", " +
      (own ? "NULL" : g_db->quote(i[2])) + ", " +
      (own ? "NULL" : g_db->quote(i[3])) + ", " +
      (own ? g_db->quote(i[4]) : "NULL") + ", " +
      (i[1] == "same" ? "NULL" : i[5]) + ", 1)");
  }
  g_db->query(
    "INSERT INTO derived_product_output (product_id, role, discipline, "
    "category, number, level1_type, level1_value, level2_type, "
    "level2_value, units) VALUES (" +
    id + ", '" + (out_num == 0 ? "dir" : "speed") + "', 0, 2, " +
    std::to_string(out_num) + ", 103, 10, 255, 0, '')");
  return std::stoull(id);
}

std::unique_ptr<ProductDaemon> daemon()
{
  ProductDaemonOptions o;
  o.root = g_root;
  o.tmp = g_root.parent_path() / "tmp";
  o.catalog = g_cfg;
  o.worker = "test:" + std::to_string(::getpid());
  std::unique_ptr<KernelExecutor> k;
#ifdef ATMO_HAVE_OPENCL
  if (g_have_cl)
  {
    k = make_opencl_executor({}, 30);
  }
#endif
  return std::make_unique<ProductDaemon>(o, std::move(k));
}

// The values of a catalogued message, file order.
std::vector<double> values_of(std::uint64_t id)
{
  const auto r = g_db->query(
    "SELECT f.path, m.byte_offset, m.byte_length FROM grib_message m JOIN "
    "grib_file f ON f.id = m.file_id WHERE m.id = " +
    std::to_string(id));
  if (r.empty())
  {
    return {};
  }
  std::ifstream f(g_root / *r[0][0], std::ios::binary);
  f.seekg(static_cast<std::streamoff>(std::stoull(*r[0][1])));
  std::vector<unsigned char> b(std::stoull(*r[0][2]));
  f.read(
    reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
  return atmo::grib_values(b.data(), b.size());
}

std::uint64_t msg(const std::string& api, int num, int fhr)
{
  return std::stoull(q(
    "SELECT m.id FROM grib_message m JOIN grib_signature s ON s.id = "
    "m.signature_id JOIN grib_run r ON r.id = m.run_id "
    "JOIN grib_model_grid mg ON mg.id = r.model_grid_id WHERE mg.api_name = " +
    g_db->quote(api) + " AND s.number = " + std::to_string(num) +
    " AND m.offset_end_s = " + std::to_string(fhr * 3600)));
}

}  // namespace

TEST(productd_end_to_end)
{
  ingest_hour(0);
  ingest_hour(3);
  common_names();
  const std::string gfs = q("SELECT api_name FROM grib_model_grid LIMIT 1");
  CHECK(!gfs.empty());

  // ws: the seed kernel (or, without OpenCL, the built-in) on GFS u/v.
  const bool cl = g_have_cl;
  const auto kid = add_kernel("wind_speed", kWindSpeed, "wind_speed");
  const auto ws = add_product(
    "ws10", 1, cl ? "kernel" : "builtin",
    cl ? std::to_string(kid) : "wind_speed",
    {{"u", "same", gfs, "wind_u@10m", "", ""},
     {"v", "same", gfs, "wind_v@10m", "", ""}},
    1);
  // dir: the built-in direction on the same inputs.
  const auto dir = add_product(
    "dir10", 2, "builtin", "wind_direction",
    {{"u", "same", gfs, "wind_u@10m", "", ""},
     {"v", "same", gfs, "wind_v@10m", "", ""}},
    0);
  // chain: wind speed again, but from ws10's own output (u) and GFS v.
  const auto chain = add_product(
    "chain", 3, "builtin", "wind_speed",
    {{"u", "same", "ws10", "wind_speed@10m", "", ""},
     {"v", "same", gfs, "wind_v@10m", "", ""}},
    1);

  auto d = daemon();
  auto st = d->step();
  for (const auto& l : st.log)
  {
    std::printf("  pass 1: %s\n", l.c_str());
  }
  CHECK(n("SELECT COUNT(*) FROM derived_processor") == 2);
  CHECK(n("SELECT schema_version FROM derived_state") == 2);
  CHECK(q("SELECT heartbeat_at IS NOT NULL FROM derived_state") == "1");
  CHECK(st.rebuilds == 3);
  CHECK(st.kernels_built == 1);
  CHECK(q("SELECT build_status FROM derived_kernel") == (cl ? "ok" : "error"));
  // ws and dir: f000 and f003 each.  chain: nothing yet (no ws output when
  // it was planned).
  CHECK(st.ran == 4 && st.failed == 0);
  CHECK(n("SELECT COUNT(*) FROM derived_job WHERE status = 'done'") == 4);
  CHECK(
    n("SELECT COUNT(*) FROM derived_product WHERE rebuild_requested_at IS NOT "
      "NULL") == 0);
  CHECK(
    std::stoull(q("SELECT cursor_seq FROM derived_state")) == st.generation);

  // Outputs: named, api_name = slug, values = hypot(u, v).
  CHECK(
    q("SELECT m.name FROM grib_model m JOIN grib_subcenter s ON s.id = "
      "m.subcenter_id JOIN grib_center c ON c.id = s.center_id "
      "WHERE c.code = 254 AND m.process_id = 1") == "ws10 product");
  CHECK(n("SELECT COUNT(*) FROM grib_model_grid WHERE api_name = 'ws10'") == 1);
  const auto u0 = values_of(msg(gfs, 2, 0)), v0 = values_of(msg(gfs, 3, 0)),
             s0 = values_of(msg("ws10", 1, 0));
  bool same = s0.size() == 24 && u0.size() == 24;
  for (std::size_t k = 0; same && k < 24; ++k)
  {
    same = std::fabs(s0[k] - std::hypot(u0[k], v0[k])) < 1e-3;
  }
  CHECK(same);
  // Directions: v = 4 (from the south-ish), u > 0: between 180 and 270.
  const auto d0 = values_of(msg("dir10", 0, 0));
  CHECK(d0.size() == 24 && d0[0] > 180.0 && d0[0] < 270.0);
  // The output file lives under grib/derived-1/<base>/ and the job knows it.
  const std::string job3 = q(
    "SELECT id FROM derived_job WHERE offset_end_s = 10800 AND product_id = " +
    std::to_string(ws));
  CHECK(
    q("SELECT f.path FROM grib_message m JOIN grib_file f ON f.id = m.file_id "
      "WHERE m.id = " +
      std::to_string(msg("ws10", 1, 3)))
      .rfind("grib/derived-1/2026112100/ws10.f003.j" + job3 + ".", 0) == 0);
  CHECK(
    q("SELECT JSON_EXTRACT(outputs, '$.speed') FROM derived_job WHERE "
      "product_id = " +
      std::to_string(ws) + " AND offset_end_s = 0") ==
    std::to_string(msg("ws10", 1, 0)));
  CHECK(
    q("SELECT device FROM derived_job WHERE product_id = " +
      std::to_string(dir) + " LIMIT 1") == "cpu");

  // Pass 2: ws outputs arrived -> chain f000, f003.  Pass 3: a new GFS hour
  // -> ws/dir f006, then pass 4 -> chain f006.
  st = d->step();
  CHECK(st.arrivals >= 2 && st.ran == 2);
  CHECK(
    n("SELECT COUNT(*) FROM derived_job WHERE product_id = " +
      std::to_string(chain) + " AND status = 'done'") == 2);
  ingest_hour(6);
  st = d->step();
  CHECK(st.ran == 2);
  st = d->step();
  CHECK(st.ran == 1);
  CHECK(n("SELECT COUNT(*) FROM derived_job WHERE status = 'done'") == 9);
  // chain = hypot(hypot(u, v), v)
  const auto c6 = values_of(msg("chain", 1, 6)), u6 = values_of(msg(gfs, 2, 6)),
             v6 = values_of(msg(gfs, 3, 6));
  CHECK(
    c6.size() == 24 &&
    std::fabs(c6[5] - std::hypot(std::hypot(u6[5], v6[5]), v6[5])) < 1e-2);
  st = d->step();
  CHECK(st.ran == 0 && st.jobs_created == 0);  // quiet

  // A newer copy of f000 u (other bytes): ws/dir f000 re-run with the new
  // message; chain follows once the new ws output arrives.
  ingest_hour(0, 2.0, "b");
  st = d->step();
  CHECK(st.jobs_requeued == 2 && st.ran == 2);
  const auto u0b = values_of(msg(gfs, 2, 0)),
             s0b = values_of(msg("ws10", 1, 0));
  CHECK(
    s0b.size() == 24 && std::fabs(s0b[3] - std::hypot(u0b[3], v0[3])) < 1e-3);
  st = d->step();
  CHECK(st.jobs_requeued == 1 && st.ran == 1);

  if (cl)
  {
    // Kernel revision 2: built on the next pass; outputs redone on Rebuild.
    g_db->query(
      "UPDATE derived_kernel SET revision = 2, source = " +
      g_db->quote(std::string("// v2\n") + kWindSpeed) +
      ", source_hash = 'fedcba9876543210', build_status = 'pending' WHERE id "
      "= " +
      std::to_string(kid));
    st = d->step();
    CHECK(
      st.kernels_built == 1 &&
      q("SELECT built_revision FROM derived_kernel") == "2");
    g_db->query(
      "UPDATE derived_product SET rebuild_requested_at = UTC_TIMESTAMP() WHERE "
      "id = " +
      std::to_string(ws));
    st = d->step();
    CHECK(
      st.rebuilds == 1 && st.jobs_created == 3 && st.jobs_superseded == 3 &&
      st.ran == 3);
    CHECK(
      n("SELECT COUNT(*) FROM derived_job WHERE product_id = " +
        std::to_string(ws) +
        " AND status = 'superseded' AND kernel_revision = 1") == 3);

    // A kernel that does not build: its product's jobs fail, with the log.
    g_db->query(
      "UPDATE derived_kernel SET revision = 3, source = '__kernel void "
      "wind_speed(', source_hash = 'badbadbadbadbad0', "
      "build_status = 'pending' WHERE id = " +
      std::to_string(kid));
    g_db->query(
      "UPDATE derived_product SET rebuild_requested_at = UTC_TIMESTAMP() WHERE "
      "id = " +
      std::to_string(ws));
    st = d->step();
    CHECK(
      q("SELECT build_status FROM derived_kernel") == "error" &&
      !q("SELECT build_log FROM derived_kernel").empty());
    CHECK(st.failed == 3);         // first attempt: back to queued, due in 60 s
    CHECK(d->step().failed == 0);  // not due yet
    g_db->query(
      "UPDATE derived_job SET queued_at = UTC_TIMESTAMP() - INTERVAL 1 SECOND "
      "WHERE status = 'queued'");
    st = d->step();
    CHECK(st.failed == 3);  // second: failed for good
    CHECK(
      n("SELECT COUNT(*) FROM derived_job WHERE product_id = " +
        std::to_string(ws) + " AND status = 'failed'") == 3);
    CHECK(
      q("SELECT error FROM derived_job WHERE status = 'failed' LIMIT 1")
        .find("does not build") != std::string::npos);
  }

  // A product edited after its job was queued: the job is superseded by one
  // for the new revision when claimed.
  g_db->query(
    "UPDATE derived_product SET revision = 2, params = '{\"calm_ms\": 1.0}', "
    "rebuild_requested_at = UTC_TIMESTAMP() WHERE id = " +
    std::to_string(dir));
  st = d->step();
  CHECK(
    n("SELECT COUNT(*) FROM derived_job WHERE product_id = " +
      std::to_string(dir) + " AND product_revision = 2 AND status = 'done'") ==
    3);

  // Stale running jobs go back to the queue.
  g_db->query(
    "UPDATE derived_job SET status = 'running', started_at = UTC_TIMESTAMP() - "
    "INTERVAL 2 HOUR WHERE id = (SELECT id FROM (SELECT MIN(id) AS id FROM "
    "derived_job WHERE status = 'done') x)");
  st = d->step(0);
  CHECK(st.stale == 1);
}

TEST(output_names)
{
  CHECK(
    product_output_name("ws10", 3 * 3600, 42, 0x0123456789abcdefULL) ==
    "ws10.f003.j42.01234567.grib2");
  CHECK(
    product_output_name("a.b", 90 * 60, 7, 0) ==
    "a.b.f00130.j7.00000000.grib2");
}

int main()
{
  const char* cfg = std::getenv("ATMO_TEST_CATALOG");
  if (!cfg)
  {
    std::fprintf(
      stderr,
      "skipped: ATMO_TEST_CATALOG not set (tests/catalog_testdb.sh start)\n");
    return 77;
  }
  try
  {
    g_cfg = CatalogConfig::from_file(cfg);
    g_db = std::make_unique<Catalog>(g_cfg);
    for (const char* t :
         {"derived_job",
          "derived_product_input",
          "derived_product_output",
          "derived_product",
          "derived_kernel_revision",
          "derived_kernel",
          "derived_processor",
          "grib_message_replaced",
          "grib_message",
          "grib_file",
          "grib_run",
          "grib_common_name",
          "grib_model_grid",
          "grib_model_alias",
          "grib_model",
          "grib_grid",
          "grib_signature",
          "grib_parameter",
          "grib_subcenter",
          "grib_center"})
    {
      g_db->query(std::string("DELETE FROM ") + t);
    }
    g_db->query("UPDATE derived_state SET cursor_seq = 0, heartbeat_at = NULL");
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "skipped: %s\n", e.what());
    return 77;
  }
#ifdef ATMO_HAVE_OPENCL
  try
  {
    make_opencl_executor({}, 5);
    g_have_cl = true;
  }
  catch (const std::exception&)
  {
  }
#endif
  char dir[] = "/tmp/atmo_productd_XXXXXX";
  if (!::mkdtemp(dir))
  {
    return 1;
  }
  g_root = fs::path(dir) / "data";
  fs::create_directories(g_root);
  const int rc = tst::run_all();
  fs::remove_all(dir);
  return rc;
}
