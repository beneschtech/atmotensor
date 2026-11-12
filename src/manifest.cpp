#include "atmo/manifest.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "atmo/crc32.h"
#include "atmo/fieldfile.h"
#include "atmo/obs_segment.h"
#include "atmo/stations.h"
#include "fileio.h"
#include "jsonutil.h"

namespace atmo
{
namespace
{

namespace fs = std::filesystem;
using nlohmann::json;

constexpr const char* kWho = "manifest";
constexpr const char* kCurrent = "CURRENT";
constexpr const char* kCurrentTmp = "CURRENT.tmp";
constexpr const char* kLock = "LOCK";
constexpr const char* kCurrentMagic = "atmo-current";
constexpr int kCurrentVersion = 1;

[[noreturn]] void bad(const std::string& what)
{
  throw std::invalid_argument(std::string("manifest: ") + what);
}

[[noreturn]] void corrupt(const fs::path& p, const std::string& what)
{
  throw std::runtime_error(
    std::string("manifest: ") + p.string() + ": " + what);
}

// ---- JSON field access ------------------------------------------------

using detail::check_keys;
using detail::get_i64;
using detail::get_str;
using detail::get_u32;
using detail::get_u64;

GridKey get_key(const json& j, const char* key, std::string_view what)
{
  const std::string s = get_str(j, key, what);
  bool ok = s.size() == 16;
  for (char c : s)
  {
    ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
  if (!ok)
  {
    throw std::invalid_argument(
      std::string(what) + ": \"" + key + "\" must be 16 lowercase hex digits");
  }
  return std::stoull(s, nullptr, 16);
}

// ---- paths ------------------------------------------------------------------

// `p` is relative, has only normal components, and starts with the
// components of `prefix` followed by at least one more.
void check_path(const std::string& p, const std::string& prefix)
{
  if (p.empty() || p.front() == '/' || p.back() == '/')
  {
    bad("bad path \"" + p + "\"");
  }
  std::size_t start = 0;
  while (start <= p.size())
  {
    std::size_t end = p.find('/', start);
    if (end == std::string::npos)
    {
      end = p.size();
    }
    const std::string_view c(p.data() + start, end - start);
    if (c.empty() || c == "." || c == "..")
    {
      bad("bad path \"" + p + "\"");
    }
    start = end + 1;
  }
  if (
    p.size() <= prefix.size() + 1 || p.compare(0, prefix.size(), prefix) ||
    p[prefix.size()] != '/')
  {
    bad("path \"" + p + "\" is not under " + prefix + "/");
  }
}

std::string read_file(const fs::path& p)
{
  return detail::read_text_file(p, kWho);
}

std::int64_t now_s()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

// Parses "MANIFEST-<16 hex>.json"; nullopt for any other name.
std::optional<std::uint64_t> manifest_seq(const std::string& name)
{
  constexpr std::string_view pre = "MANIFEST-", post = ".json";
  if (
    name.size() != pre.size() + 16 + post.size() ||
    name.compare(0, pre.size(), pre) ||
    name.compare(pre.size() + 16, post.size(), post))
  {
    return std::nullopt;
  }
  const std::string hex = name.substr(pre.size(), 16);
  for (char c : hex)
  {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
    {
      return std::nullopt;
    }
  }
  return std::stoull(hex, nullptr, 16);
}

struct CurrentRecord
{
  std::uint64_t seq = 0;
  std::uint32_t crc = 0;
};

std::string format_current(const CurrentRecord& r)
{
  char buf[64];
  std::snprintf(
    buf, sizeof buf, "%s %d %" PRIu64 " %08" PRIx32 "\n", kCurrentMagic,
    kCurrentVersion, r.seq, r.crc);
  return buf;
}

std::optional<CurrentRecord> read_current(const fs::path& root)
{
  const fs::path p = root / "manifest" / kCurrent;
  std::error_code ec;
  if (!fs::exists(p, ec))
  {
    return std::nullopt;
  }
  const std::string text = read_file(p);
  char magic[16] = {};
  int version = 0;
  unsigned long long seq = 0;
  unsigned crc = 0;
  int consumed = 0;
  if (
    text.size() > 60 ||
    std::sscanf(
      text.c_str(), "%15s %d %llu %8x\n%n", magic, &version, &seq, &crc,
      &consumed) != 4 ||
    std::strcmp(magic, kCurrentMagic) != 0 || version != kCurrentVersion ||
    static_cast<std::size_t>(consumed) != text.size() ||
    format_current({seq, crc}) != text || seq == 0)
  {
    corrupt(p, "malformed");
  }
  return CurrentRecord{seq, crc};
}

}  // namespace

// ---------------------------------------------------------------------------
// Entries and Manifest
// ---------------------------------------------------------------------------

SegmentEntry SegmentEntry::from_segment(
  const fs::path& root,
  const ObsSegment& seg)
{
  const fs::path rel = seg.path().lexically_relative(root);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
  {
    bad(seg.path().string() + " is not under " + root.string());
  }
  SegmentEntry e;
  e.source = std::string(seg.source_name());
  e.bucket_start_s = seg.bucket_start_s();
  e.bucket_seconds = seg.bucket_seconds();
  e.path = rel.generic_string();
  e.receipt_seq_min = seg.receipt_seq_min();
  e.receipt_seq_max = seg.receipt_seq_max();
  e.report_count = seg.size();
  e.header_crc = seg.header_crc();
  return e;
}

FieldEntry FieldEntry::from_file(const fs::path& root, const FieldFile& file)
{
  const fs::path rel = file.path().lexically_relative(root);
  if (rel.empty() || rel.is_absolute() || *rel.begin() == "..")
  {
    bad(file.path().string() + " is not under " + root.string());
  }
  FieldEntry e;
  e.model = std::string(file.model());
  e.run_s = file.run_s();
  e.fhr = file.fhr();
  e.grid_key = file.grid_key();
  e.path = rel.generic_string();
  e.header_crc = file.header_crc();
  return e;
}

std::string Manifest::file_name(std::uint64_t seq)
{
  char buf[32];
  std::snprintf(buf, sizeof buf, "MANIFEST-%016" PRIx64 ".json", seq);
  return buf;
}

void Manifest::validate() const
{
  if (sources.size() == 0)
  {
    // A manifest without source classes could not be read back
    // (SourceRegistry::from_json requires at least one).
    bad("no source classes");
  }
  if (next_receipt_seq == 0)
  {
    bad("next_receipt_seq must be >= 1");
  }
  std::set<std::string> paths;
  auto add_path = [&](const std::string& p)
  {
    if (!paths.insert(p).second)
    {
      bad("path \"" + p + "\" listed twice");
    }
  };

  if (stations)
  {
    if (stations->generation == 0)
    {
      bad("stations: generation must be >= 1");
    }
    const std::string want =
      "stations/" + StationTable::file_name(stations->generation);
    if (stations->path != want)
    {
      bad("stations: path must be \"" + want + "\"");
    }
    add_path(stations->path);
  }

  std::set<GridKey> keys;
  for (const auto& g : grids)
  {
    const std::string want = "grids/" + key_hex(g.key) + ".grid";
    if (g.path != want)
    {
      bad("grid: path must be \"" + want + "\"");
    }
    if (!keys.insert(g.key).second)
    {
      bad("grid " + key_hex(g.key) + " listed twice");
    }
    add_path(g.path);
  }

  std::set<std::tuple<std::string, std::int64_t, std::uint32_t>> fids;
  for (const auto& f : fields)
  {
    if (!SourceClass::valid_name(f.model))
    {
      bad("field: bad model name \"" + f.model + "\"");
    }
    check_path(f.path, "fields/" + f.model);
    if (!keys.count(f.grid_key))
    {
      bad("field " + f.path + ": grid " + key_hex(f.grid_key) + " not listed");
    }
    if (!fids.insert({f.model, f.run_s, f.fhr}).second)
    {
      bad("field " + f.path + ": (model, run, fhr) listed twice");
    }
    add_path(f.path);
  }

  for (const auto& s : segments)
  {
    if (!sources.find(s.source))
    {
      bad("segment " + s.path + ": unknown source \"" + s.source + "\"");
    }
    check_path(s.path, "obs/" + s.source);
    if (s.bucket_seconds <= 0)
    {
      bad("segment " + s.path + ": bucket_seconds must be > 0");
    }
    if (s.report_count == 0)
    {
      if (s.receipt_seq_min != 0 || s.receipt_seq_max != 0)
      {
        bad("segment " + s.path + ": empty but has receipt seqs");
      }
    }
    else if (
      s.receipt_seq_min == 0 || s.receipt_seq_min > s.receipt_seq_max ||
      s.receipt_seq_max >= next_receipt_seq)
    {
      bad(
        "segment " + s.path +
        ": receipt seqs must satisfy 1 <= min <= max < next_receipt_seq");
    }
    add_path(s.path);
  }

  for (const auto& r : retired)
  {
    bool under_kind = false;
    for (const char* kind : {"grids", "stations", "fields", "grib", "obs"})
    {
      const std::string pre = std::string(kind) + "/";
      under_kind = under_kind || r.path.compare(0, pre.size(), pre) == 0;
    }
    if (!under_kind)
    {
      bad("retired path \"" + r.path + "\" is not a data file");
    }
    check_path(r.path, r.path.substr(0, r.path.find('/')));
    add_path(r.path);
  }
}

std::vector<std::string> Manifest::live_paths() const
{
  std::vector<std::string> v;
  if (stations)
  {
    v.push_back(stations->path);
  }
  for (const auto& g : grids)
  {
    v.push_back(g.path);
  }
  for (const auto& f : fields)
  {
    v.push_back(f.path);
  }
  for (const auto& e : segments)
  {
    v.push_back(e.path);
  }
  return v;
}

json Manifest::to_json() const
{
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["seq"] = seq;
  j["created_at"] = created_at_s;
  j["next_receipt_seq"] = next_receipt_seq;
  j["sources"] = sources.to_json();
  if (stations)
  {
    j["stations"] = json{
      {"generation", stations->generation},
      {"path", stations->path},
      {"header_crc", stations->header_crc}};
  }
  else
  {
    j["stations"] = nullptr;
  }
  json g = json::array();
  for (const auto& e : grids)
  {
    g.push_back(
      {{"key", key_hex(e.key)},
       {"path", e.path},
       {"header_crc", e.header_crc}});
  }
  j["grids"] = std::move(g);
  json f = json::array();
  for (const auto& e : fields)
  {
    f.push_back(
      {{"model", e.model},
       {"run", e.run_s},
       {"fhr", e.fhr},
       {"grid_key", key_hex(e.grid_key)},
       {"path", e.path},
       {"header_crc", e.header_crc}});
  }
  j["fields"] = std::move(f);
  json s = json::array();
  for (const auto& e : segments)
  {
    s.push_back(
      {{"source", e.source},
       {"bucket_start", e.bucket_start_s},
       {"bucket_seconds", e.bucket_seconds},
       {"path", e.path},
       {"receipt_seq_min", e.receipt_seq_min},
       {"receipt_seq_max", e.receipt_seq_max},
       {"report_count", e.report_count},
       {"header_crc", e.header_crc}});
  }
  j["segments"] = std::move(s);
  json r = json::array();
  for (const auto& e : retired)
  {
    r.push_back({{"path", e.path}, {"retired_at", e.retired_at_s}});
  }
  j["retired"] = std::move(r);
  return j;
}

std::string Manifest::to_text() const
{
  return to_json().dump(1, ' ') + "\n";
}

Manifest Manifest::from_json(const json& j)
{
  if (
    !j.is_object() || !j.contains("version") ||
    !j.at("version").is_number_integer() ||
    (j.at("version") != kVersion && j.at("version") != 1))
  {
    bad("unsupported \"version\" (want " + std::to_string(kVersion) + ")");
  }
  const bool v1 = j.at("version") == 1;
  if (v1)
  {
    check_keys(
      j, "manifest",
      {"format", "version", "seq", "created_at", "next_receipt_seq", "sources",
       "stations", "grids", "fields", "gribs", "segments", "retired"});
  }
  else
  {
    check_keys(
      j, "manifest",
      {"format", "version", "seq", "created_at", "next_receipt_seq", "sources",
       "stations", "grids", "fields", "segments", "retired"});
  }
  if (!j.at("format").is_string() || j.at("format") != kFormat)
  {
    bad(std::string("\"format\" must be \"") + kFormat + "\"");
  }
  Manifest m;
  m.seq = get_u64(j, "seq", "manifest");
  m.created_at_s = get_i64(j, "created_at", "manifest");
  m.next_receipt_seq = get_u64(j, "next_receipt_seq", "manifest");
  m.sources = SourceRegistry::from_json(j.at("sources"));

  const json& st = j.at("stations");
  if (!st.is_null())
  {
    check_keys(st, "manifest: stations", {"generation", "path", "header_crc"});
    StationsEntry e;
    e.generation = get_u64(st, "generation", "manifest: stations");
    e.path = get_str(st, "path", "manifest: stations");
    e.header_crc = get_u32(st, "header_crc", "manifest: stations");
    m.stations = e;
  }
  for (const auto& g : detail::get_array(j, "grids", "manifest"))
  {
    check_keys(g, "manifest: grid", {"key", "path", "header_crc"});
    GridEntry e;
    e.key = get_key(g, "key", "manifest: grid");
    e.path = get_str(g, "path", "manifest: grid");
    e.header_crc = get_u32(g, "header_crc", "manifest: grid");
    m.grids.push_back(std::move(e));
  }
  for (const auto& f : detail::get_array(j, "fields", "manifest"))
  {
    check_keys(
      f, "manifest: field",
      {"model", "run", "fhr", "grid_key", "path", "header_crc"});
    FieldEntry e;
    e.model = get_str(f, "model", "manifest: field");
    e.run_s = get_i64(f, "run", "manifest: field");
    e.fhr = get_u32(f, "fhr", "manifest: field");
    e.grid_key = get_key(f, "grid_key", "manifest: field");
    e.path = get_str(f, "path", "manifest: field");
    e.header_crc = get_u32(f, "header_crc", "manifest: field");
    m.fields.push_back(std::move(e));
  }
  for (const auto& s : detail::get_array(j, "segments", "manifest"))
  {
    check_keys(
      s, "manifest: segment",
      {"source", "bucket_start", "bucket_seconds", "path", "receipt_seq_min",
       "receipt_seq_max", "report_count", "header_crc"});
    SegmentEntry e;
    e.source = get_str(s, "source", "manifest: segment");
    e.bucket_start_s = get_i64(s, "bucket_start", "manifest: segment");
    e.bucket_seconds = get_i64(s, "bucket_seconds", "manifest: segment");
    e.path = get_str(s, "path", "manifest: segment");
    e.receipt_seq_min = get_u64(s, "receipt_seq_min", "manifest: segment");
    e.receipt_seq_max = get_u64(s, "receipt_seq_max", "manifest: segment");
    e.report_count = get_u64(s, "report_count", "manifest: segment");
    e.header_crc = get_u32(s, "header_crc", "manifest: segment");
    m.segments.push_back(std::move(e));
  }
  for (const auto& r : detail::get_array(j, "retired", "manifest"))
  {
    check_keys(r, "manifest: retired", {"path", "retired_at"});
    m.retired.push_back(
      RetiredEntry{
        get_str(r, "path", "manifest: retired"),
        get_i64(r, "retired_at", "manifest: retired")});
  }
  if (v1 && j.contains("gribs"))
  {
    // T10c GRIB entries (T18): only their index sidecars are retired.
    for (const auto& g : detail::get_array(j, "gribs", "manifest"))
    {
      const std::string idx = get_str(g, "index_path", "manifest: grib");
      const bool dup = std::any_of(
        m.retired.begin(), m.retired.end(),
        [&](const RetiredEntry& r) { return r.path == idx; });
      if (!dup)
      {
        m.retired.push_back(RetiredEntry{idx, m.created_at_s});
      }
    }
  }
  m.validate();
  return m;
}

Manifest Manifest::from_text(std::string_view text)
{
  return from_json(detail::parse_json(text, kWho));
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

std::optional<std::uint64_t> read_current_seq(const fs::path& root)
{
  const auto r = read_current(root);
  if (!r)
  {
    return std::nullopt;
  }
  return r->seq;
}

std::optional<Manifest> read_current_manifest(const fs::path& root)
{
  // A publish can replace CURRENT and prune the manifest it named between
  // our two reads; CURRENT then names a newer seq, so try again.
  for (int attempt = 0;; ++attempt)
  {
    const auto r = read_current(root);
    if (!r)
    {
      return std::nullopt;
    }
    const fs::path p = root / "manifest" / Manifest::file_name(r->seq);
    std::string text;
    try
    {
      text = read_file(p);
    }
    catch (const std::runtime_error&)
    {
      const auto again = read_current(root);
      if (attempt < 3 && again && again->seq != r->seq)
      {
        continue;
      }
      throw;
    }
    if (crc32(text.data(), text.size()) != r->crc)
    {
      corrupt(p, "checksum does not match CURRENT");
    }
    Manifest m;
    try
    {
      m = Manifest::from_text(text);
    }
    catch (const std::invalid_argument& e)
    {
      corrupt(p, e.what());
    }
    if (m.seq != r->seq)
    {
      corrupt(p, "seq does not match file name");
    }
    return m;
  }
}

Manifest read_manifest(const fs::path& root, std::uint64_t seq)
{
  const fs::path p = root / "manifest" / Manifest::file_name(seq);
  Manifest m;
  try
  {
    m = Manifest::from_text(read_file(p));
  }
  catch (const std::invalid_argument& e)
  {
    corrupt(p, e.what());
  }
  if (m.seq != seq)
  {
    corrupt(p, "seq does not match file name");
  }
  return m;
}

// ---------------------------------------------------------------------------
// ManifestPublisher
// ---------------------------------------------------------------------------

ManifestPublisher::ManifestPublisher(fs::path root, std::size_t keep)
    : root_(std::move(root)), dir_(root_ / "manifest"), keep_(keep)
{
  if (keep_ == 0)
  {
    bad("keep must be >= 1");
  }
  fs::create_directories(dir_);
  const fs::path lock = dir_ / kLock;
  lock_fd_ = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (lock_fd_ < 0)
  {
    corrupt(lock, std::string("cannot open: ") + std::strerror(errno));
  }
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
  {
    const int err = errno;
    ::close(lock_fd_);
    lock_fd_ = -1;
    corrupt(
      lock, err == EWOULDBLOCK ? "held by another publisher"
                               : std::string("flock: ") + std::strerror(err));
  }
  try
  {
    recover();
  }
  catch (...)
  {
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw;
  }
}

ManifestPublisher::~ManifestPublisher()
{
  if (lock_fd_ >= 0)
  {
    ::close(lock_fd_);  // releases the flock
  }
}

void ManifestPublisher::recover()
{
  if (auto m = read_current_manifest(root_))
  {
    current_ = std::move(*m);
  }
  // Anything above CURRENT was never published: no reader can depend on it,
  // and leaving it would make the next publish collide with it.
  bool removed = false;
  for (const auto& e : fs::directory_iterator(dir_))
  {
    const std::string name = e.path().filename().string();
    const auto s = manifest_seq(name);
    if (
      (s && *s > current_.seq) || name == kCurrentTmp ||
      name.find(".tmp.") != std::string::npos)
    {
      fs::remove(e.path());
      removed = true;
    }
  }
  if (removed)
  {
    detail::fsync_dir(dir_, kWho);
  }
}

void ManifestPublisher::set_fault_hook(std::function<void(PublishStep)> hook)
{
  hook_ = std::move(hook);
}

void ManifestPublisher::fault(PublishStep s) const
{
  if (hook_)
  {
    hook_(s);
  }
}

void ManifestPublisher::set_clock(std::function<std::int64_t()> clock)
{
  clock_ = std::move(clock);
}

std::int64_t ManifestPublisher::now() const
{
  return clock_ ? clock_() : now_s();
}

const Manifest& ManifestPublisher::publish(Manifest next)
{
  const std::int64_t t = now();
  std::set<std::string> live;
  for (auto& p : next.live_paths())
  {
    live.insert(std::move(p));
  }
  std::vector<RetiredEntry> retired;
  std::set<std::string> seen;
  for (const auto& r : current_.retired)
  {
    if (!live.count(r.path) && seen.insert(r.path).second)
    {
      retired.push_back(r);
    }
  }
  for (auto& p : current_.live_paths())
  {
    if (!live.count(p) && seen.insert(p).second)
    {
      retired.push_back(RetiredEntry{std::move(p), t});
    }
  }
  return publish_impl(std::move(next), std::move(retired));
}

const Manifest& ManifestPublisher::publish_impl(
  Manifest next,
  std::vector<RetiredEntry> retired)
{
  next.seq = current_.seq + 1;
  next.created_at_s = now();
  next.retired = std::move(retired);
  next.validate();
  if (next.next_receipt_seq < current_.next_receipt_seq)
  {
    bad("next_receipt_seq must not go backwards");
  }
  if (
    current_.stations && (!next.stations || next.stations->generation <
                                              current_.stations->generation))
  {
    bad("station generation must not go backwards");
  }
  auto must_exist = [&](const std::string& p)
  {
    std::error_code ec;
    if (!fs::is_regular_file(root_ / p, ec))
    {
      bad("file " + p + " does not exist");
    }
  };
  if (next.stations)
  {
    must_exist(next.stations->path);
  }
  for (const auto& g : next.grids)
  {
    must_exist(g.path);
  }
  for (const auto& f : next.fields)
  {
    must_exist(f.path);
  }
  for (const auto& s : next.segments)
  {
    must_exist(s.path);
  }

  // Step 2: the manifest itself.
  const std::string text = next.to_text();
  detail::publish_file(
    dir_ / Manifest::file_name(next.seq),
    std::vector<unsigned char>(text.begin(), text.end()), kWho);
  fault(PublishStep::manifest_written);

  // Step 3: CURRENT.tmp -> fsync -> rename -> fsync dir.
  const std::string cur =
    format_current({next.seq, crc32(text.data(), text.size())});
  const fs::path tmp = dir_ / kCurrentTmp;
  const int fd =
    ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
  {
    corrupt(tmp, std::string("cannot create: ") + std::strerror(errno));
  }
  try
  {
    detail::write_all(fd, cur.data(), cur.size(), 0, kWho);
    if (::fsync(fd) != 0)
    {
      corrupt(tmp, "fsync failed");
    }
  }
  catch (...)
  {
    ::close(fd);
    throw;
  }
  ::close(fd);
  fault(PublishStep::current_written);

  if (::rename(tmp.c_str(), (dir_ / kCurrent).c_str()) != 0)
  {
    corrupt(tmp, std::string("rename failed: ") + std::strerror(errno));
  }
  // From here on the new manifest is the published state.
  current_ = std::move(next);
  fault(PublishStep::current_renamed);
  detail::fsync_dir(dir_, kWho);

  prune();
  return current_;
}

GcReport ManifestPublisher::collect_garbage(std::int64_t grace_s)
{
  GcReport rep;
  const std::int64_t t = now();
  auto unlink = [](const fs::path& p) -> bool
  {
    if (::unlink(p.c_str()) == 0)
    {
      return true;
    }
    if (errno == ENOENT)
    {
      return false;
    }
    corrupt(p, std::string("unlink failed: ") + std::strerror(errno));
  };

  // 1. Retired files whose grace period is over: drop them from the
  //    manifest first, so no published state lists a file that is gone.
  std::vector<RetiredEntry> keep, due;
  for (const auto& r : current_.retired)
  {
    (t - r.retired_at_s >= grace_s ? due : keep).push_back(r);
  }
  if (!due.empty())
  {
    rep.published_seq = publish_impl(current_, std::move(keep)).seq;
    for (const auto& d : due)
    {
      rep.retired_unlinked += unlink(root_ / d.path) ? 1 : 0;
    }
  }

  // 2. Orphans: named by nothing, old enough that no publish in progress
  //    can be about to name them.
  std::set<std::string> named;
  for (auto& p : current_.live_paths())
  {
    named.insert(std::move(p));
  }
  for (const auto& r : current_.retired)
  {
    named.insert(r.path);
  }
  // Not grids/ or grib/: the grid catalog (D13) keeps files there that no
  // manifest names; Catalog::expire() removes its own (T18).
  for (const char* kind : {"stations", "fields", "obs", "tmp"})
  {
    const fs::path top = root_ / kind;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(top, ec)))
    {
      continue;
    }
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(top);
         it != fs::recursive_directory_iterator(); ++it)
    {
      const fs::file_status st = it->symlink_status();
      if (fs::is_directory(st))
      {
        dirs.push_back(it->path());
        continue;
      }
      const std::string rel =
        it->path().lexically_relative(root_).generic_string();
      if (named.count(rel))
      {
        continue;
      }
      struct stat sb;
      if (::lstat(it->path().c_str(), &sb) != 0)
      {
        continue;  // vanished meanwhile
      }
      if (static_cast<std::int64_t>(sb.st_mtim.tv_sec) <= t - grace_s)
      {
        rep.orphans_unlinked += unlink(it->path()) ? 1 : 0;
      }
    }
    // 3. Empty directories, deepest first; the top-level one stays.
    std::sort(
      dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b)
      { return a.native().size() > b.native().size(); });
    for (const auto& d : dirs)
    {
      if (fs::is_empty(d, ec) && !ec && ::rmdir(d.c_str()) == 0)
      {
        ++rep.dirs_removed;
      }
    }
  }
  return rep;
}

void ManifestPublisher::prune() const
{
  if (current_.seq <= keep_)
  {
    return;
  }
  const std::uint64_t oldest_kept = current_.seq - keep_ + 1;
  for (const auto& e : fs::directory_iterator(dir_))
  {
    const auto s = manifest_seq(e.path().filename().string());
    if (s && *s < oldest_kept)
    {
      std::error_code ec;
      fs::remove(e.path(), ec);  // best effort; gc retries later
    }
  }
}

}  // namespace atmo
