#include "atmo/products_db.h"

#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace atmo
{

namespace
{

using json = nlohmann::json;

std::string str(const std::optional<std::string>& v)
{
  return v ? *v : std::string();
}

std::uint64_t u64(const std::optional<std::string>& v)
{
  return v ? std::stoull(*v) : 0;
}

std::int64_t i64(const std::optional<std::string>& v)
{
  return v ? std::stoll(*v) : 0;
}

double dbl(const std::optional<std::string>& v)
{
  return v ? std::stod(*v) : 0.0;
}

std::string num(std::uint64_t v)
{
  return std::to_string(v);
}

std::string inputs_json(const std::map<std::string, std::uint64_t>& m)
{
  json j = json::object();
  for (const auto& [k, v] : m)
  {
    j[k] = v;
  }
  return j.dump();
}

std::map<std::string, std::uint64_t> inputs_from(const std::string& text)
{
  std::map<std::string, std::uint64_t> out;
  const json j = json::parse(text.empty() ? "{}" : text);
  for (auto it = j.begin(); it != j.end(); ++it)
  {
    if (it.value().is_number_unsigned() || it.value().is_number_integer())
    {
      out[it.key()] = it.value().get<std::uint64_t>();
    }
  }
  return out;
}

}  // namespace

ProductsDb::ProductsDb(Catalog& db) : db_(db)
{
  const auto r =
    db_.query("SELECT schema_version FROM derived_state WHERE id = 1");
  if (r.size() != 1 || !r[0][0])
  {
    throw std::runtime_error(
      "products: derived_state row missing (web migrations not applied?)");
  }
  if (*r[0][0] != std::to_string(kDerivedSchemaVersion))
  {
    throw std::runtime_error(
      "products: derived schema version " + *r[0][0] + ", this engine knows " +
      std::to_string(kDerivedSchemaVersion));
  }
}

void ProductsDb::heartbeat()
{
  db_.query(
    "UPDATE derived_state SET heartbeat_at = UTC_TIMESTAMP() WHERE id = 1");
}

std::uint64_t ProductsDb::cursor()
{
  const auto r = db_.query("SELECT cursor_seq FROM derived_state WHERE id = 1");
  return r.empty() ? 0 : u64(r[0][0]);
}

void ProductsDb::set_cursor(std::uint64_t seq)
{
  db_.query(
    "UPDATE derived_state SET cursor_seq = " + num(seq) + " WHERE id = 1");
}

std::vector<Product> ProductsDb::products()
{
  std::map<std::uint64_t, Product> by_id;
  for (const auto& r : db_.query(
         "SELECT id, slug, name, processor_kind, processor, kernel_id, params, "
         "enabled, process_id, spinup_s, revision, rebuild_requested_at "
         "FROM derived_product ORDER BY id"))
  {
    Product p;
    p.id = u64(r[0]);
    p.slug = str(r[1]);
    p.name = str(r[2]);
    p.kind =
      str(r[3]) == "builtin" ? ProcessorKind::builtin : ProcessorKind::kernel;
    p.processor = str(r[4]);
    if (r[5])
    {
      p.kernel_id = u64(r[5]);
    }
    const json params = json::parse(str(r[6]).empty() ? "{}" : str(r[6]));
    if (params.is_object())
    {
      for (auto it = params.begin(); it != params.end(); ++it)
      {
        if (it.value().is_number())
        {
          p.params[it.key()] = it.value().get<double>();
        }
      }
    }
    p.enabled = str(r[7]) == "1";
    p.process_id = static_cast<std::uint32_t>(u64(r[8]));
    if (r[9])
    {
      p.spinup_s = i64(r[9]);
    }
    p.revision = static_cast<int>(i64(r[10]));
    p.rebuild_requested = r[11];
    by_id[p.id] = std::move(p);
  }
  for (const auto& r :
       db_.query("SELECT product_id, role, offset_rule, model_grid_api, field, "
                 "own_output_role, step_s, required FROM derived_product_input "
                 "ORDER BY product_id, id"))
  {
    auto it = by_id.find(u64(r[0]));
    if (it == by_id.end())
    {
      continue;
    }
    ProductInput in;
    in.role = str(r[1]);
    in.rule = offset_rule_from(str(r[2])).value_or(OffsetRule::same);
    in.model = str(r[3]);
    in.field = str(r[4]);
    in.own_output = str(r[5]);
    in.step_s = i64(r[6]);
    in.required = str(r[7]) != "0";
    it->second.inputs.push_back(std::move(in));
  }
  for (const auto& r : db_.query(
         "SELECT product_id, role, discipline, category, number, level1_type, "
         "level1_value, level2_type, level2_value, units "
         "FROM derived_product_output ORDER BY product_id, id"))
  {
    auto it = by_id.find(u64(r[0]));
    if (it == by_id.end())
    {
      continue;
    }
    ProductOutput o;
    o.role = str(r[1]);
    o.discipline = static_cast<std::uint32_t>(u64(r[2]));
    o.category = static_cast<std::uint32_t>(u64(r[3]));
    o.number = static_cast<std::uint32_t>(u64(r[4]));
    o.level1 = {static_cast<std::uint32_t>(u64(r[5])), dbl(r[6])};
    o.level2 = {static_cast<std::uint32_t>(u64(r[7])), dbl(r[8])};
    o.units = str(r[9]);
    it->second.outputs.push_back(std::move(o));
  }
  std::vector<Product> out;
  for (auto& [id, p] : by_id)
  {
    out.push_back(std::move(p));
  }
  return out;
}

std::vector<ProductKernel> ProductsDb::kernels()
{
  std::vector<ProductKernel> out;
  for (const auto& r : db_.query(
         "SELECT id, name, revision, source, entry, inputs, outputs, params, "
         "source_hash, built_revision, build_status FROM derived_kernel "
         "ORDER BY id"))
  {
    ProductKernel k;
    k.id = u64(r[0]);
    k.name = str(r[1]);
    k.revision = static_cast<int>(i64(r[2]));
    k.source = str(r[3]);
    k.entry = str(r[4]);
    k.iface = ProductInterface::from_json(str(r[5]), str(r[6]), str(r[7]));
    k.source_hash = str(r[8]);
    if (r[9])
    {
      k.built_revision = static_cast<int>(i64(r[9]));
    }
    k.build_status = str(r[10]);
    out.push_back(std::move(k));
  }
  return out;
}

void ProductsDb::register_builtins(const std::vector<BuiltinProcessor>& b)
{
  db_.query("START TRANSACTION");
  try
  {
    db_.query("DELETE FROM derived_processor");
    for (const auto& p : b)
    {
      std::string in, out, par;
      p.iface.to_json(in, out, par);
      db_.query(
        "INSERT INTO derived_processor (name, version, description, inputs, "
        "outputs, params, updated_at) VALUES (" +
        db_.quote(p.name) + ", " + std::to_string(p.version) + ", " +
        db_.quote(p.description) + ", " + db_.quote(in) + ", " +
        db_.quote(out) + ", " + db_.quote(par) + ", UTC_TIMESTAMP())");
    }
    db_.query("COMMIT");
  }
  catch (...)
  {
    db_.query("ROLLBACK");
    throw;
  }
}

void ProductsDb::set_build(
  std::uint64_t kernel_id,
  int revision,
  bool ok,
  const std::string& log)
{
  // Only if nobody saved a newer revision meanwhile.
  db_.query(
    "UPDATE derived_kernel SET build_status = " +
    db_.quote(ok ? "ok" : "error") + ", build_log = " +
    (log.empty() ? std::string("NULL") : db_.quote(log.substr(0, 60000))) +
    ", built_revision = " + std::to_string(revision) + " WHERE id = " +
    num(kernel_id) + " AND revision = " + std::to_string(revision));
}

std::vector<ArrivedMessage> ProductsDb::arrivals(
  std::uint64_t after,
  std::uint64_t upto,
  std::size_t limit,
  std::uint64_t& last)
{
  std::vector<ArrivedMessage> out;
  last = after;
  const auto rows = db_.query(
    "SELECT id, run_id, signature_id, level1_type, level1_value, "
    "offset_end_s, ingest_seq FROM grib_message WHERE ingest_seq > " +
    num(after) + " AND ingest_seq <= " + num(upto) +
    " ORDER BY ingest_seq, id LIMIT " + std::to_string(limit));
  for (const auto& r : rows)
  {
    ArrivedMessage m;
    m.id = u64(r[0]);
    m.run_id = u64(r[1]);
    m.signature_id = u64(r[2]);
    m.level1 = {static_cast<std::uint32_t>(u64(r[3])), dbl(r[4])};
    m.offset_end_s = i64(r[5]);
    last = std::max(last, u64(r[6]));
    out.push_back(m);
  }
  if (rows.size() < limit)
  {
    last = upto;
    return out;
  }
  // Cut at the limit: never inside one generation (its rest would be lost
  // when the cursor moves to it).  Keep the complete generations; if the
  // first one alone is larger than `limit`, read it whole.
  const std::uint64_t cut = last;
  std::size_t keep = 0;
  for (std::size_t k = rows.size(); k-- > 0;)
  {
    if (u64(rows[k][6]) != cut)
    {
      keep = k + 1;
      break;
    }
  }
  if (keep > 0)
  {
    out.resize(keep);
    last = u64(rows[keep - 1][6]);
    return out;
  }
  out.clear();
  for (const auto& r : db_.query(
         "SELECT id, run_id, signature_id, level1_type, level1_value, "
         "offset_end_s FROM grib_message WHERE ingest_seq = " +
         num(cut) + " ORDER BY id"))
  {
    ArrivedMessage m;
    m.id = u64(r[0]);
    m.run_id = u64(r[1]);
    m.signature_id = u64(r[2]);
    m.level1 = {static_cast<std::uint32_t>(u64(r[3])), dbl(r[4])};
    m.offset_end_s = i64(r[5]);
    out.push_back(m);
  }
  last = cut;
  return out;
}

std::vector<std::pair<std::uint64_t, std::string>>
ProductsDb::rebuild_requests()
{
  std::vector<std::pair<std::uint64_t, std::string>> out;
  for (const auto& r : db_.query(
         "SELECT id, rebuild_requested_at FROM derived_product WHERE "
         "rebuild_requested_at IS NOT NULL AND enabled = 1 ORDER BY id"))
  {
    out.emplace_back(u64(r[0]), str(r[1]));
  }
  return out;
}

bool ProductsDb::clear_rebuild(
  std::uint64_t product_id,
  const std::string& seen)
{
  db_.query(
    "UPDATE derived_product SET rebuild_requested_at = NULL WHERE id = " +
    num(product_id) + " AND rebuild_requested_at = " + db_.quote(seen));
  const auto r = db_.query("SELECT ROW_COUNT()");
  return !r.empty() && u64(r[0][0]) == 1;
}

JobInsertStats ProductsDb::add_jobs(const std::vector<JobPlan>& plans)
{
  JobInsertStats st;
  for (const auto& p : plans)
  {
    const std::string key =
      "product_id = " + num(p.product_id) +
      " AND source_run_id = " + num(p.run_id) +
      " AND offset_end_s = " + std::to_string(p.offset_end_s);
    const std::string version =
      "product_revision = " + std::to_string(p.product_revision) +
      " AND processor_version = " + std::to_string(p.processor_version) +
      " AND kernel_revision = " + std::to_string(p.kernel_revision);
    const std::string in = db_.quote(inputs_json(p.inputs));
    const std::string flags =
      std::to_string(p.chain_start ? kJobChainStart : 0);
    db_.query(
      "INSERT IGNORE INTO derived_job (product_id, product_revision, "
      "processor_version, kernel_revision, source_run_id, offset_end_s, "
      "status, attempts, inputs, flags, queued_at) VALUES (" +
      num(p.product_id) + ", " + std::to_string(p.product_revision) + ", " +
      std::to_string(p.processor_version) + ", " +
      std::to_string(p.kernel_revision) + ", " + num(p.run_id) + ", " +
      std::to_string(p.offset_end_s) + ", 'queued', 0, " + in + ", " + flags +
      ", UTC_TIMESTAMP())");
    auto rc = db_.query("SELECT ROW_COUNT()");
    if (!rc.empty() && u64(rc[0][0]) == 1)
    {
      ++st.created;
    }
    else
    {
      // The same job with other input messages (a newer copy replaced one,
      // or a chain start can now see its history): run it again.
      db_.query(
        "UPDATE derived_job SET inputs = " + in + ", flags = " + flags +
        ", status = 'queued', attempts = 0, error = NULL, claimed_by = NULL, "
        "started_at = NULL, finished_at = NULL, queued_at = UTC_TIMESTAMP() "
        "WHERE " +
        key + " AND " + version +
        " AND status <> 'running' AND (inputs <> CAST(" + in +
        " AS JSON) OR flags <> " + flags + ")");
      rc = db_.query("SELECT ROW_COUNT()");
      if (!rc.empty() && u64(rc[0][0]) == 1)
      {
        ++st.requeued;
      }
    }
    db_.query(
      "UPDATE derived_job SET status = 'superseded' WHERE " + key +
      " AND NOT (" + version +
      ") AND status <> 'running' AND status <> "
      "'superseded'");
    rc = db_.query("SELECT ROW_COUNT()");
    st.superseded += rc.empty() ? 0 : u64(rc[0][0]);
  }
  return st;
}

std::size_t ProductsDb::requeue_stale(int timeout_s)
{
  db_.query(
    "UPDATE derived_job SET status = 'queued', claimed_by = NULL, "
    "started_at = NULL WHERE status = 'running' AND started_at < "
    "UTC_TIMESTAMP() - INTERVAL " +
    std::to_string(timeout_s) + " SECOND");
  const auto r = db_.query("SELECT ROW_COUNT()");
  return r.empty() ? 0 : u64(r[0][0]);
}

std::optional<DerivedJob> ProductsDb::claim(const std::string& worker)
{
  // Oldest first, but a disabled product's jobs wait.
  db_.query(
    "UPDATE derived_job j SET j.status = 'running', j.claimed_by = " +
    db_.quote(worker) +
    ", j.started_at = UTC_TIMESTAMP(), j.attempts = j.attempts + 1 "
    "WHERE j.id = (SELECT id FROM (SELECT q.id FROM derived_job q JOIN "
    "derived_product p ON p.id = q.product_id WHERE q.status = 'queued' AND "
    "q.queued_at <= UTC_TIMESTAMP() AND p.enabled = 1 ORDER BY q.queued_at, "
    "q.id LIMIT 1) AS pick) AND "
    "j.status = 'queued'");
  const auto rc = db_.query("SELECT ROW_COUNT()");
  if (rc.empty() || u64(rc[0][0]) != 1)
  {
    return std::nullopt;
  }
  const auto r = db_.query(
    "SELECT id, product_id, product_revision, processor_version, "
    "kernel_revision, source_run_id, offset_end_s, inputs, flags, attempts "
    "FROM derived_job WHERE status = 'running' AND claimed_by = " +
    db_.quote(worker) + " ORDER BY started_at DESC, id DESC LIMIT 1");
  if (r.empty())
  {
    return std::nullopt;
  }
  DerivedJob j;
  j.id = u64(r[0][0]);
  j.product_id = u64(r[0][1]);
  j.product_revision = static_cast<int>(i64(r[0][2]));
  j.processor_version = static_cast<int>(i64(r[0][3]));
  j.kernel_revision = static_cast<int>(i64(r[0][4]));
  j.run_id = u64(r[0][5]);
  j.offset_end_s = i64(r[0][6]);
  j.inputs = inputs_from(str(r[0][7]));
  j.flags = static_cast<std::uint32_t>(u64(r[0][8]));
  j.attempts = static_cast<int>(i64(r[0][9]));
  return j;
}

void ProductsDb::done(
  std::uint64_t job_id,
  const std::map<std::string, std::uint64_t>& outputs,
  const std::string& device)
{
  db_.query(
    "UPDATE derived_job SET status = 'done', outputs = " +
    db_.quote(inputs_json(outputs)) +
    ", device = " + db_.quote(device.substr(0, 128)) +
    ", error = NULL, finished_at = UTC_TIMESTAMP() WHERE id = " + num(job_id));
}

void ProductsDb::failed(
  std::uint64_t job_id,
  const std::string& error,
  int attempts,
  int max_attempts,
  int retry_delay_s)
{
  const bool again = attempts < max_attempts;
  db_.query(
    std::string("UPDATE derived_job SET status = ") +
    (again ? "'queued', claimed_by = NULL, started_at = NULL, queued_at = "
             "UTC_TIMESTAMP() + INTERVAL " +
               std::to_string(retry_delay_s) + " SECOND"
           : std::string("'failed'")) +
    ", error = " + db_.quote(error.substr(0, 60000)) + ", finished_at = " +
    (again ? "NULL" : "UTC_TIMESTAMP()") + " WHERE id = " + num(job_id));
}

void ProductsDb::superseded(std::uint64_t job_id)
{
  db_.query(
    "UPDATE derived_job SET status = 'superseded', finished_at = "
    "UTC_TIMESTAMP() WHERE id = " +
    num(job_id));
}

std::map<std::string, std::uint64_t> ProductsDb::output_messages(
  const std::string& file_path,
  const Product& p)
{
  std::map<std::string, std::uint64_t> out;
  const auto rows = db_.query(
    "SELECT m.id, s.discipline, s.category, s.number, m.level1_type, "
    "m.level1_value FROM grib_message m JOIN grib_file f ON f.id = m.file_id "
    "JOIN grib_signature s ON s.id = m.signature_id WHERE f.path = " +
    db_.quote(file_path));
  for (const auto& o : p.outputs)
  {
    for (const auto& r : rows)
    {
      if (
        u64(r[1]) == o.discipline && u64(r[2]) == o.category &&
        u64(r[3]) == o.number && u64(r[4]) == o.level1.type &&
        static_cast<float>(dbl(r[5])) == static_cast<float>(o.level1.value))
      {
        out[o.role] = u64(r[0]);
        break;
      }
    }
  }
  return out;
}

void ProductsDb::name_outputs(const Product& p)
{
  const std::string model_where =
    "m.process_id = " + std::to_string(p.process_id) +
    " AND s.code = 0 AND c.code = 254";
  db_.query(
    "UPDATE grib_model m JOIN grib_subcenter s ON s.id = m.subcenter_id JOIN "
    "grib_center c ON c.id = s.center_id SET m.name = " +
    db_.quote(p.name.substr(0, 255)) + " WHERE m.name = '' AND " + model_where);
  // The slug as api_name, unless another model grid already has it.
  const auto taken = db_.query(
    "SELECT id FROM grib_model_grid WHERE api_name = " + db_.quote(p.slug));
  if (!taken.empty())
  {
    return;
  }
  const auto grids = db_.query(
    "SELECT mg.id FROM grib_model_grid mg JOIN grib_model m ON m.id = "
    "mg.model_id JOIN grib_subcenter s ON s.id = m.subcenter_id JOIN "
    "grib_center c ON c.id = s.center_id WHERE " +
    model_where + " ORDER BY mg.id LIMIT 1");
  if (!grids.empty())
  {
    db_.query(
      "UPDATE grib_model_grid SET api_name = " + db_.quote(p.slug) +
      " WHERE id = " + str(grids[0][0]));
  }
}

}  // namespace atmo
