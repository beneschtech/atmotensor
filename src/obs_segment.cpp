#include "atmo/obs_segment.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>

#include "atmo/crc32.hpp"

static_assert(
  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
  "segment files are little-endian");

namespace atmo
{
namespace obsseg
{
namespace
{

std::size_t align(std::size_t v, std::size_t a)
{
  return (v + a - 1) / a * a;
}

}  // namespace

Layout::Layout(
  std::uint64_t n_reports,
  std::uint64_t n_values,
  std::uint64_t raw_bytes)
{
  const std::size_t n = static_cast<std::size_t>(n_reports);
  const std::size_t m = static_cast<std::size_t>(n_values);
  std::size_t cur = kPage;
  auto section = [&](std::size_t s, auto&& body)
  {
    sec_off[s] = cur;
    std::size_t c = cur;
    body(c);
    sec_len[s] = c - cur;
    cur = align(c, kPage);
  };
  section(
    0,
    [&](std::size_t& c)
    {
      for (std::size_t k = 0; k < kNumRCols; ++k)
      {
        c = align(c, 8);
        rcol[k] = c;
        c += n * kRColSize[k];
      }
    });
  section(
    1,
    [&](std::size_t& c)
    {
      for (std::size_t k = 0; k < kNumVCols; ++k)
      {
        c = align(c, 8);
        vcol[k] = c;
        c += m * kVColSize[k];
      }
    });
  section(
    2,
    [&](std::size_t& c)
    {
      raw = c;
      c += static_cast<std::size_t>(raw_bytes);
    });
  section(
    3,
    [&](std::size_t& c)
    {
      tree_pts = c;
      c += n * 16;
      tree_ids = c;
      c += n * 4;
      tree_axes = c;
      c += n;
    });
  total = cur;
}

}  // namespace obsseg

namespace
{

using namespace obsseg;

void write_all(int fd, const void* buf, std::size_t n, std::size_t off)
{
  const char* p = static_cast<const char*>(buf);
  while (n > 0)
  {
    const ssize_t w = ::pwrite(fd, p, n, static_cast<off_t>(off));
    if (w < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      throw std::runtime_error(
        std::string("ObsSegment: write failed: ") + std::strerror(errno));
    }
    p += w;
    off += static_cast<std::size_t>(w);
    n -= static_cast<std::size_t>(w);
  }
}

void fsync_dir(const std::filesystem::path& dir)
{
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0)
  {
    throw std::runtime_error(
      "ObsSegment: cannot open directory " + dir.string());
  }
  const int rc = ::fsync(dfd);
  ::close(dfd);
  if (rc != 0)
  {
    throw std::runtime_error(
      "ObsSegment: fsync of " + dir.string() + " failed");
  }
}

// Temp file in the target directory -> fsync -> link() to the final name
// (refuses to replace an existing file) -> unlink temp -> fsync directory.
void write_atomically(
  const std::filesystem::path& path,
  const std::vector<unsigned char>& file)
{
  namespace fs = std::filesystem;
  const fs::path dir =
    path.has_parent_path() ? path.parent_path() : fs::path(".");
  // Create missing directories, remembering which, so their own entries
  // can be made durable too.
  std::vector<fs::path> created;
  for (fs::path d = dir; !d.empty() && !fs::exists(d); d = d.parent_path())
  {
    created.push_back(d);
    if (!d.has_parent_path() || d.parent_path() == d)
    {
      break;
    }
  }
  fs::create_directories(dir);

  static std::atomic<unsigned> counter{0};
  const fs::path tmp(
    path.string() + ".tmp." + std::to_string(::getpid()) + "." +
    std::to_string(counter++));
  const int fd =
    ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
  {
    throw std::runtime_error(
      "ObsSegment: cannot create " + tmp.string() + ": " +
      std::strerror(errno));
  }
  try
  {
    write_all(fd, file.data(), file.size(), 0);
    if (::fsync(fd) != 0)
    {
      throw std::runtime_error(
        std::string("ObsSegment: fsync: ") + std::strerror(errno));
    }
  }
  catch (...)
  {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw;
  }
  ::close(fd);
  if (::link(tmp.c_str(), path.c_str()) != 0)
  {
    const int err = errno;
    ::unlink(tmp.c_str());
    throw std::runtime_error(
      "ObsSegment: cannot publish " + path.string() + ": " +
      std::strerror(err));
  }
  ::unlink(tmp.c_str());
  fsync_dir(dir);
  for (const fs::path& d : created)
  {
    fsync_dir(d.has_parent_path() ? d.parent_path() : fs::path("."));
  }
}

}  // namespace

ValuesView::ValuesView(
  const std::uint16_t* var,
  const float* value,
  const std::uint16_t* qc,
  const float* height_m,
  std::size_t n)
    : var_(var), value_(value), qc_(qc), height_(height_m), n_(n)
{
}

// ---------------------------------------------------------------------------
// ObsSegmentWriter

ObsSegmentWriter::ObsSegmentWriter(
  const SourceClass& source,
  std::int64_t bucket_start_s)
    : source_(source), bucket_start_s_(bucket_start_s)
{
  source_.validate();
  embedding_ = source_.make_embedding(bucket_start_s);  // throws if unaligned
}

std::size_t ObsSegmentWriter::add_report(const ObsReport& r)
{
  if (sealed_)
  {
    throw std::logic_error("ObsSegmentWriter: already sealed");
  }
  auto bad = [&](const std::string& why)
  {
    return std::invalid_argument(
      "ObsSegmentWriter (" + source_.name + "): report " +
      std::to_string(r.report_key) + ": " + why);
  };
  if (
    r.obs_time_s < bucket_start_s_ ||
    r.obs_time_s - bucket_start_s_ >= source_.bucket_seconds)
  {
    throw bad(
      "obs_time_s " + std::to_string(r.obs_time_s) + " outside bucket " +
      std::to_string(bucket_start_s_));
  }
  if (!(std::isfinite(r.lat_deg) && r.lat_deg >= -90.0 && r.lat_deg <= 90.0))
  {
    throw bad("bad latitude");
  }
  if (!(std::isfinite(r.lon_deg) && r.lon_deg >= -180.0 && r.lon_deg <= 360.0))
  {
    throw bad("bad longitude");
  }
  if (!std::isfinite(r.height_m))
  {
    throw bad("bad height");
  }
  if (r.values.size() > std::numeric_limits<std::uint16_t>::max())
  {
    throw bad("too many values");
  }
  for (const ObsValue& v : r.values)
  {
    if (!std::isfinite(v.value))
    {
      throw bad("non-finite value");
    }
    if (std::isinf(v.height_m))
    {
      throw bad("infinite value height");
    }
  }
  if (r.raw.size() > std::numeric_limits<std::uint32_t>::max())
  {
    throw bad("raw message too long");
  }
  if (
    key_.size() >= std::numeric_limits<std::uint32_t>::max() ||
    var_.size() + r.values.size() > std::numeric_limits<std::uint32_t>::max() ||
    raw_.size() + r.raw.size() > (std::uint64_t(1) << 40))
  {
    throw std::length_error("ObsSegmentWriter: segment too large");
  }
  if (!versions_.insert({r.report_key, r.receipt_seq}).second)
  {
    throw bad("duplicate (report_key, receipt_seq)");
  }

  const std::size_t row = key_.size();
  key_.push_back(r.report_key);
  seq_.push_back(r.receipt_seq);
  receipt_time_ms_.push_back(r.receipt_time_ms);
  obs_time_s_.push_back(r.obs_time_s);
  lat_deg_.push_back(r.lat_deg);
  lon_deg_.push_back(r.lon_deg);
  height_m_.push_back(r.height_m);
  station_.push_back(r.station_ref);
  flags_.push_back(r.flags);
  nval_.push_back(static_cast<std::uint16_t>(r.values.size()));
  first_.push_back(static_cast<std::uint32_t>(var_.size()));
  raw_off_.push_back(raw_.size());
  raw_len_.push_back(static_cast<std::uint32_t>(r.raw.size()));
  for (const ObsValue& v : r.values)
  {
    var_.push_back(v.var_code);
    value_.push_back(v.value);
    qc_.push_back(v.qc);
    value_height_m_.push_back(std::isnan(v.height_m) ? r.height_m : v.height_m);
  }
  raw_.insert(raw_.end(), r.raw.begin(), r.raw.end());
  return row;
}

SealedSegment ObsSegmentWriter::seal(const std::filesystem::path& path)
{
  if (sealed_)
  {
    throw std::logic_error("ObsSegmentWriter: already sealed");
  }
  const std::size_t n = size();

  std::vector<KdTree4<float>::Point> pts(n);
  for (std::size_t i = 0; i < n; ++i)
  {
    pts[i] = embed<float>(
      lat_deg_[i], lon_deg_[i], height_m_[i],
      static_cast<double>(obs_time_s_[i]), embedding_);
  }
  KdTree4<float> tree(pts);

  const Layout L(n, var_.size(), raw_.size());
  std::vector<unsigned char> file(L.total, 0);
  auto put = [&](std::size_t off, const auto& vec)
  {
    if (!vec.empty())
    {
      std::memcpy(file.data() + off, vec.data(), vec.size() * sizeof(vec[0]));
    }
  };
  put(L.rcol[kKey], key_);
  put(L.rcol[kSeq], seq_);
  put(L.rcol[kRecvTime], receipt_time_ms_);
  put(L.rcol[kObsTime], obs_time_s_);
  put(L.rcol[kLat], lat_deg_);
  put(L.rcol[kLon], lon_deg_);
  put(L.rcol[kHeight], height_m_);
  put(L.rcol[kStation], station_);
  put(L.rcol[kFlags], flags_);
  put(L.rcol[kNVal], nval_);
  put(L.rcol[kFirst], first_);
  put(L.rcol[kRawOff], raw_off_);
  put(L.rcol[kRawLen], raw_len_);
  put(L.vcol[kVar], var_);
  put(L.vcol[kValue], value_);
  put(L.vcol[kQc], qc_);
  put(L.vcol[kVHeight], value_height_m_);
  put(L.raw, raw_);
  put(L.tree_pts, tree.points());
  put(L.tree_ids, tree.ids());
  put(L.tree_axes, tree.axes());

  SegHeader h;
  std::memset(&h, 0, sizeof h);
  std::memcpy(h.magic, kMagic, 8);
  h.version = kVersion;
  std::memcpy(h.source, source_.name.data(), source_.name.size());
  h.bucket_start_s = bucket_start_s_;
  h.bucket_seconds = source_.bucket_seconds;
  h.L_km = source_.L_km;
  h.tau_seconds = source_.tau_seconds;
  h.vertical_scale = source_.vertical_scale;
  h.radius_km = embedding_.radius_km;
  h.n_reports = n;
  h.n_values = var_.size();
  h.raw_bytes = raw_.size();
  if (n > 0)
  {
    h.receipt_seq_min = h.receipt_seq_max = seq_[0];
    h.obs_time_min_s = h.obs_time_max_s = obs_time_s_[0];
    for (std::size_t i = 1; i < n; ++i)
    {
      h.receipt_seq_min = std::min(h.receipt_seq_min, seq_[i]);
      h.receipt_seq_max = std::max(h.receipt_seq_max, seq_[i]);
      h.obs_time_min_s = std::min(h.obs_time_min_s, obs_time_s_[i]);
      h.obs_time_max_s = std::max(h.obs_time_max_s, obs_time_s_[i]);
    }
  }
  for (std::size_t s = 0; s < kSections; ++s)
  {
    h.sec[s].off = L.sec_off[s];
    h.sec[s].len = L.sec_len[s];
    h.sec[s].crc = crc32(file.data() + L.sec_off[s], L.sec_len[s]);
  }
  h.crc_header = crc32(&h, offsetof(SegHeader, crc_header));
  std::memcpy(file.data(), &h, sizeof h);

  write_atomically(path, file);
  sealed_ = true;

  SealedSegment info;
  info.path = path;
  info.n_reports = n;
  info.n_values = h.n_values;
  info.raw_bytes = h.raw_bytes;
  info.receipt_seq_min = h.receipt_seq_min;
  info.receipt_seq_max = h.receipt_seq_max;
  info.header_crc = h.crc_header;
  release();
  return info;
}

void ObsSegmentWriter::release()
{
  auto drop = [](auto& v) { std::decay_t<decltype(v)>().swap(v); };
  drop(versions_);
  drop(key_), drop(seq_), drop(receipt_time_ms_), drop(obs_time_s_);
  drop(lat_deg_), drop(lon_deg_), drop(height_m_), drop(station_);
  drop(flags_), drop(nval_), drop(first_), drop(raw_off_), drop(raw_len_);
  drop(var_), drop(qc_), drop(value_), drop(value_height_m_), drop(raw_);
}

// ---------------------------------------------------------------------------
// ObsSegment

struct ObsSegment::Mapping
{
  void* p = MAP_FAILED;
  std::size_t n = 0;
  ~Mapping()
  {
    if (p != MAP_FAILED)
    {
      ::munmap(p, n);
    }
  }
};

std::shared_ptr<const ObsSegment> ObsSegment::open(
  const std::filesystem::path& path,
  Verify verify)
{
  return std::shared_ptr<const ObsSegment>(new ObsSegment(path, verify));
}

bool ObsSegment::matches(const SourceClass& s) const
{
  return s.name == source_name() && s.bucket_seconds == h_.bucket_seconds &&
         s.L_km == h_.L_km && s.tau_seconds == h_.tau_seconds &&
         s.vertical_scale == h_.vertical_scale &&
         h_.radius_km == Embedding{}.radius_km;
}

std::vector<ObsSegment::SectionInfo> ObsSegment::sections() const
{
  std::vector<SectionInfo> v;
  for (std::size_t s = 0; s < kSections; ++s)
  {
    v.push_back(
      {kSectionNames[s], static_cast<std::size_t>(h_.sec[s].off),
       static_cast<std::size_t>(h_.sec[s].len), h_.sec[s].crc});
  }
  return v;
}

ReportView ObsSegment::report(std::size_t i) const
{
  if (i >= n_)
  {
    throw std::out_of_range("ObsSegment: report index out of range");
  }
  ReportView v;
  v.row = i;
  v.report_key = key_[i];
  v.receipt_seq = seq_[i];
  v.receipt_time_ms = receipt_time_ms_[i];
  v.obs_time_s = obs_time_s_[i];
  v.lat_deg = lat_deg_[i];
  v.lon_deg = lon_deg_[i];
  v.height_m = height_m_[i];
  v.station_ref = station_[i];
  v.flags = flags_[i];
  const std::size_t first = first_[i];
  v.values = ValuesView(
    var_ + first, value_ + first, qc_ + first, value_height_m_ + first,
    nval_[i]);
  v.raw = std::string_view(raw_ + raw_off_[i], raw_len_[i]);
  return v;
}

ObsSegment::Tree::Point ObsSegment::query_point(
  double lat_deg,
  double lon_deg,
  double height_m,
  double t_s) const
{
  return embed<float>(lat_deg, lon_deg, height_m, t_s, emb_);
}

std::vector<ObsSegment::Tree::Hit> ObsSegment::knn(
  double lat_deg,
  double lon_deg,
  double height_m,
  double t_s,
  std::size_t k) const
{
  return tree_.knn(query_point(lat_deg, lon_deg, height_m, t_s), k);
}

std::vector<ObsSegment::Tree::Hit> ObsSegment::radius(
  double lat_deg,
  double lon_deg,
  double height_m,
  double t_s,
  float r_km) const
{
  return tree_.radius(query_point(lat_deg, lon_deg, height_m, t_s), r_km);
}

ObsSegment::ObsSegment(const std::filesystem::path& path, Verify verify)
    : path_(path)
{
  auto bad = [&](const std::string& why)
  { return std::runtime_error("ObsSegment: " + path.string() + ": " + why); };
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    throw bad(std::strerror(errno));
  }
  struct stat st;
  if (::fstat(fd, &st) != 0)
  {
    ::close(fd);
    throw bad("fstat failed");
  }
  const std::size_t fsize = static_cast<std::size_t>(st.st_size);
  if (fsize < kPage)
  {
    ::close(fd);
    throw bad("truncated (shorter than header page)");
  }
  map_ = std::make_shared<Mapping>();
  map_->p = ::mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map_->p == MAP_FAILED)
  {
    throw bad("mmap failed");
  }
  map_->n = fsize;
  base_ = static_cast<const unsigned char*>(map_->p);

  std::memcpy(&h_, base_, sizeof h_);
  if (std::memcmp(h_.magic, kMagic, 8) != 0)
  {
    throw bad("bad magic");
  }
  if (crc32(&h_, offsetof(SegHeader, crc_header)) != h_.crc_header)
  {
    throw bad("header checksum mismatch");
  }
  if (h_.version != kVersion)
  {
    throw bad("unsupported version");
  }
  if (std::memchr(h_.source, 0, sizeof h_.source) == nullptr)
  {
    throw bad("bad source name");
  }

  SourceClass s;
  s.name = h_.source;
  s.bucket_seconds = h_.bucket_seconds;
  s.L_km = h_.L_km;
  s.tau_seconds = h_.tau_seconds;
  s.vertical_scale = h_.vertical_scale;
  try
  {
    s.validate();
    emb_ = s.make_embedding(h_.bucket_start_s);
  }
  catch (const std::exception& e)
  {
    throw bad(e.what());
  }
  if (!(std::isfinite(h_.radius_km) && h_.radius_km > 0))
  {
    throw bad("bad radius");
  }
  emb_.radius_km = h_.radius_km;

  if (
    h_.n_reports > std::numeric_limits<std::uint32_t>::max() ||
    h_.n_values > std::numeric_limits<std::uint32_t>::max() ||
    h_.raw_bytes > fsize)
  {
    throw bad("bad counts");
  }
  n_ = static_cast<std::size_t>(h_.n_reports);
  layout_ = Layout(h_.n_reports, h_.n_values, h_.raw_bytes);
  const Layout& L = layout_;
  for (std::size_t k = 0; k < kSections; ++k)
  {
    if (h_.sec[k].off != L.sec_off[k] || h_.sec[k].len != L.sec_len[k])
    {
      throw bad("bad section offsets");
    }
  }
  if (fsize != L.total)
  {
    throw bad("file size does not match header (truncated?)");
  }
  if (verify == Verify::full)
  {
    for (std::size_t k = 0; k < kSections; ++k)
    {
      if (crc32(base_ + L.sec_off[k], L.sec_len[k]) != h_.sec[k].crc)
      {
        throw bad(std::string(kSectionNames[k]) + " section checksum mismatch");
      }
    }
  }

  auto at = [&](std::size_t off) { return base_ + off; };
  key_ = reinterpret_cast<const std::uint64_t*>(at(L.rcol[kKey]));
  seq_ = reinterpret_cast<const std::uint64_t*>(at(L.rcol[kSeq]));
  receipt_time_ms_ =
    reinterpret_cast<const std::int64_t*>(at(L.rcol[kRecvTime]));
  obs_time_s_ = reinterpret_cast<const std::int64_t*>(at(L.rcol[kObsTime]));
  lat_deg_ = reinterpret_cast<const double*>(at(L.rcol[kLat]));
  lon_deg_ = reinterpret_cast<const double*>(at(L.rcol[kLon]));
  height_m_ = reinterpret_cast<const float*>(at(L.rcol[kHeight]));
  station_ = reinterpret_cast<const std::uint32_t*>(at(L.rcol[kStation]));
  flags_ = reinterpret_cast<const std::uint16_t*>(at(L.rcol[kFlags]));
  nval_ = reinterpret_cast<const std::uint16_t*>(at(L.rcol[kNVal]));
  first_ = reinterpret_cast<const std::uint32_t*>(at(L.rcol[kFirst]));
  raw_off_ = reinterpret_cast<const std::uint64_t*>(at(L.rcol[kRawOff]));
  raw_len_ = reinterpret_cast<const std::uint32_t*>(at(L.rcol[kRawLen]));
  var_ = reinterpret_cast<const std::uint16_t*>(at(L.vcol[kVar]));
  value_ = reinterpret_cast<const float*>(at(L.vcol[kValue]));
  qc_ = reinterpret_cast<const std::uint16_t*>(at(L.vcol[kQc]));
  value_height_m_ = reinterpret_cast<const float*>(at(L.vcol[kVHeight]));
  raw_ = reinterpret_cast<const char*>(at(L.raw));

  // Structure: value slices contiguous, raw slices in range, times in the
  // bucket, receipt range as advertised, tree a permutation of the rows.
  std::uint64_t next = 0;
  for (std::size_t i = 0; i < n_; ++i)
  {
    if (first_[i] != next)
    {
      throw bad("values table not contiguous");
    }
    next += nval_[i];
    if (raw_off_[i] > h_.raw_bytes || raw_len_[i] > h_.raw_bytes - raw_off_[i])
    {
      throw bad("raw slice out of range");
    }
    const std::int64_t t = obs_time_s_[i];
    if (
      t < h_.bucket_start_s || t - h_.bucket_start_s >= h_.bucket_seconds ||
      t < h_.obs_time_min_s || t > h_.obs_time_max_s)
    {
      throw bad("obs_time outside bucket");
    }
    if (seq_[i] < h_.receipt_seq_min || seq_[i] > h_.receipt_seq_max)
    {
      throw bad("receipt_seq outside header range");
    }
  }
  if (next != h_.n_values)
  {
    throw bad("value count mismatch");
  }
  for (std::size_t k = 0; k < h_.n_values; ++k)
  {
    if (!std::isfinite(value_height_m_[k]))
    {
      throw bad("non-finite value height");
    }
  }

  tree_ = Tree(
    reinterpret_cast<const Tree::Point*>(at(L.tree_pts)),
    reinterpret_cast<const Tree::Id*>(at(L.tree_ids)), at(L.tree_axes), n_);
  if (!tree_.well_formed())
  {
    throw bad("malformed tree");
  }
}

}  // namespace atmo
