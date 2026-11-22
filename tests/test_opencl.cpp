// OpenCL kernels for derived products (T22): build checks against the
// declared interface, the seed wind_speed kernel against the built-in
// processor, params and alpha in ABI order, device selection.  Exits 77
// (skipped) when no OpenCL device is available.
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "atmo/opencl.h"
#include "atmo/products.h"
#include "check.h"

using namespace atmo;

namespace
{

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// The web app's seed kernel (resources/kernels/wind_speed.cl), verbatim.
const char* kWindSpeed =
  R"(// Wind speed from u/v components (06-derived-products.md, seed kernel).
// One work item per grid cell; NaN in either component gives NaN.
// Speed does not depend on grid- vs earth-relative components, so no alpha.
__kernel void wind_speed(__global const float* u,
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

OpenClExecutor* ex = nullptr;

ProductKernel kernel(
  std::string name,
  std::string source,
  std::string entry,
  ProductInterface iface)
{
  ProductKernel k;
  k.id = 1;
  k.name = std::move(name);
  k.source = std::move(source);
  k.entry = std::move(entry);
  k.iface = std::move(iface);
  k.source_hash = std::to_string(std::hash<std::string>{}(k.source));
  return k;
}

ProductInterface uv_speed()
{
  ProductInterface f;
  f.inputs = {{"u", "m/s", false}, {"v", "m/s", false}};
  f.outputs = {{"speed", "m/s", false}};
  return f;
}

}  // namespace

TEST(seed_wind_speed_kernel_matches_the_builtin)
{
  const auto k = kernel("wind_speed", kWindSpeed, "wind_speed", uv_speed());
  const KernelBuild b = ex->build(k);
  CHECK(b.ok);
  // 1000 x 700 cells, some missing.
  const std::uint32_t ni = 1000, nj = 700;
  std::vector<float> u(ni * nj), v(ni * nj), gpu(ni * nj), cpu(ni * nj);
  for (std::size_t i = 0; i < u.size(); ++i)
  {
    u[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.001) * 30.0);
    v[i] = static_cast<float>(std::cos(static_cast<double>(i) * 0.0007) * 25.0);
  }
  u[17] = kNaN;
  v[99] = kNaN;
  ProcessorArgs a;
  a.ni = ni;
  a.nj = nj;
  a.inputs = {u.data(), v.data()};
  a.outputs = {gpu.data()};
  const std::string dev = ex->run(k, a);
  CHECK(!dev.empty());
  a.outputs = {cpu.data()};
  find_builtin("wind_speed")->fn(a);
  double worst = 0.0;
  bool nan_ok = std::isnan(gpu[17]) && std::isnan(gpu[99]);
  for (std::size_t i = 0; i < gpu.size(); ++i)
  {
    if (std::isnan(cpu[i]))
    {
      nan_ok = nan_ok && std::isnan(gpu[i]);
      continue;
    }
    worst = std::max(
      worst,
      static_cast<double>(std::fabs(gpu[i] - cpu[i]) / std::max(1.0f, cpu[i])));
  }
  CHECK(nan_ok);
  CHECK(worst < 1e-6);
  std::printf(
    "  wind_speed on %s, worst relative difference %.2g\n", dev.c_str(), worst);
}

TEST(build_errors_name_the_problem)
{
  // A syntax error: the compiler's log.
  auto bad = kernel(
    "bad", "__kernel void f(__global const float* u { }", "f", uv_speed());
  KernelBuild b = ex->build(bad);
  CHECK(!b.ok && b.log.find("FAILED") != std::string::npos);

  // Wrong entry name.
  auto entry = kernel("e", kWindSpeed, "windspeed", uv_speed());
  b = ex->build(entry);
  CHECK(
    !b.ok &&
    b.log.find("no __kernel function \"windspeed\"") != std::string::npos);

  // nj missing: argument count, with the signature it should have.
  std::string src = kWindSpeed;
  src.replace(
    src.find(",\n                         const int nj"),
    std::string(",\n                         const int nj").size(), "");
  src.replace(
    src.find("k >= ni * nj"), std::string("k >= ni * nj").size(), "k >= ni");
  b = ex->build(kernel("short", src, "wind_speed", uv_speed()));
  CHECK(
    !b.ok &&
    b.log.find("has 4 arguments, the interface needs 5") != std::string::npos);
  CHECK(
    b.log.find("required: __kernel void wind_speed(__global const float* u") !=
    std::string::npos);

  // A param declared int but written float.
  ProductInterface f = uv_speed();
  ParamDecl n;
  n.name = "n";
  n.is_int = true;
  f.params = {n};
  const char* wrong =
    R"(__kernel void g(__global const float* u, __global const float* v,
    __global float* speed, const int ni, const int nj, const float n)
  { const int k = get_global_id(0); if (k < ni * nj) speed[k] = u[k] * n; })";
  b = ex->build(kernel("wrongtype", wrong, "g", f));
  CHECK(
    !b.ok && b.log.find("argument 6 (param n) should be const int") !=
               std::string::npos);

  // An output declared as input-only (const) is caught too.
  const char* constout =
    R"(__kernel void h(__global const float* u, __global const float* v,
    __global const float* speed, const int ni, const int nj) { })";
  b = ex->build(kernel("constout", constout, "h", uv_speed()));
  CHECK(!b.ok && b.log.find("argument 3 (output speed)") != std::string::npos);

  // run() of a kernel that does not build: a clear error, no crash.
  ProcessorArgs a;
  a.ni = a.nj = 1;
  float x = 1, y = 1, o = 0;
  a.inputs = {&x, &y};
  a.outputs = {&o};
  bool threw = false;
  try
  {
    ex->run(bad, a);
  }
  catch (const std::runtime_error& e)
  {
    threw = std::string(e.what()).find("does not build") != std::string::npos;
  }
  CHECK(threw);
}

TEST(params_and_alpha_follow_the_abi)
{
  ProductInterface f;
  f.inputs = {{"a", "", true}};
  f.outputs = {{"out", "", false}};
  ParamDecl scale;
  scale.name = "scale";
  ParamDecl add;
  add.name = "add";
  add.is_int = true;
  f.params = {scale, add};
  const char* src =
    R"(__kernel void lin(__global const float* a, __global float* out,
      const int ni, const int nj, __global const float* alpha,
      const float scale, const int add)
  {
    const int k = get_global_id(0);
    if (k >= ni * nj) return;
    out[k] = a[k] * scale + (float)add + alpha[k];
  })";
  const auto k = kernel("lin", src, "lin", f);
  CHECK(ex->build(k).ok);
  CHECK(
    opencl_signature("lin", f) ==
    "__kernel void lin(__global const float* a, __global float* out, const int "
    "ni, const int nj, "
    "__global const float* alpha, const float scale, const int add)");
  std::vector<float> in = {1, 2, 3, 4, 5, 6}, alpha = {0, 0.5f, 0, 0, 0, 1},
                     out(6);
  ProcessorArgs a;
  a.ni = 3;
  a.nj = 2;
  a.inputs = {in.data()};
  a.outputs = {out.data()};
  a.alpha = alpha.data();
  a.params = param_values(f, {{"scale", 2.0}, {"add", 3.0}});
  ex->run(k, a);
  CHECK(out[0] == 5.0f && out[1] == 7.5f && out[5] == 16.0f);
}

TEST(device_preference)
{
  const auto devs = ex->devices();
  CHECK(!devs.empty());
  // A preference matching nothing leaves no device.
  bool threw = false;
  try
  {
    OpenClExecutor none({"no such device anywhere"}, 5);
  }
  catch (const std::runtime_error& e)
  {
    threw =
      std::string(e.what()).find("no configured device") != std::string::npos;
  }
  CHECK(threw);
  // The first device by name (case-insensitive substring).
  std::string part = devs[0].substr(devs[0].find(" / ") + 3, 6);
  for (auto& c : part)
  {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  OpenClExecutor one({part}, 5);
  CHECK(!one.devices().empty() && one.devices()[0] == devs[0]);
}

int main()
{
  try
  {
    static OpenClExecutor e({}, 30);
    ex = &e;
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "skipped: %s\n", e.what());
    return 77;
  }
  for (const auto& d : ex->devices())
  {
    std::printf("OpenCL device: %s\n", d.c_str());
  }
  return tst::run_all();
}
