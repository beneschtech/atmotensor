// In-memory CatalogQuery over one catalogued file (tests only): ids are
// assigned in file order, the way the catalog would; one model "7-0-96"
// (name GFS), one model grid per grid (api_name "gfs_<grid name>"), one
// run per grid.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "atmo/catalog.h"
#include "atmo/grib_api.h"

namespace atmo
{

class FakeCatalog : public CatalogQuery
{
 public:
  explicit FakeCatalog(const CatalogFile& f);

  std::shared_ptr<const CatalogTables> tables() override;
  std::vector<std::uint64_t> run_signatures(
    const CatalogTables& t,
    std::uint64_t run) override;
  std::vector<CatMessage> messages(
    const CatalogTables& t,
    std::uint64_t run,
    std::uint64_t sig) override;
  std::optional<CatMessage> message(const CatalogTables& t, std::uint64_t id)
    override;
  std::vector<std::int64_t> run_offsets(
    const CatalogTables& t,
    std::uint64_t run) override;
  std::vector<CatMessage> messages_at(
    const CatalogTables& t,
    std::uint64_t run,
    std::int64_t off) override;

  std::optional<std::uint64_t> replaced_by(
    const CatalogTables& t,
    std::uint64_t id) override;
  // old id -> new id, for replaced_by().
  std::map<std::uint64_t, std::uint64_t> replaced;

  // Edit the tables as the web app would: a copy with the next generation
  // (as a reload would make).
  CatalogTables& edit();
  CatSignature& sig(std::uint64_t hash);

 private:
  std::shared_ptr<CatalogTables> t_;
  std::vector<CatMessage> msgs_;
};

}  // namespace atmo
