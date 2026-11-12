#include "fake_catalog.h"

#include <algorithm>
#include <map>
#include <set>

namespace atmo
{

FakeCatalog::FakeCatalog(const CatalogFile& f)
{
  auto t = std::make_shared<CatalogTables>();
  t->generation = 7;
  std::map<GridKey, std::uint64_t> grid_ids;
  for (const auto& g : f.grids)
  {
    CatGrid cg;
    cg.id = t->grids.size() + 1;
    cg.key = g.key;
    cg.ni = g.ni;
    cg.nj = g.nj;
    cg.viewable = g.viewable;
    cg.periodic_i = g.periodic_i;
    cg.west = g.west;
    cg.south = g.south;
    cg.east = g.east;
    cg.north = g.north;
    t->grids.push_back(cg);
    grid_ids[g.key] = cg.id;
    t->model_grids.push_back(
      {cg.id, 1, cg.id, g.name, catalog_api_name("GFS", g.name)});
    t->runs.push_back({cg.id, cg.id, 0, -1, std::nullopt});
  }
  t->centers.push_back({1, 7, "NCEP", {{1, 0, "ops"}}});
  t->models.push_back({1, 1, 1, 7, 0, 96, "7-0-96", "GFS"});
  std::map<std::uint64_t, std::uint64_t> sig_ids;
  for (const auto& m : f.messages)
  {
    const auto& s = m.header.sig;
    if (!sig_ids.count(s.hash))
    {
      CatSignature c;
      c.id = t->signatures.size() + 1;
      c.hash = s.hash;
      c.base_hash = s.base_hash;
      c.stat_interval_s = s.stat_interval_s;
      c.discipline = s.discipline;
      c.category = s.category;
      c.number = s.number;
      c.pdt = s.pdt;
      c.canonical = s.canonical;
      c.status = s.identified() ? "identified" : "unknown";
      c.name = catalog_parameter_name(s);
      c.short_name = "";  // as the catalog writes it (Q17b)
      c.units = catalog_parameter_units(s);
      t->signatures.push_back(c);
      sig_ids[s.hash] = c.id;
    }
    CatMessage cm;
    cm.id = 100 + msgs_.size();
    const std::uint64_t gid = grid_ids.at(m.grid);
    cm.run_id = gid;
    cm.signature_id = sig_ids[s.hash];
    if (m.flags == 0)
    {
      cm.grid_id = gid;
    }
    cm.file_path = f.path;
    cm.level1 = m.header.level1;
    cm.level2 = m.header.level2;
    cm.offset_start_s = m.header.offset_start_s;
    cm.offset_end_s = m.header.offset_end_s;
    cm.valid_time_s = m.header.valid_time_s;
    cm.byte_offset = m.byte_offset;
    cm.byte_length = m.byte_length;
    cm.content_hash = m.content_hash;
    cm.flags = m.flags;
    cm.process_id = m.header.process;
    msgs_.push_back(cm);
  }
  // Run base time from the messages.
  for (auto& r : t->runs)
  {
    r.basetime_s = f.messages[0].header.base_time_s;
  }
  t->sort();
  t_ = t;
}
std::shared_ptr<const CatalogTables> FakeCatalog::tables()
{
  return t_;
}
std::vector<std::uint64_t> FakeCatalog::run_signatures(
  const CatalogTables&,
  std::uint64_t run)
{
  std::vector<std::uint64_t> out;
  for (const auto& m : msgs_)
  {
    if (
      m.run_id == run &&
      std::find(out.begin(), out.end(), m.signature_id) == out.end())
    {
      out.push_back(m.signature_id);
    }
  }
  return out;
}
std::vector<CatMessage> FakeCatalog::messages(
  const CatalogTables&,
  std::uint64_t run,
  std::uint64_t sig)
{
  std::vector<CatMessage> out;
  for (const auto& m : msgs_)
  {
    if (m.run_id == run && m.signature_id == sig)
    {
      out.push_back(m);
    }
  }
  return out;
}
std::optional<CatMessage> FakeCatalog::message(
  const CatalogTables&,
  std::uint64_t id)
{
  for (const auto& m : msgs_)
  {
    if (m.id == id)
    {
      return m;
    }
  }
  return std::nullopt;
}
std::vector<std::int64_t> FakeCatalog::run_offsets(
  const CatalogTables&,
  std::uint64_t run)
{
  std::set<std::int64_t> out;
  for (const auto& m : msgs_)
  {
    if (m.run_id == run)
    {
      out.insert(m.offset_end_s);
    }
  }
  return {out.begin(), out.end()};
}
std::vector<CatMessage> FakeCatalog::messages_at(
  const CatalogTables&,
  std::uint64_t run,
  std::int64_t off)
{
  std::vector<CatMessage> out;
  for (const auto& m : msgs_)
  {
    if (m.run_id == run && m.offset_end_s == off)
    {
      out.push_back(m);
    }
  }
  return out;
}
std::optional<std::uint64_t> FakeCatalog::replaced_by(
  const CatalogTables&,
  std::uint64_t id)
{
  auto it = replaced.find(id);
  if (it == replaced.end())
  {
    return std::nullopt;
  }
  return it->second;
}

CatalogTables& FakeCatalog::edit()
{
  t_ = std::make_shared<CatalogTables>(*t_);  // as a reload would
  ++t_->generation;
  return *t_;
}
CatSignature& FakeCatalog::sig(std::uint64_t hash)
{
  return const_cast<CatSignature&>(*t_->signature_by_hash(hash));
}

}  // namespace atmo
