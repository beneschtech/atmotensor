#include "atmo/productd.h"

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

#include "atmo/grib.h"
#include "atmo/grib_api.h"
#include "atmo/grid.h"

namespace atmo
{

namespace fs = std::filesystem;

namespace
{

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

std::vector<unsigned char>
read_message(const fs::path& file, std::uint64_t offset, std::uint64_t length)
{
  const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    throw std::runtime_error("cannot open " + file.string());
  }
  std::vector<unsigned char> buf(length);
  std::size_t got = 0;
  while (got < buf.size())
  {
    const ssize_t n = ::pread(
      fd, buf.data() + got, buf.size() - got, static_cast<off_t>(offset + got));
    if (n <= 0)
    {
      break;
    }
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  if (got != buf.size())
  {
    throw std::runtime_error("short read from " + file.string());
  }
  return buf;
}

// Directory name under grib/ for a product's outputs (catalog_grib wants
// [a-z][a-z0-9_-]*; slugs may hold '.' and start with a digit).
std::string output_model(std::uint32_t process_id)
{
  return "derived-" + std::to_string(process_id);
}

}  // namespace

std::string product_output_name(
  const std::string& slug,
  std::int64_t offset_end_s,
  std::uint64_t job_id,
  std::uint64_t content_hash)
{
  char off[32];
  const std::int64_t h = offset_end_s / 3600;
  const std::int64_t m = (offset_end_s % 3600) / 60;
  if (m == 0)
  {
    std::snprintf(off, sizeof off, "f%03lld", static_cast<long long>(h));
  }
  else
  {
    std::snprintf(
      off, sizeof off, "f%03lld%02lld", static_cast<long long>(h),
      static_cast<long long>(m));
  }
  char hash[16];
  std::snprintf(
    hash, sizeof hash, "%08llx",
    static_cast<unsigned long long>(content_hash >> 32));
  return slug + "." + off + ".j" + std::to_string(job_id) + "." + hash +
         ".grib2";
}

struct ProductDaemon::Impl
{
  ProductDaemonOptions opt;
  std::unique_ptr<KernelExecutor> kernels;
  std::unique_ptr<Catalog> writer;
  std::unique_ptr<ProductsDb> db;
  std::unique_ptr<CatalogReader> reader;
  std::unique_ptr<GribCache> cache;
  std::unique_ptr<GribApi> api;
  std::set<std::uint64_t> named;  // products whose outputs got their names
  bool registered = false;

  void connect()
  {
    writer = std::make_unique<Catalog>(opt.catalog);
    db = std::make_unique<ProductsDb>(*writer);
    reader = std::make_unique<CatalogReader>(
      opt.catalog, std::chrono::milliseconds(0));
    GribCacheLimits lim;
    lim.max_bytes = opt.cache_mb << 20;
    cache = std::make_unique<GribCache>(lim, grib_decoder());
    api =
      std::make_unique<GribApi>(opt.root, *reader, cache.get(), grib_values);
  }

  ProductMatcher::Versions versions(
    const std::vector<Product>& ps,
    const std::map<std::uint64_t, ProductKernel>& ks)
  {
    ProductMatcher::Versions v;
    for (const auto& p : ps)
    {
      int proc = 0, krev = 0;
      if (p.kind == ProcessorKind::builtin)
      {
        if (const auto* b = find_builtin(p.processor))
        {
          proc = b->version;
        }
      }
      else if (p.kernel_id)
      {
        auto it = ks.find(*p.kernel_id);
        if (it != ks.end())
        {
          krev = it->second.revision;
        }
      }
      v[p.id] = {proc, krev};
    }
    return v;
  }

  // Runs one claimed job.  Returns the device used.
  std::string execute(
    const DerivedJob& job,
    const Product& p,
    const ProductInterface& iface,
    const ProductKernel* kernel,
    const CatalogTables& t,
    ProductPassStats& st)
  {
    // Inputs, in the interface's order.
    const CatRun* run = t.run(job.run_id);
    if (!run)
    {
      throw std::runtime_error(
        "source run " + std::to_string(job.run_id) + " is gone");
    }
    std::vector<std::vector<float>> in(iface.inputs.size());
    std::shared_ptr<const Grid> grid;
    std::optional<CatMessage> tmpl;
    bool grid_relative = false;
    for (std::size_t k = 0; k < iface.inputs.size(); ++k)
    {
      const std::string& role = iface.inputs[k].role;
      auto it = job.inputs.find(role);
      if (it == job.inputs.end())
      {
        continue;  // optional or chain start: NaN (filled below)
      }
      std::optional<CatMessage> msg = reader->message(t, it->second);
      if (!msg)
      {
        if (auto n = reader->replaced_by(t, it->second))
        {
          msg = reader->message(t, *n);
        }
      }
      if (!msg)
      {
        throw std::runtime_error(
          "input " + role + ": message " + std::to_string(it->second) +
          " is gone (expired?)");
      }
      const CatGrid* g = msg->grid_id ? t.grid(*msg->grid_id) : nullptr;
      const CatSignature* sig = t.signature(msg->signature_id);
      if (!g || !sig)
      {
        throw std::runtime_error("input " + role + ": grid cannot be decoded");
      }
      const auto view = api->decode(*msg, *g, *sig, FieldInfo{});
      if (!view)
      {
        throw std::runtime_error(
          "input " + role + ": cannot decode message " +
          std::to_string(msg->id) + " (grid or " + msg->file_path +
          " missing under " + opt.root.string() + "?)");
      }
      if (grid && grid->key() != view->grid().key())
      {
        throw std::runtime_error(
          "inputs are on different grids (regridding is not supported yet)");
      }
      if (!grid)
      {
        grid = load_grid(g->key);
      }
      if ((view->info().flags & kFieldGridRelative) != 0)
      {
        grid_relative = true;
      }
      const std::uint32_t ni = view->grid().ni(), nj = view->grid().nj();
      in[k].resize(static_cast<std::size_t>(ni) * nj);
      for (std::uint32_t j = 0; j < nj; ++j)
      {
        for (std::uint32_t i = 0; i < ni; ++i)
        {
          in[k][static_cast<std::size_t>(j) * ni + i] =
            static_cast<float>(view->at(i, j));
        }
      }
      // The template: a message of the source run (the first input that is
      // not one of our own outputs keeps the source's reference time).
      if (!tmpl || (msg->run_id == run->id && tmpl->run_id != run->id))
      {
        tmpl = msg;
      }
    }
    if (!grid || !tmpl)
    {
      throw std::runtime_error("no input could be read");
    }
    ProcessorArgs a;
    a.ni = grid->ni();
    a.nj = grid->nj();
    const std::size_t n = a.cells();
    for (auto& v : in)
    {
      if (v.empty())
      {
        v.assign(n, kNaN);
      }
      a.inputs.push_back(v.data());
    }
    std::vector<float> alpha;
    if (iface.needs_alpha())
    {
      alpha.assign(n, 0.0f);
      if (grid_relative && grid->rot_rad())
      {
        alpha.assign(grid->rot_rad(), grid->rot_rad() + n);
      }
      a.alpha = alpha.data();
    }
    std::map<std::string, double> params = p.params;
    for (const auto& d : iface.params)
    {
      if (d.name == "has_prev")
      {
        params["has_prev"] = (job.flags & kJobChainStart) ? 0.0 : 1.0;
      }
    }
    a.params = param_values(iface, params);
    std::vector<std::vector<float>> out(
      iface.outputs.size(), std::vector<float>(n, kNaN));
    for (auto& v : out)
    {
      a.outputs.push_back(v.data());
    }

    std::string device = "cpu";
    if (p.kind == ProcessorKind::builtin)
    {
      const auto* b = find_builtin(p.processor);
      b->fn(a);
    }
    else
    {
      if (!kernels)
      {
        throw std::runtime_error(
          "kernel products need OpenCL; this engine was built without it");
      }
      device = kernels->run(*kernel, a);
    }

    // One GRIB2 file per job, one message per output.
    const fs::path src_file = opt.root / tmpl->file_path;
    const auto tbytes =
      read_message(src_file, tmpl->byte_offset, tmpl->byte_length);
    std::vector<unsigned char> file_bytes;
    for (std::size_t k = 0; k < iface.outputs.size(); ++k)
    {
      const ProductOutput* o = p.output(iface.outputs[k].role);
      if (!o)
      {
        throw std::runtime_error(
          "output " + iface.outputs[k].role + " is not defined");
      }
      GribDeriveSpec spec;
      spec.process_id = p.process_id;
      spec.discipline = o->discipline;
      spec.category = o->category;
      spec.number = o->number;
      spec.level1 = o->level1;
      spec.level2 = o->level2;
      spec.offset_end_s = job.offset_end_s;
      spec.bits = opt.bits;
      const auto bytes =
        grib_derive(tbytes.data(), tbytes.size(), spec, out[k]);
      file_bytes.insert(file_bytes.end(), bytes.begin(), bytes.end());
    }
    const std::string name = product_output_name(
      p.slug, job.offset_end_s, job.id,
      fnv1a64(file_bytes.data(), file_bytes.size()));
    fs::create_directories(opt.tmp);
    const fs::path tmp_file = opt.tmp / (".productd." + name);
    {
      std::ofstream f(tmp_file, std::ios::binary | std::ios::trunc);
      f.write(
        reinterpret_cast<const char*>(file_bytes.data()),
        static_cast<std::streamsize>(file_bytes.size()));
      f.flush();
      if (!f)
      {
        throw std::runtime_error("cannot write " + tmp_file.string());
      }
    }
    // catalog_grib links the file under grib/derived-<process>/<base>/ by
    // its own name; the scratch name must therefore be the final one.
    const fs::path final_tmp = opt.tmp / name;
    fs::rename(tmp_file, final_tmp);
    CatalogFile cf;
    try
    {
      cf = catalog_grib(opt.root, final_tmp, output_model(p.process_id));
      writer->ingest(cf);
    }
    catch (...)
    {
      std::error_code ec;
      fs::remove(final_tmp, ec);
      throw;
    }
    std::error_code ec;
    fs::remove(final_tmp, ec);
    const auto ids = db->output_messages(cf.path, p);
    if (ids.size() != iface.outputs.size())
    {
      throw std::runtime_error(
        "outputs were catalogued but not all were found again");
    }
    if (!named.count(p.id))
    {
      db->name_outputs(p);
      named.insert(p.id);
    }
    db->done(job.id, ids, device);
    st.log.push_back(
      "job " + std::to_string(job.id) + " " + p.slug + " " + name + " on " +
      device);
    return device;
  }

  std::shared_ptr<const Grid> load_grid(GridKey key)
  {
    return api->load_grid(key);
  }

  ProductPassStats step(std::size_t max_jobs)
  {
    ProductPassStats st;
    if (!registered)
    {
      db->register_builtins(builtin_processors());
      registered = true;
    }
    db->heartbeat();
    const auto t = reader->tables();
    st.generation = t->generation;

    std::vector<Product> products = db->products();
    std::map<std::uint64_t, ProductKernel> kernel_by_id;
    for (auto& k : db->kernels())
    {
      kernel_by_id[k.id] = std::move(k);
    }

    // Kernel builds.
    for (auto& [id, k] : kernel_by_id)
    {
      if (k.built_revision && *k.built_revision == k.revision)
      {
        continue;
      }
      KernelBuild b;
      if (kernels)
      {
        b = kernels->build(k);
      }
      else
      {
        b.log = "this engine was built without OpenCL";
      }
      db->set_build(k.id, k.revision, b.ok, b.log);
      k.built_revision = k.revision;
      k.build_status = b.ok ? "ok" : "error";
      ++st.kernels_built;
      st.log.push_back(
        "kernel " + k.name + " revision " + std::to_string(k.revision) +
        (b.ok ? " built" : " failed to build"));
    }

    const auto field_store = api->field_store();
    const auto vers = versions(products, kernel_by_id);
    ProductMatcher matcher(t, *field_store, *reader, products, vers);

    auto add = [&](const std::vector<JobPlan>& plans)
    {
      const JobInsertStats s = db->add_jobs(plans);
      st.jobs_created += s.created;
      st.jobs_requeued += s.requeued;
      st.jobs_superseded += s.superseded;
    };

    // Arrivals.
    std::uint64_t cur = db->cursor();
    if (cur == 0 && t->generation > 0)
    {
      // First start: what is already catalogued is processed through
      // rebuild requests (set by the web app on create), not as arrivals.
      db->set_cursor(t->generation);
      cur = t->generation;
      st.log.push_back(
        "first start: arrivals from generation " + std::to_string(cur));
    }
    while (cur < t->generation)
    {
      std::uint64_t last = cur;
      const auto arrived =
        db->arrivals(cur, t->generation, opt.arrivals_batch, last);
      st.arrivals += arrived.size();
      if (!arrived.empty())
      {
        add(matcher.plan(arrived));
      }
      db->set_cursor(last);
      if (last == cur)
      {
        break;
      }
      cur = last;
    }

    // Rebuild requests.
    for (const auto& [pid, seen] : db->rebuild_requests())
    {
      add(matcher.plan_all(pid));
      db->clear_rebuild(pid, seen);
      ++st.rebuilds;
    }

    st.stale = db->requeue_stale(opt.job_timeout_s);

    // Jobs.
    std::map<std::uint64_t, const Product*> by_id;
    for (const auto& p : products)
    {
      by_id[p.id] = &p;
    }
    for (std::size_t k = 0; k < max_jobs; ++k)
    {
      auto job = db->claim(opt.worker);
      if (!job)
      {
        break;
      }
      auto pit = by_id.find(job->product_id);
      const Product* p = pit == by_id.end() ? nullptr : pit->second;
      const auto v = p ? vers.at(p->id) : std::make_pair(0, 0);
      if (
        !p || job->product_revision != p->revision ||
        job->processor_version != v.first || job->kernel_revision != v.second)
      {
        // Edited since the job was made: replace it with the current one.
        db->superseded(job->id);
        if (p)
        {
          if (auto plan = matcher.ready(p->id, job->run_id, job->offset_end_s))
          {
            add({*plan});
          }
        }
        continue;
      }
      try
      {
        ProductInterface iface;
        const ProductKernel* kernel = nullptr;
        if (p->kind == ProcessorKind::builtin)
        {
          const auto* b = find_builtin(p->processor);
          if (!b)
          {
            throw std::runtime_error(
              "no built-in processor \"" + p->processor + "\"");
          }
          iface = b->iface;
        }
        else
        {
          auto kit = p->kernel_id ? kernel_by_id.find(*p->kernel_id)
                                  : kernel_by_id.end();
          if (kit == kernel_by_id.end())
          {
            throw std::runtime_error("the product's kernel is gone");
          }
          kernel = &kit->second;
          if (kernel->build_status != "ok")
          {
            throw std::runtime_error(
              "kernel " + kernel->name + " revision " +
              std::to_string(kernel->revision) + " does not build");
          }
          iface = kernel->iface;
        }
        execute(*job, *p, iface, kernel, *t, st);
        ++st.ran;
      }
      catch (const std::exception& e)
      {
        db->failed(
          job->id, e.what(), job->attempts, opt.max_attempts,
          opt.retry_delay_s);
        ++st.failed;
        st.log.push_back(
          "job " + std::to_string(job->id) + " failed: " + e.what());
      }
    }
    return st;
  }
};

ProductDaemon::ProductDaemon(
  ProductDaemonOptions opt,
  std::unique_ptr<KernelExecutor> kernels)
    : impl_(std::make_unique<Impl>())
{
  impl_->opt = std::move(opt);
  impl_->kernels = std::move(kernels);
  impl_->connect();
}

ProductDaemon::~ProductDaemon() = default;

ProductPassStats ProductDaemon::step(std::size_t max_jobs)
{
  return impl_->step(max_jobs);
}

void ProductDaemon::run(
  const std::atomic<bool>& stop,
  std::chrono::milliseconds poll)
{
  while (!stop.load())
  {
    bool busy = false;
    try
    {
      const auto st = impl_->step(64);
      for (const auto& line : st.log)
      {
        std::fprintf(stderr, "atmo_productd: %s\n", line.c_str());
      }
      busy = st.ran + st.failed > 0;
    }
    catch (const std::exception& e)
    {
      std::fprintf(
        stderr, "atmo_productd: %s; reconnecting in 10 s\n", e.what());
      for (int i = 0; i < 100 && !stop.load(); ++i)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      try
      {
        impl_->connect();
        impl_->registered = false;
      }
      catch (const std::exception& e2)
      {
        std::fprintf(stderr, "atmo_productd: %s\n", e2.what());
      }
      continue;
    }
    if (!busy)
    {
      std::this_thread::sleep_for(poll);
    }
  }
}

}  // namespace atmo
