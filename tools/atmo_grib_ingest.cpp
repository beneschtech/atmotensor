// atmo_grib_ingest: put GRIB2 files into the store.  A development tool
// until the ingest daemon exists.
//
//   atmo_grib_ingest --root DIR --model NAME --catalog CONFIG FILE...
//
// (T16, D13) keeps each file as published under DIR/grib/ and writes every
// message to the grid catalog in MySQL (CONFIG = connection file,
// catalog.h), one transaction per file.
//
//   atmo_grib_ingest --catalog CONFIG --realias
//
// applies the web app's model aliases to what is already catalogued (Q18a,
// Q21d; Catalog::realias), without reading files.  Run it after changing
// grib_model_alias.
//
//   atmo_grib_ingest --catalog CONFIG --root DIR --expire [--keep K]
//                    [--keep-model API_NAME=K]... [--model NAME FILE...]
//
// catalog retention (T18, D1): newest K base times per model grid (default
// 4), older runs deleted, then their files.  With files, they are
// ingested first.  K counts a run that is still arriving: keep one more
// than the complete runs wanted.
//
//   atmo_grib_ingest --root DIR --model NAME --tiles [--var NAME]... FILE...
//
// converts the files of ONE run + forecast hour into a float32 field file
// (T7) and publishes it in a new manifest.
//
// --seed-sources (with --root, alone or with any of the above) writes the
// built-in source classes to DIR/config/sources.json if it does not exist
// yet and publishes them, so the query daemon has a snapshot.
//
// The T10c default (index sidecars + manifest "gribs") was removed in T18.
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "atmo/catalog.h"
#include "atmo/grib.h"
#include "atmo/manifest.h"
#include "atmo/refdata.h"
#include "atmo/retention.h"
#include "atmo/source.h"
#include "atmo/variable.h"

namespace fs = std::filesystem;

namespace
{

int usage()
{
  std::fprintf(
    stderr,
    "usage: atmo_grib_ingest --root DIR --model NAME --catalog CONFIG "
    "FILE...\n"
    "       atmo_grib_ingest --catalog CONFIG --realias\n"
    "       atmo_grib_ingest --catalog CONFIG --root DIR --expire [--keep K]\n"
    "                        [--keep-model API_NAME=K]... [--model NAME "
    "FILE...]\n"
    "       atmo_grib_ingest --root DIR --model NAME --tiles [--var NAME]...\n"
    "                        FILE...\n"
    "       atmo_grib_ingest --root DIR --seed-sources\n"
    "  --catalog: keep each file in DIR/grib and catalogue every message\n"
    "  (MySQL connection file CONFIG).  --realias: move catalogued runs per\n"
    "  the current model aliases.  --expire: keep the newest K (default 4)\n"
    "  base times per model grid, delete older runs and their files.\n"
    "  --tiles: convert one run + hour to a field file and publish it.\n"
    "  --seed-sources: write the built-in source classes to\n"
    "  DIR/config/sources.json if missing and publish them.\n");
  return 2;
}

bool parse_count(const std::string& s, std::size_t& out)
{
  if (
    s.empty() || s.size() > 9 ||
    s.find_first_not_of("0123456789") != std::string::npos)
  {
    return false;
  }
  out = static_cast<std::size_t>(std::stoul(s));
  return true;
}

}  // namespace

int main(int argc, char** argv)
{
  fs::path root;
  atmo::GribIngestOptions opt;
  bool seed = false, tiles = false, realias = false, expire = false;
  fs::path catalog;
  atmo::RetentionPolicy policy;
  std::vector<fs::path> files;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--root" && i + 1 < argc)
    {
      root = argv[++i];
    }
    else if (a == "--model" && i + 1 < argc)
    {
      opt.model = argv[++i];
    }
    else if (a == "--var" && i + 1 < argc)
    {
      const atmo::VariableInfo* v = atmo::find_variable(std::string(argv[++i]));
      if (!v)
      {
        std::fprintf(stderr, "unknown variable '%s'\n", argv[i]);
        return 2;
      }
      opt.variables.insert(v->code);
    }
    else if (a == "--seed-sources")
    {
      seed = true;
    }
    else if (a == "--tiles")
    {
      tiles = true;
    }
    else if (a == "--catalog" && i + 1 < argc)
    {
      catalog = argv[++i];
    }
    else if (a == "--realias")
    {
      realias = true;
    }
    else if (a == "--expire")
    {
      expire = true;
    }
    else if (a == "--keep" && i + 1 < argc)
    {
      if (!parse_count(argv[++i], policy.runs_per_model))
      {
        return usage();
      }
    }
    else if (a == "--keep-model" && i + 1 < argc)
    {
      const std::string kv = argv[++i];
      const auto eq = kv.find('=');
      std::size_t k = 0;
      if (
        eq == std::string::npos || eq == 0 ||
        !parse_count(kv.substr(eq + 1), k))
      {
        return usage();
      }
      policy.runs_by_model[kv.substr(0, eq)] = k;
    }
    else if (!a.empty() && a[0] == '-')
    {
      return usage();
    }
    else
    {
      files.push_back(a);
    }
  }
  // --expire with files: ingest them, then expire (one catalog mode).
  const bool ingest =
    !catalog.empty() && !realias && (!expire || !files.empty());
  const bool expire_only = expire && files.empty();
  const int modes = (realias ? 1 : 0) + (expire_only ? 1 : 0) +
                    (tiles ? 1 : 0) + (ingest ? 1 : 0);
  if (
    modes > 1 || (modes == 0 && !seed) ||
    ((realias || expire) && catalog.empty()))
  {
    return usage();
  }

  try
  {
    if (seed)
    {
      if (root.empty())
      {
        return usage();
      }
      const fs::path sources = root / "config" / "sources.json";
      if (!fs::exists(sources))
      {
        fs::create_directories(sources.parent_path());
        std::ofstream(sources)
          << atmo::SourceRegistry::defaults().to_json().dump(1) << "\n";
        std::printf("seeded %s\n", sources.c_str());
      }
      atmo::ManifestPublisher pub(root);
      atmo::RefDataImporter imp(pub);
      for (const auto& e : imp.poll().errors)
      {
        std::fprintf(stderr, "config: %s\n", e.c_str());
      }
      std::printf(
        "manifest seq %llu\n",
        static_cast<unsigned long long>(pub.current().seq));
      if (modes == 0)
      {
        return 0;
      }
    }

    if (tiles)
    {
      if (root.empty() || opt.model.empty() || files.empty())
      {
        return usage();
      }
      atmo::ManifestPublisher pub(root);
      atmo::RefDataImporter imp(pub);
      for (const auto& e : imp.poll().errors)
      {
        std::fprintf(stderr, "config: %s\n", e.c_str());
      }
      if (pub.current().sources.size() == 0)
      {
        std::fprintf(
          stderr,
          "no source classes published yet: export %s first "
          "(or pass --seed-sources)\n",
          (root / "config" / "sources.json").c_str());
        return 1;
      }
      const atmo::GribIngestResult r = atmo::ingest_grib(root, files, opt);
      atmo::Manifest m = pub.current();
      atmo::add_to_manifest(m, r);
      const atmo::Manifest& out = pub.publish(m);
      std::printf(
        "%s: %zu fields from %zu messages, grid %s, manifest seq %llu\n",
        r.field.path.c_str(), r.fields, r.messages,
        atmo::key_hex(r.grid.key).c_str(),
        static_cast<unsigned long long>(out.seq));
      for (const auto& s : r.skipped)
      {
        std::printf(
          "  skipped %s #%zu: %s\n", s.file.c_str(), s.message,
          s.reason.c_str());
      }
      return 0;
    }

    // The catalog modes.
    if (!opt.variables.empty())
    {
      std::fprintf(stderr, "--catalog takes every message: no --var\n");
      return 2;
    }
#ifdef ATMO_HAVE_MYSQL
    atmo::Catalog db(atmo::CatalogConfig::from_file(catalog));
    if (realias)
    {
      if (!files.empty() || !opt.model.empty())
      {
        return usage();
      }
      const atmo::CatalogRealiasStats st = db.realias();
      std::printf(
        "%zu message(s) moved, %zu older duplicate(s) dropped, removed "
        "%zu run(s), %zu model grid(s), %zu model(s); generation %llu\n",
        st.moved_messages, st.dropped_duplicates, st.removed_runs,
        st.removed_model_grids, st.removed_models,
        static_cast<unsigned long long>(st.generation));
      return 0;
    }
    auto run_expire = [&]
    {
      const atmo::CatalogExpireStats st = db.expire(policy, root);
      std::printf(
        "%zu run(s) expired (%zu messages), %zu file row(s) deleted, %zu "
        "file(s) unlinked; generation %llu\n",
        st.runs, st.messages, st.files, st.unlinked,
        static_cast<unsigned long long>(st.generation));
    };
    if (expire_only)
    {
      if (root.empty() || !opt.model.empty())
      {
        return usage();
      }
      run_expire();
      return 0;
    }
    if (root.empty() || opt.model.empty() || files.empty())
    {
      return usage();
    }
    for (const auto& f : files)
    {
      const atmo::CatalogFile cf = atmo::catalog_grib(root, f, opt.model);
      const atmo::CatalogIngestStats st = db.ingest(cf);
      std::size_t flagged = 0;
      for (const auto& m : cf.messages)
      {
        flagged += m.flags != 0;
      }
      std::printf(
        "%s: %zu of %u messages catalogued (%zu flagged), %zu grid(s), "
        "%zu new signature(s) (%zu unknown), %zu stale row(s) removed, "
        "generation %llu\n",
        cf.path.c_str(), st.messages, cf.messages_total, flagged,
        cf.grids.size(), st.new_signatures, st.unknown_signatures,
        st.removed_messages, static_cast<unsigned long long>(st.generation));
      for (const auto& s : cf.skipped)
      {
        std::printf("  skipped #%zu: %s\n", s.message, s.reason.c_str());
      }
    }
    if (expire)
    {
      run_expire();  // after the new files, so they count as the newest
    }
    return 0;
#else
    std::fprintf(stderr, "built without the grid catalog (ATMO_WITH_MYSQL)\n");
    return 2;
#endif
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
