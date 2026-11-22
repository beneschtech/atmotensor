#define CL_TARGET_OPENCL_VERSION 120
#include "atmo/opencl.h"

#include <CL/cl.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace atmo
{

namespace
{

std::string lower(std::string s)
{
  for (auto& c : s)
  {
    if (c >= 'A' && c <= 'Z')
    {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

std::string trim(std::string s)
{
  while (!s.empty() &&
         (s.back() == '\0' || s.back() == ' ' || s.back() == '\n'))
  {
    s.pop_back();
  }
  return s;
}

std::string cl_err(cl_int e)
{
  switch (e)
  {
    case CL_MEM_OBJECT_ALLOCATION_FAILURE:
      return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
    case CL_OUT_OF_RESOURCES:
      return "CL_OUT_OF_RESOURCES";
    case CL_OUT_OF_HOST_MEMORY:
      return "CL_OUT_OF_HOST_MEMORY";
    case CL_INVALID_KERNEL_ARGS:
      return "CL_INVALID_KERNEL_ARGS";
    case CL_INVALID_ARG_SIZE:
      return "CL_INVALID_ARG_SIZE";
    case CL_INVALID_WORK_GROUP_SIZE:
      return "CL_INVALID_WORK_GROUP_SIZE";
    case CL_BUILD_PROGRAM_FAILURE:
      return "CL_BUILD_PROGRAM_FAILURE";
    case CL_INVALID_KERNEL_NAME:
      return "CL_INVALID_KERNEL_NAME";
    default:
      return "OpenCL error " + std::to_string(e);
  }
}

std::string device_string(cl_device_id d, cl_device_info what)
{
  std::size_t n = 0;
  clGetDeviceInfo(d, what, 0, nullptr, &n);
  std::string s(n, '\0');
  clGetDeviceInfo(d, what, n, s.data(), nullptr);
  return trim(s);
}

std::string platform_string(cl_platform_id p, cl_platform_info what)
{
  std::size_t n = 0;
  clGetPlatformInfo(p, what, 0, nullptr, &n);
  std::string s(n, '\0');
  clGetPlatformInfo(p, what, n, s.data(), nullptr);
  return trim(s);
}

struct Arg
{
  std::string what;  // for messages
  bool global = false;
  bool is_const = false;
  std::string type;  // "float*", "float", "int"
};

std::vector<Arg> expected_args(const ProductInterface& f)
{
  std::vector<Arg> a;
  for (const auto& i : f.inputs)
  {
    a.push_back({"input " + i.role, true, true, "float*"});
  }
  for (const auto& o : f.outputs)
  {
    a.push_back({"output " + o.role, true, false, "float*"});
  }
  a.push_back({"ni", false, false, "int"});
  a.push_back({"nj", false, false, "int"});
  if (f.needs_alpha())
  {
    a.push_back({"alpha", true, true, "float*"});
  }
  for (const auto& p : f.params)
  {
    a.push_back({"param " + p.name, false, false, p.is_int ? "int" : "float"});
  }
  return a;
}

}  // namespace

std::string opencl_signature(
  const std::string& entry,
  const ProductInterface& f)
{
  std::string s = "__kernel void " + entry + "(";
  bool first = true;
  auto add = [&](const std::string& a)
  {
    s += (first ? "" : ", ") + a;
    first = false;
  };
  for (const auto& i : f.inputs)
  {
    add("__global const float* " + i.role);
  }
  for (const auto& o : f.outputs)
  {
    add("__global float* " + o.role);
  }
  add("const int ni");
  add("const int nj");
  if (f.needs_alpha())
  {
    add("__global const float* alpha");
  }
  for (const auto& p : f.params)
  {
    add(std::string("const ") + (p.is_int ? "int " : "float ") + p.name);
  }
  return s + ")";
}

struct OpenClExecutor::Impl
{
  struct Device
  {
    std::string name;
    cl_device_id id = nullptr;
    cl_context ctx = nullptr;
    cl_command_queue queue = nullptr;
    cl_ulong max_alloc = 0;
    cl_ulong global_mem = 0;
    bool disabled = false;
    std::string why_disabled;
  };
  std::vector<Device> devs;
  int timeout_s = 600;
  // (device index, source hash, entry) -> program (nullptr: failed there)
  std::map<std::tuple<std::size_t, std::string, std::string>, cl_program> progs;
  std::map<std::tuple<std::size_t, std::string, std::string>, std::string> logs;
  std::mutex mu;

  ~Impl()
  {
    for (auto& [k, p] : progs)
    {
      if (p)
      {
        clReleaseProgram(p);
      }
    }
    for (auto& d : devs)
    {
      if (d.queue)
      {
        clReleaseCommandQueue(d.queue);
      }
      if (d.ctx)
      {
        clReleaseContext(d.ctx);
      }
    }
  }

  // Builds (or finds) the program on one device; nullptr + log on failure.
  cl_program program(std::size_t di, const ProductKernel& k, std::string& log)
  {
    const auto key = std::make_tuple(
      di, k.source_hash + ":" + std::to_string(k.source.size()), k.entry);
    auto it = progs.find(key);
    if (it != progs.end())
    {
      log = logs[key];
      return it->second;
    }
    Device& d = devs[di];
    cl_int err = CL_SUCCESS;
    const char* src = k.source.c_str();
    const std::size_t len = k.source.size();
    cl_program p = clCreateProgramWithSource(d.ctx, 1, &src, &len, &err);
    if (!p)
    {
      log = "cannot create the program: " + cl_err(err);
      progs[key] = nullptr;
      logs[key] = log;
      return nullptr;
    }
    err = clBuildProgram(p, 1, &d.id, "-cl-kernel-arg-info", nullptr, nullptr);
    std::size_t n = 0;
    clGetProgramBuildInfo(p, d.id, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n);
    std::string blog(n, '\0');
    clGetProgramBuildInfo(
      p, d.id, CL_PROGRAM_BUILD_LOG, n, blog.data(), nullptr);
    log = trim(blog);
    if (err != CL_SUCCESS)
    {
      clReleaseProgram(p);
      p = nullptr;
    }
    else if (std::string why = check_args(p, k); !why.empty())
    {
      log = (log.empty() ? "" : log + "\n") + why;
      clReleaseProgram(p);
      p = nullptr;
    }
    progs[key] = p;
    logs[key] = log;
    return p;
  }

  // The kernel's argument list against the interface.  "" if it fits.
  static std::string check_args(cl_program p, const ProductKernel& k)
  {
    cl_int err = CL_SUCCESS;
    cl_kernel kern = clCreateKernel(p, k.entry.c_str(), &err);
    if (!kern)
    {
      return "no __kernel function \"" + k.entry + "\" (" + cl_err(err) + ")";
    }
    const auto want = expected_args(k.iface);
    cl_uint nargs = 0;
    clGetKernelInfo(kern, CL_KERNEL_NUM_ARGS, sizeof nargs, &nargs, nullptr);
    std::string why;
    if (nargs != want.size())
    {
      why = "the kernel has " + std::to_string(nargs) +
            " arguments, the interface needs " + std::to_string(want.size());
    }
    else
    {
      for (cl_uint i = 0; i < nargs && why.empty(); ++i)
      {
        cl_kernel_arg_address_qualifier aq = 0;
        cl_kernel_arg_type_qualifier tq = 0;
        std::size_t n = 0;
        if (
          clGetKernelArgInfo(
            kern, i, CL_KERNEL_ARG_ADDRESS_QUALIFIER, sizeof aq, &aq,
            nullptr) != CL_SUCCESS)
        {
          break;  // no arg info on this platform: count check only
        }
        clGetKernelArgInfo(
          kern, i, CL_KERNEL_ARG_TYPE_QUALIFIER, sizeof tq, &tq, nullptr);
        clGetKernelArgInfo(kern, i, CL_KERNEL_ARG_TYPE_NAME, 0, nullptr, &n);
        std::string type(n, '\0');
        clGetKernelArgInfo(
          kern, i, CL_KERNEL_ARG_TYPE_NAME, n, type.data(), nullptr);
        type = trim(type);
        const Arg& w = want[i];
        const bool global = aq == CL_KERNEL_ARG_ADDRESS_GLOBAL;
        const bool is_const = (tq & CL_KERNEL_ARG_TYPE_CONST) != 0;
        if (
          global != w.global || type != w.type ||
          (w.global && is_const != w.is_const))
        {
          why =
            "argument " + std::to_string(i + 1) + " (" + w.what +
            ") should be " +
            (w.global ? std::string("__global ") + (w.is_const ? "const " : "")
                      : std::string("const ")) +
            w.type + ", the kernel has " + (global ? "__global " : "") +
            (is_const && global ? "const " : "") + type;
        }
      }
    }
    clReleaseKernel(kern);
    if (!why.empty())
    {
      why += "\nrequired: " + opencl_signature(k.entry, k.iface);
    }
    return why;
  }

  // One run on one device; "" on success, else why it did not run there.
  std::string run_on(
    std::size_t di,
    const ProductKernel& k,
    const ProcessorArgs& a,
    cl_program prog)
  {
    Device& d = devs[di];
    const std::size_t n = a.cells();
    const cl_ulong bytes = static_cast<cl_ulong>(n) * sizeof(float);
    const std::size_t nbuf =
      a.inputs.size() + a.outputs.size() + (a.alpha ? 1 : 0);
    if (bytes > d.max_alloc || bytes * nbuf > d.global_mem)
    {
      return "does not fit (" + std::to_string(nbuf) + " buffers of " +
             std::to_string(bytes >> 20) + " MiB)";
    }
    cl_int err = CL_SUCCESS;
    cl_kernel kern = clCreateKernel(prog, k.entry.c_str(), &err);
    if (!kern)
    {
      return cl_err(err);
    }
    std::vector<cl_mem> bufs;
    auto release = [&]()
    {
      for (auto b : bufs)
      {
        clReleaseMemObject(b);
      }
      clReleaseKernel(kern);
    };
    cl_uint arg = 0;
    auto buffer = [&](const float* host, bool out) -> bool
    {
      cl_mem m = clCreateBuffer(
        d.ctx,
        out ? CL_MEM_WRITE_ONLY : (CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR),
        bytes, out ? nullptr : const_cast<float*>(host), &err);
      if (!m)
      {
        return false;
      }
      bufs.push_back(m);
      err = clSetKernelArg(kern, arg++, sizeof(cl_mem), &m);
      return err == CL_SUCCESS;
    };
    bool ok = true;
    for (const float* in : a.inputs)
    {
      ok = ok && buffer(in, false);
    }
    const std::size_t first_out = bufs.size();
    for (float* out : a.outputs)
    {
      ok = ok && buffer(out, true);
    }
    const cl_int ni = static_cast<cl_int>(a.ni), nj = static_cast<cl_int>(a.nj);
    ok = ok && clSetKernelArg(kern, arg++, sizeof ni, &ni) == CL_SUCCESS;
    ok = ok && clSetKernelArg(kern, arg++, sizeof nj, &nj) == CL_SUCCESS;
    if (ok && k.iface.needs_alpha())
    {
      ok = a.alpha && buffer(a.alpha, false);
    }
    for (std::size_t i = 0; ok && i < k.iface.params.size(); ++i)
    {
      const double v = i < a.params.size() ? a.params[i] : 0.0;
      if (k.iface.params[i].is_int)
      {
        const cl_int x = static_cast<cl_int>(v);
        ok = clSetKernelArg(kern, arg++, sizeof x, &x) == CL_SUCCESS;
      }
      else
      {
        const cl_float x = static_cast<cl_float>(v);
        ok = clSetKernelArg(kern, arg++, sizeof x, &x) == CL_SUCCESS;
      }
    }
    if (!ok)
    {
      release();
      return "setting up arguments: " + cl_err(err);
    }
    const std::size_t global = n;
    cl_event ev = nullptr;
    err = clEnqueueNDRangeKernel(
      d.queue, kern, 1, nullptr, &global, nullptr, 0, nullptr, &ev);
    if (err != CL_SUCCESS)
    {
      release();
      return "enqueue: " + cl_err(err);
    }
    clFlush(d.queue);
    const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    cl_int status = CL_QUEUED;
    while (true)
    {
      clGetEventInfo(
        ev, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof status, &status, nullptr);
      if (status == CL_COMPLETE || status < 0)
      {
        break;
      }
      if (std::chrono::steady_clock::now() > deadline)
      {
        d.disabled = true;
        d.why_disabled =
          "a kernel ran past " + std::to_string(timeout_s) + " s";
        // Not released: the device may still be using them.
        return "timeout after " + std::to_string(timeout_s) +
               " s (device disabled until restart)";
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    clReleaseEvent(ev);
    if (status < 0)
    {
      release();
      return "kernel failed: " + cl_err(status);
    }
    for (std::size_t o = 0; o < a.outputs.size(); ++o)
    {
      err = clEnqueueReadBuffer(
        d.queue, bufs[first_out + o], CL_TRUE, 0, bytes, a.outputs[o], 0,
        nullptr, nullptr);
      if (err != CL_SUCCESS)
      {
        release();
        return "reading results: " + cl_err(err);
      }
    }
    release();
    return "";
  }
};

OpenClExecutor::OpenClExecutor(
  std::vector<std::string> preference,
  int timeout_s)
    : impl_(std::make_unique<Impl>())
{
  impl_->timeout_s = timeout_s > 0 ? timeout_s : 600;
  cl_uint np = 0;
  if (clGetPlatformIDs(0, nullptr, &np) != CL_SUCCESS || np == 0)
  {
    throw std::runtime_error(
      "OpenCL: no platform found (is an ICD installed?)");
  }
  std::vector<cl_platform_id> plats(np);
  clGetPlatformIDs(np, plats.data(), nullptr);
  struct Found
  {
    std::string name;
    cl_device_id id;
    bool gpu;
  };
  std::vector<Found> found;
  for (auto p : plats)
  {
    cl_uint nd = 0;
    if (
      clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 0, nullptr, &nd) != CL_SUCCESS ||
      nd == 0)
    {
      continue;
    }
    std::vector<cl_device_id> ids(nd);
    clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, nd, ids.data(), nullptr);
    for (auto d : ids)
    {
      cl_device_type t = 0;
      clGetDeviceInfo(d, CL_DEVICE_TYPE, sizeof t, &t, nullptr);
      found.push_back(
        {platform_string(p, CL_PLATFORM_NAME) + " / " +
           device_string(d, CL_DEVICE_NAME),
         d, (t & CL_DEVICE_TYPE_GPU) != 0});
    }
  }
  std::vector<Found> order;
  if (preference.empty())
  {
    std::stable_sort(
      found.begin(), found.end(),
      [](const Found& a, const Found& b) { return a.gpu > b.gpu; });
    order = found;
  }
  else
  {
    for (const auto& want : preference)
    {
      for (const auto& f : found)
      {
        const bool taken = std::any_of(
          order.begin(), order.end(),
          [&](const Found& o) { return o.id == f.id; });
        if (!taken && lower(f.name).find(lower(want)) != std::string::npos)
        {
          order.push_back(f);
        }
      }
    }
  }
  for (const auto& f : order)
  {
    Impl::Device d;
    d.name = f.name;
    d.id = f.id;
    cl_int err = CL_SUCCESS;
    d.ctx = clCreateContext(nullptr, 1, &d.id, nullptr, nullptr, &err);
    if (!d.ctx)
    {
      continue;
    }
    d.queue = clCreateCommandQueue(d.ctx, d.id, 0, &err);
    if (!d.queue)
    {
      clReleaseContext(d.ctx);
      continue;
    }
    clGetDeviceInfo(
      d.id, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof d.max_alloc, &d.max_alloc,
      nullptr);
    clGetDeviceInfo(
      d.id, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof d.global_mem, &d.global_mem,
      nullptr);
    impl_->devs.push_back(d);
  }
  if (impl_->devs.empty())
  {
    std::string names;
    for (const auto& f : found)
    {
      names += (names.empty() ? "" : ", ") + f.name;
    }
    throw std::runtime_error(
      "OpenCL: no configured device is usable (found: " +
      (names.empty() ? std::string("none") : names) + ")");
  }
}

OpenClExecutor::~OpenClExecutor() = default;

std::vector<std::string> OpenClExecutor::devices() const
{
  std::vector<std::string> out;
  for (const auto& d : impl_->devs)
  {
    out.push_back(d.name);
  }
  return out;
}

KernelBuild OpenClExecutor::build(const ProductKernel& k)
{
  std::lock_guard<std::mutex> lk(impl_->mu);
  KernelBuild b;
  std::string all;
  for (std::size_t i = 0; i < impl_->devs.size(); ++i)
  {
    std::string log;
    const bool ok = impl_->program(i, k, log) != nullptr;
    b.ok = b.ok || ok;
    if (!log.empty() || !ok)
    {
      all += "[" + impl_->devs[i].name + "] " + (ok ? "built" : "FAILED") +
             (log.empty() ? "" : ":\n" + log) + "\n";
    }
  }
  b.log = trim(all);
  return b;
}

std::string OpenClExecutor::run(const ProductKernel& k, const ProcessorArgs& a)
{
  std::lock_guard<std::mutex> lk(impl_->mu);
  if (
    a.inputs.size() != k.iface.inputs.size() ||
    a.outputs.size() != k.iface.outputs.size())
  {
    throw std::runtime_error(
      "kernel " + k.name + ": arguments do not match its interface");
  }
  std::string why;
  for (std::size_t i = 0; i < impl_->devs.size(); ++i)
  {
    auto& d = impl_->devs[i];
    if (d.disabled)
    {
      why += "\n" + d.name + ": disabled (" + d.why_disabled + ")";
      continue;
    }
    std::string log;
    cl_program p = impl_->program(i, k, log);
    if (!p)
    {
      why +=
        "\n" + d.name + ": does not build" + (log.empty() ? "" : ": " + log);
      continue;
    }
    const std::string r = impl_->run_on(i, k, a, p);
    if (r.empty())
    {
      return d.name;
    }
    why += "\n" + d.name + ": " + r;
  }
  throw std::runtime_error(
    "kernel " + k.name + " could not run on any device:" + why);
}

std::unique_ptr<OpenClExecutor> make_opencl_executor(
  std::vector<std::string> preference,
  int timeout_s)
{
  return std::make_unique<OpenClExecutor>(std::move(preference), timeout_s);
}

}  // namespace atmo
