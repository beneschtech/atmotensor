// atmo_grib_sigcheck: validates the signature deny list on real files
// (T15, 07-grib-catalog.md "Parameter identity").
//
//   atmo_grib_sigcheck [--all] FILE...
//
// Give it several runs and forecast hours of a model.  Files are grouped
// by (centre, subcentre, section 3 bytes), i.e. one model grid.  Within a
// group, every (parameter, template, level) that occurs in more than one
// file should have one signature; a signature that appears in only some of
// the files where its parameter and level occur is reported with the keys
// that differ from the most common one.  Differences only in statInterval
// are expected (1 h / 3 h / run-total siblings) and are summarised unless
// --all is given.  Messages that would collide in the catalog (same run,
// member, signature, level and offsets within one file) are reported too.
//
// Exit status: 0 nothing unexpected, 1 instability or collisions found,
// 2 usage or I/O error.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "atmo/gribsig.h"
#include "atmo/grid.h"

using namespace atmo;

namespace
{

struct Mapped
{
  const unsigned char* p = nullptr;
  std::size_t n = 0;
  explicit Mapped(const std::string& path)
  {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
      throw std::runtime_error(path + ": " + std::strerror(errno));
    }
    struct stat st{};
    ::fstat(fd, &st);
    n = static_cast<std::size_t>(st.st_size);
    if (n > 0)
    {
      void* m = ::mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
      if (m == MAP_FAILED)
      {
        ::close(fd);
        throw std::runtime_error(path + ": mmap failed");
      }
      p = static_cast<const unsigned char*>(m);
    }
    ::close(fd);
  }
  ~Mapped()
  {
    if (p)
    {
      ::munmap(const_cast<unsigned char*>(p), n);
    }
  }
};

// FNV-1a of section 3 after its length/number (enough to group files).
std::uint64_t grid_hash(const unsigned char* m, std::size_t len)
{
  std::size_t i = 16;
  while (i + 5 <= len && std::memcmp(m + i, "7777", 4) != 0)
  {
    const std::size_t sl = (std::size_t(m[i]) << 24) |
                           (std::size_t(m[i + 1]) << 16) |
                           (std::size_t(m[i + 2]) << 8) | m[i + 3];
    if (sl < 5 || i + sl > len)
    {
      break;
    }
    if (m[i + 4] == 3)
    {
      return fnv1a64(m + i + 5, sl - 5);
    }
    i += sl;
  }
  return 0;
}

std::string iso(std::int64_t t)
{
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
  ::gmtime_r(&tt, &tm);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%dT%H:%MZ", &tm);
  return b;
}

std::string level_text(const GribLevel& a, const GribLevel& b)
{
  char buf[96];
  if (b.type == 255)
  {
    std::snprintf(buf, sizeof buf, "%u:%g", a.type, a.value);
  }
  else
  {
    std::snprintf(
      buf, sizeof buf, "%u:%g-%u:%g", a.type, a.value, b.type, b.value);
  }
  return buf;
}

struct Seen
{
  std::size_t file = 0;
  std::uint64_t group = 0;
  GribHeader h;
};

}  // namespace

int main(int argc, char** argv)
{
  bool all = false;
  std::vector<std::string> files;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--all")
    {
      all = true;
    }
    else if (a == "-h" || a == "--help")
    {
      std::puts("usage: atmo_grib_sigcheck [--all] FILE...");
      return 0;
    }
    else
    {
      files.push_back(a);
    }
  }
  if (files.size() < 1)
  {
    std::fputs("usage: atmo_grib_sigcheck [--all] FILE...\n", stderr);
    return 2;
  }

  std::vector<Seen> seen;
  std::map<std::uint64_t, std::string> canon;  // hash -> canonical
  bool bad = false;
  std::printf(
    "signature version %d, %zu deny rules\n\n", kSignatureVersion,
    signature_deny_list().size());
  std::puts("== files");
  for (std::size_t f = 0; f < files.size(); ++f)
  {
    try
    {
      Mapped m(files[f]);
      const auto spans = grib_message_spans(m.p, m.n);
      std::size_t ok = 0, other = 0, undecodable = 0;
      std::set<std::uint64_t> sigs;
      std::set<std::uint32_t> procs;
      std::set<std::int64_t> bases;
      std::int64_t omin = INT64_MAX, omax = INT64_MIN;
      for (const auto& s : spans)
      {
        if (s.edition != 2)
        {
          ++other;
          continue;
        }
        try
        {
          Seen x;
          x.file = f;
          x.h = describe_grib(m.p + s.offset, s.length);
          x.group = fnv1a64(
            &x.h.sig.centre, sizeof x.h.sig.centre,
            fnv1a64(
              &x.h.subcentre, sizeof x.h.subcentre,
              grid_hash(m.p + s.offset, s.length)));
          auto [it, fresh] = canon.emplace(x.h.sig.hash, x.h.sig.canonical);
          if (!fresh && it->second != x.h.sig.canonical)
          {
            std::printf("HASH COLLISION %s\n", key_hex(x.h.sig.hash).c_str());
            bad = true;
          }
          sigs.insert(x.h.sig.hash);
          procs.insert(x.h.process);
          bases.insert(x.h.base_time_s);
          omin = std::min(omin, x.h.offset_end_s);
          omax = std::max(omax, x.h.offset_end_s);
          undecodable += x.h.sig.undecodable;
          seen.push_back(std::move(x));
          ++ok;
        }
        catch (const std::exception& e)
        {
          std::printf(
            "  %s @%llu: %s\n", files[f].c_str(),
            static_cast<unsigned long long>(s.offset), e.what());
          ++other;
        }
      }
      std::string pr, bt;
      for (auto p : procs)
      {
        pr += (pr.empty() ? "" : ",") + std::to_string(p);
      }
      for (auto b : bases)
      {
        bt += (bt.empty() ? "" : ",") + iso(b);
      }
      std::printf(
        "[%zu] %s\n    %zu messages (%zu not GRIB2/unreadable, %zu "
        "undecodable), %zu signatures, process %s, base %s, offsets "
        "%lld..%lld h\n",
        f, files[f].c_str(), ok + other, other, undecodable, sigs.size(),
        pr.c_str(), bt.c_str(), static_cast<long long>(omin / 3600),
        static_cast<long long>(omax / 3600));
    }
    catch (const std::exception& e)
    {
      std::fprintf(stderr, "%s: %s\n", files[f].c_str(), e.what());
      return 2;
    }
  }

  // Collisions inside one file: the catalog's unique key would collapse
  // them (latest wins).
  std::puts("\n== catalog key collisions within a file");
  {
    std::map<
      std::tuple<
        std::size_t, std::uint64_t, std::uint32_t, std::int64_t, int,
        std::uint64_t, std::string, std::int64_t, std::int64_t>,
      std::vector<std::size_t>>
      keys;
    for (std::size_t i = 0; i < seen.size(); ++i)
    {
      const auto& h = seen[i].h;
      keys[{seen[i].file, seen[i].group, h.process, h.base_time_s, h.member,
            h.sig.hash, level_text(h.level1, h.level2), h.offset_start_s,
            h.offset_end_s}]
        .push_back(i);
    }
    std::size_t n = 0;
    for (const auto& [k, v] : keys)
    {
      if (v.size() < 2)
      {
        continue;
      }
      ++n;
      const auto& h = seen[v[0]].h;
      std::printf(
        "  [%zu] %s (%u/%u/%u pdt %u) level %s offset %lld-%lld s:"
        " %zu messages\n",
        std::get<0>(k), h.sig.ecc_short_name.c_str(), h.sig.discipline,
        h.sig.category, h.sig.number, h.sig.pdt, std::get<6>(k).c_str(),
        static_cast<long long>(h.offset_start_s),
        static_cast<long long>(h.offset_end_s), v.size());
    }
    std::printf("  %zu colliding keys\n", n);
    bad = bad || n > 0;
  }

  // Stability across files of one model grid.
  std::puts(
    "\n== signatures not present in every file of their "
    "parameter/level");
  {
    using Coarse = std::tuple<
      std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
      std::string>;
    // coarse -> signature -> files
    std::map<Coarse, std::map<std::uint64_t, std::set<std::size_t>>> g;
    std::map<Coarse, std::string> names;
    std::map<std::uint64_t, std::set<std::size_t>> group_files;
    for (const auto& x : seen)
    {
      const auto& h = x.h;
      if (h.sig.undecodable)
      {
        continue;
      }
      Coarse c{x.group,      h.sig.discipline, h.sig.category,
               h.sig.number, h.sig.pdt,        level_text(h.level1, h.level2)};
      g[c][h.sig.hash].insert(x.file);
      names[c] = h.sig.ecc_short_name;
      group_files[x.group].insert(x.file);
    }
    std::size_t unexpected = 0, siblings = 0;
    for (const auto& [c, sigs] : g)
    {
      std::set<std::size_t> where;
      for (const auto& [s, fs] : sigs)
      {
        where.insert(fs.begin(), fs.end());
      }
      if (where.size() < 2 || sigs.size() < 2)
      {
        continue;
      }
      // Most common signature = reference.
      std::uint64_t ref = 0;
      std::size_t best = 0;
      for (const auto& [s, fs] : sigs)
      {
        if (fs.size() > best)
        {
          best = fs.size();
          ref = s;
        }
      }
      for (const auto& [s, fs] : sigs)
      {
        if (s == ref || fs.size() == where.size())
        {
          continue;
        }
        const auto d = signature_diff(canon[ref], canon[s]);
        const bool only_interval =
          d.size() == 1 && d[0].rfind("statInterval:", 0) == 0;
        if (only_interval)
        {
          ++siblings;
          if (!all)
          {
            continue;
          }
        }
        else
        {
          ++unexpected;
        }
        std::string fl;
        for (auto f : fs)
        {
          fl += (fl.empty() ? "" : ",") + std::to_string(f);
        }
        std::printf(
          "  %s %s (%u/%u/%u pdt %u) level %s: in files [%s] of "
          "%zu\n",
          only_interval ? "sibling " : "UNSTABLE", names[c].c_str(),
          std::get<1>(c), std::get<2>(c), std::get<3>(c), std::get<4>(c),
          std::get<5>(c).c_str(), fl.c_str(), where.size());
        for (const auto& line : d)
        {
          std::printf("      %s\n", line.c_str());
        }
      }
    }
    std::printf(
      "  %zu unexpected, %zu statInterval siblings%s\n", unexpected, siblings,
      all ? "" : " (listed with --all)");
    bad = bad || unexpected > 0;
  }

  // Signatures eccodes cannot name.
  std::puts("\n== unknown to eccodes");
  {
    std::map<std::uint64_t, std::pair<const GribHeader*, std::size_t>> u;
    for (const auto& x : seen)
    {
      if (!x.h.sig.identified())
      {
        auto& e = u[x.h.sig.hash];
        e.first = &x.h;
        ++e.second;
      }
    }
    for (const auto& [hsh, e] : u)
    {
      const auto& s = e.first->sig;
      std::printf(
        "  %s %u/%u/%u pdt %u%s: %zu messages\n", key_hex(hsh).c_str(),
        s.discipline, s.category, s.number, s.pdt,
        s.undecodable ? " (undecodable)" : "", e.second);
    }
    std::printf("  %zu signatures\n", u.size());
  }
  return bad ? 1 : 0;
}
