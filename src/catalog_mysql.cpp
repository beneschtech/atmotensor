// Grid catalog writer (T16) on the MySQL C API.  Uses only what MySQL's
// libmysqlclient and MariaDB Connector/C have in common; the SQL needs a
// MySQL 8.0.19+ server (row alias in ON DUPLICATE KEY UPDATE, Q14).
#include <mysql.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>

#include "atmo/catalog.h"

namespace atmo
{

namespace
{

constexpr std::size_t kChunk = 500;

std::string datetime(std::int64_t t)
{
  const std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
  ::gmtime_r(&tt, &tm);
  char b[32];
  std::strftime(b, sizeof b, "'%Y-%m-%d %H:%M:%S'", &tm);
  return b;
}

std::string num(double v)
{
  if (!std::isfinite(v))
  {
    v = 0.0;
  }
  char b[40];
  std::snprintf(b, sizeof b, "%.17g", v);
  return b;
}

template <class T>
std::string num(T v)
{
  return std::to_string(v);
}

std::string fit(const std::string& s, std::size_t n)
{
  return s.size() <= n ? s : s.substr(0, n);
}

}  // namespace

struct Catalog::Impl
{
  MYSQL* db = nullptr;

  // Here rather than in ~Catalog: impl_ is destroyed even when the
  // constructor throws after connecting (e.g. wrong schema version).
  ~Impl()
  {
    if (db)
    {
      mysql_close(db);
    }
  }

  [[noreturn]] void fail(const std::string& what)
  {
    throw std::runtime_error("catalog: " + what + ": " + mysql_error(db));
  }

  void exec(const std::string& sql)
  {
    if (mysql_real_query(db, sql.data(), sql.size()) != 0)
    {
      fail(sql.substr(0, 120));
    }
    // Statements that return rows must not be run through exec().
    if (MYSQL_RES* r = mysql_store_result(db))
    {
      mysql_free_result(r);
    }
  }

  std::vector<Row> rows(const std::string& sql)
  {
    if (mysql_real_query(db, sql.data(), sql.size()) != 0)
    {
      fail(sql.substr(0, 120));
    }
    MYSQL_RES* r = mysql_store_result(db);
    std::vector<Row> out;
    if (!r)
    {
      if (mysql_field_count(db) != 0)
      {
        fail("reading result");
      }
      return out;
    }
    const unsigned n = mysql_num_fields(r);
    while (MYSQL_ROW row = mysql_fetch_row(r))
    {
      const unsigned long* len = mysql_fetch_lengths(r);
      Row x(n);
      for (unsigned i = 0; i < n; ++i)
      {
        if (row[i])
        {
          x[i] = std::string(row[i], len[i]);
        }
      }
      out.push_back(std::move(x));
    }
    mysql_free_result(r);
    return out;
  }

  std::uint64_t insert_id()
  {
    return static_cast<std::uint64_t>(mysql_insert_id(db));
  }

  // INSERT ... ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id): the id of
  // the new or the existing row.
  std::uint64_t upsert(const std::string& insert)
  {
    exec(insert + " ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)");
    const std::uint64_t id = insert_id();
    if (id == 0)
    {
      fail("upsert returned no id");
    }
    return id;
  }

  // The (model, grid) row; a new one gets a default api_name (Q19a), made
  // unique with _2, _3, ... (writers are serialised by the generation row
  // lock; the web app renaming at the same moment can still make the
  // insert fail, which rolls the transaction back).
  std::uint64_t model_grid(
    std::uint64_t model,
    std::uint64_t grid,
    const std::string& grid_name)
  {
    const auto have = rows(
      "SELECT id FROM grib_model_grid WHERE model_id = " + num(model) +
      " AND grid_id = " + num(grid));
    if (!have.empty() && have[0][0])
    {
      return std::stoull(*have[0][0]);
    }
    const auto lab = rows(
      "SELECT IF(name = '', slug, name) FROM grib_model WHERE id = " +
      num(model));
    const std::string base = catalog_api_name(
      lab.empty() ? std::string() : lab[0][0].value_or(""), grid_name);
    std::string api = base;
    for (int n = 2;
         !rows("SELECT 1 FROM grib_model_grid WHERE api_name = " + q(api))
            .empty();
         ++n)
    {
      api = base + "_" + std::to_string(n);
    }
    return upsert(
      "INSERT INTO grib_model_grid (model_id, grid_id, name, api_name) "
      "VALUES (" +
      num(model) + "," + num(grid) + "," + q(fit(grid_name, 255)) + "," +
      q(api) + ")");
  }

  // Records (old_id, new_id) pairs from `pairs` (a SELECT with columns
  // old_id, new_id) in grib_message_replaced (Q16b), keeping chains flat:
  // ids that pointed at an old id now point at its replacement.
  void record_replaced(const std::string& pairs)
  {
    exec("DROP TEMPORARY TABLE IF EXISTS atmo_pairs");
    exec("CREATE TEMPORARY TABLE atmo_pairs " + pairs);
    exec(
      "UPDATE grib_message_replaced r JOIN atmo_pairs p ON r.new_id = "
      "p.old_id SET r.new_id = p.new_id");
    exec(
      "INSERT INTO grib_message_replaced (old_id, new_id, replaced_at) "
      "SELECT * FROM (SELECT old_id, new_id, UTC_TIMESTAMP() AS t FROM "
      "atmo_pairs) AS n ON DUPLICATE KEY UPDATE new_id = n.new_id, "
      "replaced_at = n.t");
    exec("DROP TEMPORARY TABLE atmo_pairs");
  }

  std::string q(const std::string& s)
  {
    std::string out(2 * s.size() + 3, '\0');
    out[0] = '\'';
    const unsigned long n = mysql_real_escape_string(
      db, &out[1], s.data(), static_cast<unsigned long>(s.size()));
    out.resize(n + 1);
    out += '\'';
    return out;
  }
};

Catalog::Catalog(const CatalogConfig& c) : impl_(std::make_unique<Impl>())
{
  static std::once_flag once;
  std::call_once(once, [] { mysql_library_init(0, nullptr, nullptr); });
  impl_->db = mysql_init(nullptr);
  if (!impl_->db)
  {
    throw std::runtime_error("catalog: mysql_init failed");
  }
  unsigned timeout = 10;
  mysql_options(impl_->db, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(impl_->db, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  if (!mysql_real_connect(
        impl_->db, c.host.empty() ? nullptr : c.host.c_str(), c.user.c_str(),
        c.password.empty() ? nullptr : c.password.c_str(), c.database.c_str(),
        c.port, c.socket.empty() ? nullptr : c.socket.c_str(), 0))
  {
    const std::string err = mysql_error(impl_->db);
    mysql_close(impl_->db);
    impl_->db = nullptr;
    throw std::runtime_error("catalog: cannot connect: " + err);
  }
  impl_->exec("SET SESSION time_zone = '+00:00'");
  const auto r =
    impl_->rows("SELECT schema_version FROM grib_catalog_state WHERE id = 1");
  if (r.size() != 1 || !r[0][0])
  {
    throw std::runtime_error("catalog: grib_catalog_state row missing");
  }
  if (*r[0][0] != std::to_string(kCatalogSchemaVersion))
  {
    throw std::runtime_error(
      "catalog: schema version " + *r[0][0] + ", this engine knows " +
      std::to_string(kCatalogSchemaVersion));
  }
}

Catalog::~Catalog() = default;

std::vector<Catalog::Row> Catalog::query(const std::string& sql)
{
  return impl_->rows(sql);
}

std::string Catalog::quote(const std::string& s)
{
  return impl_->q(s);
}

std::uint64_t Catalog::generation()
{
  const auto r =
    impl_->rows("SELECT generation FROM grib_catalog_state WHERE id = 1");
  if (r.size() != 1 || !r[0][0])
  {
    throw std::runtime_error("catalog: grib_catalog_state row missing");
  }
  return std::stoull(*r[0][0]);
}

CatalogIngestStats Catalog::ingest(const CatalogFile& file)
{
  file.validate();
  Impl& d = *impl_;
  CatalogIngestStats st;
  d.exec("START TRANSACTION");
  try
  {
    // Generation first: the row lock serialises ingest transactions, so
    // ingest_seq <= generation is complete once the generation is seen.
    d.exec(
      "UPDATE grib_catalog_state SET generation = "
      "LAST_INSERT_ID(generation + 1) WHERE id = 1");
    st.generation = d.insert_id();
    const std::string gen = num(st.generation);

    std::map<GridKey, std::uint64_t> grid_id;
    std::map<GridKey, const CatalogGrid*> grid_of;
    for (const auto& g : file.grids)
    {
      grid_of[g.key] = &g;
      grid_id[g.key] = d.upsert(
        "INSERT INTO grib_grid (grid_key, grid_type, ni, nj, periodic_i, "
        "west, south, east, north, summary, viewable) VALUES (" +
        d.q(key_hex(g.key)) + "," + num(g.grid_type) + "," + num(g.ni) + "," +
        num(g.nj) + "," + (g.periodic_i ? "1" : "0") + "," + num(g.west) + "," +
        num(g.south) + "," + num(g.east) + "," + num(g.north) + "," +
        d.q(g.summary.empty() ? "{}" : g.summary) + "," +
        (g.viewable ? "1" : "0") + ")");
    }

    d.exec(
      "INSERT INTO grib_file (path, size, messages_total, ingested_at) "
      "VALUES (" +
      d.q(file.path) + "," + num(file.size) + "," + num(file.messages_total) +
      ", UTC_TIMESTAMP()) AS new ON DUPLICATE KEY UPDATE "
      "id = LAST_INSERT_ID(grib_file.id), size = new.size, "
      "messages_total = new.messages_total, ingested_at = new.ingested_at");
    const std::uint64_t file_id = d.insert_id();

    // Hierarchy, one upsert per distinct natural key.
    std::map<std::uint32_t, std::uint64_t> centers;
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint64_t> subs;
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint64_t> models;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> mgrids;
    std::map<std::tuple<std::uint64_t, std::int64_t, int>, std::uint64_t> runs;
    auto run_of = [&](const CatalogMessage& m) -> std::uint64_t
    {
      const GribHeader& h = m.header;
      auto c = centers.find(h.sig.centre);
      if (c == centers.end())
      {
        c =
          centers
            .emplace(
              h.sig.centre, d.upsert(
                              "INSERT INTO grib_center (code, name) VALUES (" +
                              num(h.sig.centre) + ", '')"))
            .first;
      }
      const auto sk = std::make_pair(c->second, h.subcentre);
      auto s = subs.find(sk);
      if (s == subs.end())
      {
        s = subs
              .emplace(
                sk, d.upsert(
                      "INSERT INTO grib_subcenter (center_id, "
                      "code, name) VALUES (" +
                      num(c->second) + "," + num(h.subcentre) + ", '')"))
              .first;
      }
      const auto mk = std::make_pair(s->second, h.process);
      auto mo = models.find(mk);
      if (mo == models.end())
      {
        // Model alias (Q18a): the web app maps (centre, subcentre,
        // process) to another model.
        const auto al = d.rows(
          "SELECT model_id FROM grib_model_alias WHERE center_code = " +
          num(h.sig.centre) + " AND subcenter_code = " + num(h.subcentre) +
          " AND process_id = " + num(h.process));
        if (!al.empty() && al[0][0])
        {
          mo = models.emplace(mk, std::stoull(*al[0][0])).first;
        }
      }
      if (mo == models.end())
      {
        const std::string slug = std::to_string(h.sig.centre) + "-" +
                                 std::to_string(h.subcentre) + "-" +
                                 std::to_string(h.process);
        mo = models
               .emplace(
                 mk, d.upsert(
                       "INSERT INTO grib_model (subcenter_id, "
                       "process_id, slug, name) VALUES (" +
                       num(s->second) + "," + num(h.process) + "," + d.q(slug) +
                       ", '')"))
               .first;
      }
      const auto gk = std::make_pair(mo->second, grid_id.at(m.grid));
      auto mg = mgrids.find(gk);
      if (mg == mgrids.end())
      {
        mg =
          mgrids
            .emplace(
              gk, d.model_grid(mo->second, gk.second, grid_of.at(m.grid)->name))
            .first;
      }
      const auto rk = std::make_tuple(mg->second, h.base_time_s, h.member);
      auto r = runs.find(rk);
      if (r == runs.end())
      {
        r = runs
              .emplace(
                rk,
                d.upsert(
                  "INSERT INTO grib_run (model_grid_id, "
                  "basetime, `member`, member_type) "
                  "VALUES (" +
                  num(mg->second) + "," + datetime(h.base_time_s) + "," +
                  num(h.member) + "," +
                  (h.member_type ? num(*h.member_type) : std::string("NULL")) +
                  ")"))
              .first;
      }
      return r->second;
    };

    // Signatures: look the file's distinct hashes up in chunks, insert the
    // new ones.
    std::map<std::uint64_t, const GribSignature*> sigs;
    for (const auto& m : file.messages)
    {
      sigs.emplace(m.header.sig.hash, &m.header.sig);
    }
    std::map<std::uint64_t, std::uint64_t> sig_id;
    {
      std::vector<std::uint64_t> all;
      for (const auto& [h, s] : sigs)
      {
        all.push_back(h);
      }
      for (std::size_t i = 0; i < all.size(); i += kChunk)
      {
        std::string in;
        for (std::size_t k = i; k < std::min(all.size(), i + kChunk); ++k)
        {
          in += (k > i ? "," : "") + d.q(key_hex(all[k]));
        }
        for (const auto& row : d.rows(
               "SELECT id, hash, canonical FROM grib_signature WHERE "
               "hash IN (" +
               in + ")"))
        {
          const std::uint64_t h = std::stoull(*row[1], nullptr, 16);
          if (*row[2] != sigs.at(h)->canonical)
          {
            throw std::runtime_error(
              "catalog: signature hash collision on " + *row[1]);
          }
          sig_id[h] = std::stoull(*row[0]);
        }
      }
    }
    for (const auto& [h, s] : sigs)
    {
      if (sig_id.count(h))
      {
        continue;
      }
      std::string param = "NULL";
      const bool known = s->identified();
      if (known)
      {
        // Level-independent defaults (Q17b); eccodes' level-dependent
        // shortName stays in grib_signature.ecc_short_name, the web app
        // sets grib_parameter.short_name.
        const std::string name = d.q(fit(catalog_parameter_name(*s), 255));
        const std::string units = d.q(fit(catalog_parameter_units(*s), 64));
        const auto r = d.rows(
          "SELECT id FROM grib_parameter WHERE origin = 'eccodes' AND "
          "BINARY name = BINARY " +
          name + " AND BINARY units = BINARY " + units +
          " ORDER BY id LIMIT 1");
        if (!r.empty())
        {
          param = *r[0][0];
        }
        else
        {
          d.exec(
            "INSERT INTO grib_parameter (short_name, name, units, si_scale, "
            "si_offset, origin) VALUES ('', " +
            name + "," + units + ", 1, 0, 'eccodes')");
          param = num(d.insert_id());
        }
      }
      sig_id[h] = d.upsert(
        "INSERT INTO grib_signature (hash, sig_version, canonical, base_hash, "
        "stat_interval_s, center_code, discipline, category, number, pdt, "
        "ecc_short_name, ecc_name, ecc_units, ecc_param_id, parameter_id, "
        "status, first_seen) VALUES (" +
        d.q(key_hex(h)) + "," + num(s->version) + "," + d.q(s->canonical) +
        "," + d.q(key_hex(s->base_hash)) + "," +
        (s->stat_interval_s ? num(*s->stat_interval_s) : std::string("NULL")) +
        "," + num(s->centre) + "," + num(s->discipline) + "," +
        num(s->category) + "," + num(s->number) + "," + num(s->pdt) + "," +
        d.q(fit(s->ecc_short_name, 64)) + "," + d.q(fit(s->ecc_name, 255)) +
        "," + d.q(fit(s->ecc_units, 64)) + "," +
        (s->ecc_param_id ? num(*s->ecc_param_id) : std::string("NULL")) + "," +
        param + "," + (known ? "'identified'" : "'unknown'") +
        ", UTC_TIMESTAMP())");
      ++st.new_signatures;
      st.unknown_signatures += known ? 0 : 1;
    }

    // Messages (Q16b): staged in a temporary table, then
    //  1. existing rows with the same key but other bytes are deleted and
    //     remembered (a changed copy gets a NEW id);
    //  2. everything is upserted: identical bytes keep their row and id
    //     (file, offsets and ingest_seq follow the newest copy);
    //  3. each deleted id is recorded as replaced by the new row's id.
    const std::string cols =
      "run_id, signature_id, grid_id, file_id, level1_type, level1_value, "
      "level2_type, level2_value, offset_start_s, offset_end_s, valid_time, "
      "byte_offset, byte_length, content_hash, flags, ingest_seq, "
      "process_id";
    d.exec("DROP TEMPORARY TABLE IF EXISTS atmo_incoming");
    d.exec(
      "CREATE TEMPORARY TABLE atmo_incoming (run_id INT NOT NULL, "
      "signature_id INT NOT NULL, grid_id INT NULL, file_id INT NOT NULL, "
      "level1_type SMALLINT UNSIGNED NOT NULL, level1_value DOUBLE NOT NULL, "
      "level2_type SMALLINT UNSIGNED NOT NULL, level2_value DOUBLE NOT NULL, "
      "offset_start_s INT NOT NULL, offset_end_s INT NOT NULL, valid_time "
      "DATETIME NOT NULL, byte_offset BIGINT UNSIGNED NOT NULL, byte_length "
      "BIGINT UNSIGNED NOT NULL, content_hash CHAR(16) NOT NULL, flags INT "
      "UNSIGNED NOT NULL, ingest_seq BIGINT UNSIGNED NOT NULL, process_id "
      "INT UNSIGNED NOT NULL, seq INT NOT NULL)");
    std::string sql;
    std::size_t in_chunk = 0;
    for (const auto& m : file.messages)
    {
      const GribHeader& h = m.header;
      const bool sampleable =
        grid_of.at(m.grid)->viewable &&
        (m.flags & (kCatalogUndecodable | kCatalogGridUnsupported)) == 0;
      sql += (in_chunk ? "," : "") + std::string("(") + num(run_of(m)) + "," +
             num(sig_id.at(h.sig.hash)) + "," +
             (sampleable ? num(grid_id.at(m.grid)) : std::string("NULL")) +
             "," + num(file_id) + "," + num(h.level1.type) + "," +
             num(h.level1.value) + "," + num(h.level2.type) + "," +
             num(h.level2.value) + "," + num(h.offset_start_s) + "," +
             num(h.offset_end_s) + "," + datetime(h.valid_time_s) + "," +
             num(m.byte_offset) + "," + num(m.byte_length) + "," +
             d.q(key_hex(m.content_hash)) + "," + num(m.flags) + "," + gen +
             "," + num(h.process) + "," + num(st.messages) + ")";
      if (++in_chunk == kChunk)
      {
        d.exec("INSERT INTO atmo_incoming VALUES " + sql);
        sql.clear();
        in_chunk = 0;
      }
      ++st.messages;
    }
    if (in_chunk)
    {
      d.exec("INSERT INTO atmo_incoming VALUES " + sql);
    }
    const std::string key_eq =
      "a.run_id = b.run_id AND a.signature_id = b.signature_id AND "
      "a.level1_type = b.level1_type AND a.level1_value = b.level1_value AND "
      "a.level2_type = b.level2_type AND a.level2_value = b.level2_value AND "
      "a.offset_end_s = b.offset_end_s AND a.offset_start_s = "
      "b.offset_start_s";
    // 1. Changed copies.  (A temporary table can be named once per
    // statement, hence the second one.)
    d.exec("DROP TEMPORARY TABLE IF EXISTS atmo_replaced");
    d.exec(
      "CREATE TEMPORARY TABLE atmo_replaced SELECT DISTINCT a.id AS old_id, "
      "a.run_id, a.signature_id, a.level1_type, a.level1_value, "
      "a.level2_type, a.level2_value, a.offset_end_s, a.offset_start_s FROM "
      "grib_message a JOIN atmo_incoming b ON " +
      key_eq + " WHERE a.content_hash <> b.content_hash");
    d.exec(
      "DELETE m FROM grib_message m JOIN atmo_replaced r ON m.id = r.old_id");
    st.replaced_messages = static_cast<std::size_t>(mysql_affected_rows(d.db));
    // 2. Upsert, in file order (a key repeated in the file: the last wins).
    d.exec(
      "INSERT INTO grib_message (" + cols + ") SELECT * FROM (SELECT " + cols +
      " FROM atmo_incoming ORDER BY seq) AS new ON DUPLICATE KEY UPDATE "
      "grid_id = new.grid_id, file_id = new.file_id, valid_time = "
      "new.valid_time, byte_offset = new.byte_offset, byte_length = "
      "new.byte_length, content_hash = new.content_hash, flags = new.flags, "
      "ingest_seq = new.ingest_seq, process_id = new.process_id");
    // 3. Old id -> new id.
    if (st.replaced_messages > 0)
    {
      d.record_replaced(
        "SELECT a.old_id, b.id AS new_id FROM atmo_replaced a JOIN "
        "grib_message b ON " +
        key_eq);
    }
    d.exec("DROP TEMPORARY TABLE atmo_replaced");
    d.exec("DROP TEMPORARY TABLE atmo_incoming");

    // Rows of an earlier version of this file that were not written again.
    d.exec(
      "DELETE FROM grib_message WHERE file_id = " + num(file_id) +
      " AND ingest_seq <> " + gen);
    st.removed_messages = static_cast<std::size_t>(mysql_affected_rows(d.db));
    d.exec("COMMIT");
  }
  catch (...)
  {
    mysql_query(d.db, "ROLLBACK");
    throw;
  }
  return st;
}

CatalogExpireStats Catalog::expire(
  const RetentionPolicy& policy,
  const std::filesystem::path& root)
{
  Impl& d = *impl_;
  CatalogExpireStats st;
  std::vector<std::string> paths;
  d.exec("START TRANSACTION");
  try
  {
    d.exec(
      "UPDATE grib_catalog_state SET generation = "
      "LAST_INSERT_ID(generation + 1) WHERE id = 1");
    st.generation = d.insert_id();
    for (const auto& mg :
         d.rows("SELECT id, api_name FROM grib_model_grid ORDER BY id"))
    {
      const std::size_t k = policy.runs_for(mg[1].value_or(""));
      const auto times = d.rows(
        "SELECT DISTINCT basetime FROM grib_run WHERE model_grid_id = " +
        *mg[0] + " ORDER BY basetime DESC");
      if (times.size() <= k)
      {
        continue;
      }
      const std::string cut = d.q(*times[k][0]);  // newest expired
      const std::string where =
        " WHERE model_grid_id = " + *mg[0] + " AND basetime <= " + cut;
      st.messages += std::stoull(
        d.rows(
           "SELECT COUNT(*) FROM grib_message WHERE run_id IN (SELECT id "
           "FROM grib_run" +
           where + ")")
          .at(0)
          .at(0)
          .value_or("0"));
      d.exec("DELETE FROM grib_run" + where);
      st.runs += static_cast<std::size_t>(mysql_affected_rows(d.db));
    }
    for (const auto& f : d.rows(
           "SELECT id, path FROM grib_file f WHERE NOT EXISTS (SELECT 1 FROM "
           "grib_message m WHERE m.file_id = f.id)"))
    {
      paths.push_back(f[1].value_or(""));
      d.exec("DELETE FROM grib_file WHERE id = " + *f[0]);
      ++st.files;
    }
    // Replaced ids whose replacement is gone (chains are flat).
    d.exec(
      "DELETE r FROM grib_message_replaced r LEFT JOIN grib_message m ON "
      "m.id = r.new_id WHERE m.id IS NULL");
    if (st.runs == 0 && st.files == 0)
    {
      d.exec("ROLLBACK");
      CatalogExpireStats none;
      none.generation = generation();
      return none;
    }
    d.exec("COMMIT");
  }
  catch (...)
  {
    mysql_query(d.db, "ROLLBACK");
    throw;
  }
  // After the commit: the rows no longer name these files.
  for (const auto& p : paths)
  {
    if (p.empty() || p.find("..") != std::string::npos || p.rfind("grib/", 0))
    {
      continue;  // never outside root/grib
    }
    const std::filesystem::path f = root / p;
    if (::unlink(f.c_str()) == 0)
    {
      ++st.unlinked;
      std::error_code ec;
      for (auto dir = f.parent_path(); dir != root / "grib";
           dir = dir.parent_path())
      {
        if (
          !std::filesystem::is_empty(dir, ec) || ec ||
          ::rmdir(dir.c_str()) != 0)
        {
          break;
        }
      }
    }
  }
  return st;
}

CatalogRealiasStats Catalog::realias()
{
  Impl& d = *impl_;
  CatalogRealiasStats st;
  d.exec("START TRANSACTION");
  try
  {
    d.exec(
      "UPDATE grib_catalog_state SET generation = "
      "LAST_INSERT_ID(generation + 1) WHERE id = 1");
    st.generation = d.insert_id();
    const std::string gen = num(st.generation);
    auto affected = [&]
    { return static_cast<std::size_t>(mysql_affected_rows(d.db)); };

    std::map<
      std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint64_t>
      alias;
    for (const auto& r : d.rows(
           "SELECT center_code, subcenter_code, process_id, model_id FROM "
           "grib_model_alias"))
    {
      alias[{
        static_cast<std::uint32_t>(std::stoul(r[0].value_or("0"))),
        static_cast<std::uint32_t>(std::stoul(r[1].value_or("0"))),
        static_cast<std::uint32_t>(std::stoul(r[2].value_or("0")))}] =
        std::stoull(r[3].value_or("0"));
    }
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint64_t> own;
    auto own_model = [&](
                       std::uint64_t subcenter_id, std::uint32_t c,
                       std::uint32_t s, std::uint32_t p) -> std::uint64_t
    {
      const auto k = std::make_pair(subcenter_id, p);
      if (auto it = own.find(k); it != own.end())
      {
        return it->second;
      }
      const std::string slug =
        std::to_string(c) + "-" + std::to_string(s) + "-" + std::to_string(p);
      return own[k] = d.upsert(
               "INSERT INTO grib_model (subcenter_id, process_id, slug, name) "
               "VALUES (" +
               num(subcenter_id) + "," + num(p) + "," + d.q(slug) + ", '')");
    };

    // One group per (run, raw process id).
    const auto groups = d.rows(
      "SELECT r.id, msg.process_id, ANY_VALUE(mg.id), ANY_VALUE(mg.model_id), "
      "ANY_VALUE(mg.grid_id), ANY_VALUE(mg.name), ANY_VALUE(c.code), "
      "ANY_VALUE(sc.code), ANY_VALUE(sc.id), ANY_VALUE(r.basetime), "
      "ANY_VALUE(r.`member`), ANY_VALUE(r.member_type) FROM grib_message msg "
      "JOIN grib_run r ON r.id = msg.run_id JOIN grib_model_grid mg ON mg.id "
      "= r.model_grid_id JOIN grib_model m ON m.id = mg.model_id JOIN "
      "grib_subcenter sc ON sc.id = m.subcenter_id JOIN grib_center c ON "
      "c.id = sc.center_id GROUP BY r.id, msg.process_id");
    std::set<std::uint64_t> runs, mgrids, models;
    const std::string same_key =
      "b.signature_id = a.signature_id AND b.level1_type = a.level1_type AND "
      "b.level1_value = a.level1_value AND b.level2_type = a.level2_type AND "
      "b.level2_value = a.level2_value AND b.offset_start_s = "
      "a.offset_start_s AND b.offset_end_s = a.offset_end_s";
    for (const auto& g : groups)
    {
      const std::string run = *g[0];
      const std::string proc = *g[1];
      const auto c = static_cast<std::uint32_t>(std::stoul(*g[6]));
      const auto s = static_cast<std::uint32_t>(std::stoul(*g[7]));
      const auto p = static_cast<std::uint32_t>(std::stoul(proc));
      const std::uint64_t model = std::stoull(*g[3]);
      const auto a = alias.find({c, s, p});
      const std::uint64_t want =
        a != alias.end() ? a->second : own_model(std::stoull(*g[8]), c, s, p);
      if (want == model)
      {
        continue;
      }
      const std::uint64_t mg2 =
        d.model_grid(want, std::stoull(*g[4]), g[5].value_or(""));
      const std::string run2 = num(d.upsert(
        "INSERT INTO grib_run (model_grid_id, basetime, `member`, "
        "member_type) VALUES (" +
        num(mg2) + "," + d.q(*g[9]) + "," + *g[10] + "," +
        g[11].value_or("NULL") + ")"));
      const std::string src =
        "a.run_id = " + run + " AND a.process_id = " + proc;
      // The older copy of a duplicate is replaced by the newer (Q16b).
      d.record_replaced(
        "SELECT a.id AS old_id, b.id AS new_id FROM grib_message a JOIN "
        "grib_message b ON b.run_id = " +
        run2 + " AND " + same_key + " WHERE " + src +
        " AND a.ingest_seq < b.ingest_seq");
      d.record_replaced(
        "SELECT b.id AS old_id, a.id AS new_id FROM grib_message b JOIN "
        "grib_message a ON " +
        same_key + " WHERE b.run_id = " + run2 + " AND " + src +
        " AND b.ingest_seq <= a.ingest_seq");
      d.exec(
        "DELETE a FROM grib_message a JOIN grib_message b ON b.run_id = " +
        run2 + " AND " + same_key + " WHERE " + src +
        " AND a.ingest_seq < b.ingest_seq");
      st.dropped_duplicates += affected();
      d.exec(
        "DELETE b FROM grib_message b JOIN grib_message a ON " + same_key +
        " WHERE b.run_id = " + run2 + " AND " + src +
        " AND b.ingest_seq <= a.ingest_seq");
      st.dropped_duplicates += affected();
      d.exec(
        "UPDATE grib_message SET run_id = " + run2 + ", ingest_seq = " + gen +
        " WHERE run_id = " + run + " AND process_id = " + proc);
      st.moved_messages += affected();
      runs.insert(std::stoull(run));
      mgrids.insert(std::stoull(*g[2]));
      models.insert(model);
    }
    if (st.moved_messages == 0 && st.dropped_duplicates == 0)
    {
      d.exec("ROLLBACK");
      CatalogRealiasStats none;
      none.generation = generation();
      return none;
    }
    auto list = [](const std::set<std::uint64_t>& ids)
    {
      std::string out;
      for (const auto id : ids)
      {
        out += (out.empty() ? "" : ",") + num(id);
      }
      return out;
    };
    d.exec(
      "DELETE FROM grib_run WHERE id IN (" + list(runs) +
      ") AND NOT EXISTS (SELECT 1 FROM grib_message WHERE run_id = "
      "grib_run.id)");
    st.removed_runs = affected();
    d.exec(
      "DELETE FROM grib_model_grid WHERE id IN (" + list(mgrids) +
      ") AND NOT EXISTS (SELECT 1 FROM grib_run WHERE model_grid_id = "
      "grib_model_grid.id)");
    st.removed_model_grids = affected();
    d.exec(
      "DELETE FROM grib_model WHERE id IN (" + list(models) +
      ") AND name = '' AND NOT EXISTS (SELECT 1 FROM grib_model_grid WHERE "
      "model_id = grib_model.id) AND NOT EXISTS (SELECT 1 FROM "
      "grib_model_alias WHERE model_id = grib_model.id)");
    st.removed_models = affected();
    d.exec("COMMIT");
  }
  catch (...)
  {
    mysql_query(d.db, "ROLLBACK");
    throw;
  }
  return st;
}

}  // namespace atmo

// ---------------------------------------------------------------------------
// CatalogReader (T17)
// ---------------------------------------------------------------------------

namespace atmo
{

namespace
{

std::uint64_t u64(const std::optional<std::string>& s)
{
  return s ? std::stoull(*s) : 0;
}

std::int64_t i64(const std::optional<std::string>& s)
{
  return s ? std::stoll(*s) : 0;
}

double dbl(const std::optional<std::string>& s)
{
  return s ? std::strtod(s->c_str(), nullptr) : 0.0;
}

std::uint32_t u32s(const std::optional<std::string>& s)
{
  return static_cast<std::uint32_t>(u64(s));
}

const char* kMessageColumns =
  "SELECT m.id, m.run_id, m.signature_id, m.grid_id, f.path, m.level1_type, "
  "m.level1_value, m.level2_type, m.level2_value, m.offset_start_s, "
  "m.offset_end_s, UNIX_TIMESTAMP(m.valid_time), m.byte_offset, "
  "m.byte_length, m.content_hash, m.flags, m.process_id FROM grib_message "
  "m JOIN "
  "grib_file f ON f.id = m.file_id ";

CatMessage message_row(const Catalog::Row& r)
{
  CatMessage m;
  m.id = u64(r[0]);
  m.run_id = u64(r[1]);
  m.signature_id = u64(r[2]);
  if (r[3])
  {
    m.grid_id = u64(r[3]);
  }
  m.file_path = r[4].value_or("");
  m.level1 = {u32s(r[5]), dbl(r[6])};
  m.level2 = {u32s(r[7]), dbl(r[8])};
  m.offset_start_s = i64(r[9]);
  m.offset_end_s = i64(r[10]);
  m.valid_time_s = i64(r[11]);
  m.byte_offset = u64(r[12]);
  m.byte_length = u64(r[13]);
  m.content_hash = std::stoull(r[14].value_or("0"), nullptr, 16);
  m.flags = u32s(r[15]);
  m.process_id = u32s(r[16]);
  return m;
}

template <class K, class V>
class Lru
{
 public:
  explicit Lru(std::size_t cap) : cap_(cap) {}
  const V* get(const K& k)
  {
    auto it = map_.find(k);
    if (it == map_.end())
    {
      return nullptr;
    }
    order_.splice(order_.begin(), order_, it->second.second);
    return &it->second.first;
  }
  void put(const K& k, V v)
  {
    if (cap_ == 0)
    {
      return;
    }
    if (auto it = map_.find(k); it != map_.end())
    {
      order_.erase(it->second.second);
      map_.erase(it);
    }
    order_.push_front(k);
    map_.emplace(k, std::make_pair(std::move(v), order_.begin()));
    while (map_.size() > cap_)
    {
      map_.erase(order_.back());
      order_.pop_back();
    }
  }
  void clear()
  {
    map_.clear();
    order_.clear();
  }

 private:
  std::size_t cap_;
  std::list<K> order_;
  std::map<K, std::pair<V, typename std::list<K>::iterator>> map_;
};

}  // namespace

struct CatalogReader::Impl
{
  CatalogConfig config;
  std::chrono::milliseconds poll;
  std::mutex mu;
  std::unique_ptr<Catalog> db;
  std::shared_ptr<const CatalogTables> tables;
  std::chrono::steady_clock::time_point checked{};
  Lru<std::uint64_t, std::vector<std::uint64_t>> run_sigs;
  Lru<std::pair<std::uint64_t, std::uint64_t>, std::vector<CatMessage>> msgs;
  Lru<std::uint64_t, std::optional<CatMessage>> by_id;
  Lru<std::uint64_t, std::vector<std::int64_t>> offsets;
  Lru<std::uint64_t, std::optional<std::uint64_t>> replaced;
  Lru<std::pair<std::uint64_t, std::int64_t>, std::vector<CatMessage>>
    at_offset;
  Stats stats;

  Impl(const CatalogConfig& c, std::chrono::milliseconds p, std::size_t n)
      : config(c),
        poll(p),
        run_sigs(n),
        msgs(n),
        by_id(n),
        offsets(n),
        replaced(n),
        at_offset(n)
  {
  }

  void connect()
  {
    db.reset();
    db = std::make_unique<Catalog>(config);
    db->query("SET SESSION TRANSACTION READ ONLY");
  }

  // Runs f(db) and retries once on a fresh connection.
  template <class F>
  auto with_db(F&& f)
  {
    if (!db)
    {
      connect();
    }
    try
    {
      return f(*db);
    }
    catch (const std::runtime_error&)
    {
      connect();
      return f(*db);
    }
  }

  std::shared_ptr<const CatalogTables> load(Catalog& d)
  {
    d.query("START TRANSACTION WITH CONSISTENT SNAPSHOT");
    try
    {
      auto t = std::make_shared<CatalogTables>();
      t->generation =
        u64(d.query("SELECT generation FROM grib_catalog_state WHERE id = 1")
              .at(0)
              .at(0));
      std::map<std::uint64_t, std::vector<CatSubcenter>> subs;
      for (const auto& r : d.query(
             "SELECT id, center_id, code, name FROM grib_subcenter "
             "ORDER BY code"))
      {
        subs[u64(r[1])].push_back({u64(r[0]), u32s(r[2]), r[3].value_or("")});
      }
      for (const auto& r : d.query("SELECT id, code, name FROM grib_center"))
      {
        CatCenter c{u64(r[0]), u32s(r[1]), r[2].value_or(""), {}};
        c.subcenters = std::move(subs[c.id]);
        t->centers.push_back(std::move(c));
      }
      for (const auto& r : d.query(
             "SELECT m.id, sc.center_id, m.subcenter_id, c.code, sc.code, "
             "m.process_id, m.slug, m.name FROM grib_model m JOIN "
             "grib_subcenter sc ON sc.id = m.subcenter_id JOIN grib_center c "
             "ON c.id = sc.center_id"))
      {
        t->models.push_back(
          {u64(r[0]), u64(r[1]), u64(r[2]), u32s(r[3]), u32s(r[4]), u32s(r[5]),
           r[6].value_or(""), r[7].value_or("")});
      }
      for (const auto& r : d.query(
             "SELECT id, model_id, grid_id, name, api_name FROM "
             "grib_model_grid"))
      {
        t->model_grids.push_back(
          {u64(r[0]), u64(r[1]), u64(r[2]), r[3].value_or(""),
           r[4].value_or("")});
      }
      for (const auto& r : d.query(
             "SELECT id, grid_key, grid_type, ni, nj, periodic_i, west, "
             "south, east, north, viewable, summary FROM "
             "grib_grid"))
      {
        CatGrid g;
        g.id = u64(r[0]);
        g.key = std::stoull(r[1].value_or("0"), nullptr, 16);
        g.type = u32s(r[2]);
        g.ni = u32s(r[3]);
        g.nj = u32s(r[4]);
        g.periodic_i = u64(r[5]) != 0;
        g.west = dbl(r[6]);
        g.south = dbl(r[7]);
        g.east = dbl(r[8]);
        g.north = dbl(r[9]);
        g.viewable = u64(r[10]) != 0;
        g.summary = r[11].value_or("{}");
        t->grids.push_back(g);
      }
      for (const auto& r : d.query(
             "SELECT id, model_grid_id, UNIX_TIMESTAMP(basetime), `member`, "
             "member_type FROM grib_run"))
      {
        CatRun run{
          u64(r[0]), u64(r[1]), i64(r[2]), static_cast<int>(i64(r[3])),
          std::nullopt};
        if (r[4])
        {
          run.member_type = static_cast<int>(i64(r[4]));
        }
        t->runs.push_back(run);
      }
      for (const auto& r : d.query(
             "SELECT s.id, s.hash, s.base_hash, s.stat_interval_s, "
             "s.discipline, s.category, s.number, s.pdt, s.canonical, "
             "s.status, s.ecc_short_name, s.ecc_name, s.ecc_units, "
             "p.short_name, p.name, p.units, p.si_scale, p.si_offset, "
             "s.parameter_id FROM "
             "grib_signature s LEFT JOIN grib_parameter p ON p.id = "
             "s.parameter_id"))
      {
        CatSignature s;
        s.id = u64(r[0]);
        s.hash = std::stoull(r[1].value_or("0"), nullptr, 16);
        s.base_hash = std::stoull(r[2].value_or("0"), nullptr, 16);
        if (r[3])
        {
          s.stat_interval_s = u32s(r[3]);
        }
        s.discipline = u32s(r[4]);
        s.category = u32s(r[5]);
        s.number = u32s(r[6]);
        s.pdt = u32s(r[7]);
        s.canonical = r[8].value_or("");
        s.status = r[9].value_or("");
        if (r[18])
        {
          s.parameter_id = u64(r[18]);
        }
        if (r[14])  // linked parameter
        {
          s.short_name = r[13].value_or("");
          s.name = *r[14];
          s.units = r[15].value_or("");
          s.si_scale = dbl(r[16]);
          s.si_offset = dbl(r[17]);
        }
        else
        {
          GribSignature g;
          g.ecc_short_name = r[10].value_or("unknown");
          g.ecc_name = r[11].value_or("");
          g.ecc_units = r[12].value_or("");
          g.discipline = s.discipline;
          g.category = s.category;
          g.number = s.number;
          g.stat_interval_s = s.stat_interval_s;
          const std::string tsp = "\ntypeOfStatisticalProcessing=";
          const auto at = s.canonical.find(tsp);
          if (
            at != std::string::npos &&
            std::isdigit(
              static_cast<unsigned char>(s.canonical[at + tsp.size()])))
          {
            g.stat_process = static_cast<std::uint32_t>(
              std::strtoul(s.canonical.c_str() + at + tsp.size(), nullptr, 10));
          }
          if (s.status == "unknown")
          {
            g.ecc_short_name = "unknown";
          }
          s.short_name = g.ecc_short_name;
          s.name = catalog_parameter_name(g);
          s.units = catalog_parameter_units(g);
        }
        t->signatures.push_back(std::move(s));
      }
      for (const auto& r : d.query(
             "SELECT name, parameter_id FROM grib_common_name ORDER BY name"))
      {
        t->common_names.push_back({r[0].value_or(""), u64(r[1])});
      }
      d.query("COMMIT");
      t->sort();
      return t;
    }
    catch (...)
    {
      try
      {
        d.query("ROLLBACK");
      }
      catch (...)
      {
      }
      throw;
    }
  }
};

CatalogReader::CatalogReader(
  const CatalogConfig& c,
  std::chrono::milliseconds poll,
  std::size_t cache_entries)
    : impl_(std::make_unique<Impl>(c, poll, cache_entries))
{
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->connect();
  impl_->tables = impl_->load(*impl_->db);
  impl_->checked = std::chrono::steady_clock::now();
  ++impl_->stats.reloads;
}

CatalogReader::~CatalogReader() = default;

std::shared_ptr<const CatalogTables> CatalogReader::tables()
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const auto now = std::chrono::steady_clock::now();
  if (now - d.checked < d.poll)
  {
    return d.tables;
  }
  d.checked = now;
  try
  {
    const std::uint64_t gen = d.with_db(
      [](Catalog& db)
      {
        return std::stoull(
          db.query("SELECT generation FROM grib_catalog_state WHERE id = 1")
            .at(0)
            .at(0)
            .value_or("0"));
      });
    if (gen != d.tables->generation)
    {
      d.tables = d.with_db([&](Catalog& db) { return d.load(db); });
      d.run_sigs.clear();
      d.msgs.clear();
      d.by_id.clear();
      d.offsets.clear();
      d.replaced.clear();
      d.at_offset.clear();
      ++d.stats.reloads;
    }
  }
  catch (const std::exception&)
  {
    // Keep serving the last good tables; the next poll retries.
  }
  return d.tables;
}

std::vector<std::uint64_t> CatalogReader::run_signatures(
  const CatalogTables& t,
  std::uint64_t run_id)
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const bool current = t.generation == d.tables->generation;
  if (current)
  {
    if (const auto* v = d.run_sigs.get(run_id))
    {
      ++d.stats.hits;
      return *v;
    }
  }
  ++d.stats.queries;
  std::vector<std::uint64_t> out;
  for (const auto& r : d.with_db(
         [&](Catalog& db)
         {
           return db.query(
             "SELECT DISTINCT signature_id FROM grib_message WHERE run_id = " +
             std::to_string(run_id));
         }))
  {
    out.push_back(u64(r[0]));
  }
  if (current)
  {
    d.run_sigs.put(run_id, out);
  }
  return out;
}

std::vector<CatMessage> CatalogReader::messages(
  const CatalogTables& t,
  std::uint64_t run_id,
  std::uint64_t sig_id)
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const bool current = t.generation == d.tables->generation;
  const auto key = std::make_pair(run_id, sig_id);
  if (current)
  {
    if (const auto* v = d.msgs.get(key))
    {
      ++d.stats.hits;
      return *v;
    }
  }
  ++d.stats.queries;
  std::vector<CatMessage> out;
  for (const auto& r : d.with_db(
         [&](Catalog& db)
         {
           return db.query(
             std::string(kMessageColumns) +
             "WHERE m.run_id = " + std::to_string(run_id) +
             " AND m.signature_id = " + std::to_string(sig_id));
         }))
  {
    out.push_back(message_row(r));
  }
  if (current)
  {
    d.msgs.put(key, out);
  }
  return out;
}

std::optional<CatMessage> CatalogReader::message(
  const CatalogTables& t,
  std::uint64_t id)
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const bool current = t.generation == d.tables->generation;
  if (current)
  {
    if (const auto* v = d.by_id.get(id))
    {
      ++d.stats.hits;
      return *v;
    }
  }
  ++d.stats.queries;
  const auto rows = d.with_db(
    [&](Catalog& db)
    {
      return db.query(
        std::string(kMessageColumns) + "WHERE m.id = " + std::to_string(id));
    });
  std::optional<CatMessage> out;
  if (!rows.empty())
  {
    out = message_row(rows[0]);
  }
  if (current)
  {
    d.by_id.put(id, out);
  }
  return out;
}

std::optional<std::uint64_t> CatalogReader::replaced_by(
  const CatalogTables& t,
  std::uint64_t id)
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const bool current = t.generation == d.tables->generation;
  if (current)
  {
    if (const auto* v = d.replaced.get(id))
    {
      ++d.stats.hits;
      return *v;
    }
  }
  ++d.stats.queries;
  const auto rows = d.with_db(
    [&](Catalog& db)
    {
      return db.query(
        "SELECT r.new_id FROM grib_message_replaced r JOIN grib_message m "
        "ON m.id = r.new_id WHERE r.old_id = " +
        std::to_string(id));
    });
  std::optional<std::uint64_t> out;
  if (!rows.empty())
  {
    out = u64(rows[0][0]);
  }
  if (current)
  {
    d.replaced.put(id, out);
  }
  return out;
}

std::vector<std::int64_t> CatalogReader::run_offsets(
  const CatalogTables& t,
  std::uint64_t run_id)
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const bool current = t.generation == d.tables->generation;
  if (current)
  {
    if (const auto* v = d.offsets.get(run_id))
    {
      ++d.stats.hits;
      return *v;
    }
  }
  ++d.stats.queries;
  std::vector<std::int64_t> out;
  for (const auto& r : d.with_db(
         [&](Catalog& db)
         {
           return db.query(
             "SELECT DISTINCT offset_end_s FROM grib_message WHERE run_id = " +
             std::to_string(run_id) + " ORDER BY offset_end_s");
         }))
  {
    out.push_back(i64(r[0]));
  }
  if (current)
  {
    d.offsets.put(run_id, out);
  }
  return out;
}

std::vector<CatMessage> CatalogReader::messages_at(
  const CatalogTables& t,
  std::uint64_t run_id,
  std::int64_t offset_end_s)
{
  Impl& d = *impl_;
  std::lock_guard<std::mutex> lk(d.mu);
  const bool current = t.generation == d.tables->generation;
  const auto key = std::make_pair(run_id, offset_end_s);
  if (current)
  {
    if (const auto* v = d.at_offset.get(key))
    {
      ++d.stats.hits;
      return *v;
    }
  }
  ++d.stats.queries;
  std::vector<CatMessage> out;
  for (const auto& r : d.with_db(
         [&](Catalog& db)
         {
           return db.query(
             std::string(kMessageColumns) +
             "WHERE m.run_id = " + std::to_string(run_id) +
             " AND m.offset_end_s = " + std::to_string(offset_end_s) +
             " ORDER BY m.id");
         }))
  {
    out.push_back(message_row(r));
  }
  if (current)
  {
    d.at_offset.put(key, out);
  }
  return out;
}

CatalogReader::Stats CatalogReader::stats() const
{
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->stats;
}

}  // namespace atmo
