// atmo_grib_ingest: decode GRIB2 files of one model run + forecast hour
// into the store and publish them (T11).  A development tool until the
// ingest daemon exists.
//
//   atmo_grib_ingest --root /atmotensor --model hrrr [--var NAME]...
//                    [--seed-sources] FILE...
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "atmo/grib.h"
#include "atmo/manifest.h"
#include "atmo/refdata.h"
#include "atmo/source.h"
#include "atmo/variable.h"

namespace fs = std::filesystem;

namespace
{

int usage()
{
  std::fprintf(
    stderr,
    "usage: atmo_grib_ingest --root DIR --model NAME [--var NAME]...\n"
    "                        [--seed-sources] FILE...\n"
    "  Decodes GRIB2 files of one run + forecast hour into DIR and\n"
    "  publishes a new manifest.  --var limits ingest to registry\n"
    "  variables (default: all); --seed-sources writes the built-in source\n"
    "  classes to DIR/config/sources.json if it does not exist yet.\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv)
{
  fs::path root;
  atmo::GribIngestOptions opt;
  bool seed = false;
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
    else if (!a.empty() && a[0] == '-')
    {
      return usage();
    }
    else
    {
      files.push_back(a);
    }
  }
  if (root.empty() || opt.model.empty() || files.empty())
  {
    return usage();
  }

  try
  {
    const fs::path sources = root / "config" / "sources.json";
    if (seed && !fs::exists(sources))
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
    if (pub.current().sources.size() == 0)
    {
      std::fprintf(
        stderr,
        "no source classes published yet: export %s first "
        "(or pass --seed-sources)\n",
        sources.c_str());
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
        "  skipped %s #%zu: %s\n", s.file.c_str(), s.message, s.reason.c_str());
    }
    return 0;
  }
  catch (const std::exception& e)
  {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
