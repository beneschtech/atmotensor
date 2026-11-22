// OpenCL kernels for derived products (06-derived-products.md "Processors";
// T22).  Only built with an OpenCL ICD loader (ATMO_WITH_OPENCL; defines
// ATMO_HAVE_OPENCL); this header does not include CL/cl.h.
//
// Kernel ABI (one work item per grid cell, index k = j*ni + i, NaN =
// missing in and out):
//   __kernel void <entry>(__global const float* <inputs...>,
//                         __global float* <outputs...>,
//                         const int ni, const int nj,
//                         [__global const float* alpha,]  // if any input
//                                                         // needs_alpha
//                         const float|int <params...>)
// build() compiles for every configured device (with -cl-kernel-arg-info)
// and checks the argument list against the declared interface: count,
// address space and type of every argument, so a kernel whose signature
// does not match fails at build time with the expected signature in the
// log, not at run time.  Programs are cached per (device, source hash,
// entry).
//
// run() tries the devices in preference order (Q2b): a device is skipped
// when the buffers do not fit (max allocation or global memory) or the
// enqueue fails for lack of resources, and the next one is tried.  A
// kernel still running after the timeout disables that device until the
// process restarts (OpenCL cannot cancel a running kernel) and fails the
// job; systemd restarting atmo_productd brings the device back.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "atmo/products.h"

namespace atmo
{

class OpenClExecutor : public KernelExecutor
{
 public:
  // `preference`: case-insensitive substrings of "<platform> / <device>",
  // in order; devices matching none are not used.  Empty: every device
  // found (GPUs first).  std::runtime_error if no device is usable.
  OpenClExecutor(std::vector<std::string> preference, int timeout_s);
  ~OpenClExecutor() override;

  // "<platform> / <device>" of the devices in use, in order.
  std::vector<std::string> devices() const;

  KernelBuild build(const ProductKernel& k) override;
  std::string run(const ProductKernel& k, const ProcessorArgs& args) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<OpenClExecutor> make_opencl_executor(
  std::vector<std::string> preference,
  int timeout_s);

// The C argument list the interface requires (for build logs and the web
// editor's "required signature").
std::string opencl_signature(
  const std::string& entry,
  const ProductInterface& iface);

}  // namespace atmo
