#include "atmo/gridstore.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>

#include "atmo/crc32.h"

static_assert(
  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
  "grid files are little-endian");

namespace atmo
{
namespace
{

constexpr std::size_t kPage = 4096;
constexpr std::size_t kCanonOff = 128;
constexpr std::uint32_t kVersion = 1;

struct FileHeader
{
  char magic[8];
  std::uint32_t version;
  std::uint32_t flags;  // bit 0: periodic_i
  std::uint64_t key;
  std::uint32_t ni, nj;
  std::uint32_t canon_len;
  std::uint32_t crc_canon;
  std::uint64_t lat_off, lon_off, rot_off;
  std::uint32_t crc_lat, crc_lon, crc_rot;
  std::uint32_t crc_header;  // over every byte before this field
};
static_assert(sizeof(FileHeader) == 80, "FileHeader layout");

struct Mapping
{
  void* p = nullptr;
  std::size_t n = 0;
  ~Mapping()
  {
    if (p && p != MAP_FAILED)
    {
      ::munmap(p, n);
    }
  }
};

std::size_t align_up(std::size_t v)
{
  return (v + kPage - 1) / kPage * kPage;
}

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
        std::string("GridStore: write failed: ") + std::strerror(errno));
    }
    p += w;
    off += static_cast<std::size_t>(w);
    n -= static_cast<std::size_t>(w);
  }
}

std::shared_ptr<const Grid> load_file(
  const std::filesystem::path& path,
  GridKey want_key)
{
  auto bad = [&](const char* why)
  { return std::runtime_error("GridStore: " + path.string() + ": " + why); };
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
  auto map = std::make_shared<Mapping>();
  map->n = fsize;
  map->p = ::mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map->p == MAP_FAILED)
  {
    throw bad("mmap failed");
  }
  const auto* base = static_cast<const unsigned char*>(map->p);

  FileHeader h;
  std::memcpy(&h, base, sizeof h);
  if (std::memcmp(h.magic, "ATGRID01", 8) != 0)
  {
    throw bad("bad magic");
  }
  if (crc32(&h, offsetof(FileHeader, crc_header)) != h.crc_header)
  {
    throw bad("header checksum mismatch");
  }
  if (h.version != kVersion)
  {
    throw bad("unsupported version");
  }
  if (h.key != want_key)
  {
    throw bad("key does not match file name");
  }
  if (h.canon_len > GridIdentity::kMaxCanon)
  {
    throw bad("bad canon length");
  }

  GridIdentity id;
  id.ni = h.ni;
  id.nj = h.nj;
  id.periodic_i = (h.flags & 1u) != 0;
  id.canon.assign(base + kCanonOff, base + kCanonOff + h.canon_len);
  try
  {
    id.validate();
  }
  catch (const std::exception& e)
  {
    throw bad(e.what());
  }
  if (crc32(id.canon.data(), id.canon.size()) != h.crc_canon)
  {
    throw bad("canon checksum mismatch");
  }
  if (id.key() != h.key)
  {
    throw bad("identity does not hash to stored key");
  }

  const std::size_t n = id.points();
  const std::size_t lat_off = align_up(kPage);
  const std::size_t lon_off = align_up(lat_off + n * sizeof(double));
  const std::size_t rot_off = align_up(lon_off + n * sizeof(double));
  const std::size_t total = align_up(rot_off + n * sizeof(float));
  if (h.lat_off != lat_off || h.lon_off != lon_off || h.rot_off != rot_off)
  {
    throw bad("bad section offsets");
  }
  if (fsize != total)
  {
    throw bad("file size does not match header (truncated?)");
  }

  if (crc32(base + lat_off, n * sizeof(double)) != h.crc_lat)
  {
    throw bad("lat checksum mismatch");
  }
  if (crc32(base + lon_off, n * sizeof(double)) != h.crc_lon)
  {
    throw bad("lon checksum mismatch");
  }
  if (crc32(base + rot_off, n * sizeof(float)) != h.crc_rot)
  {
    throw bad("rot checksum mismatch");
  }

  return Grid::adopt(
    std::move(id), reinterpret_cast<const double*>(base + lat_off),
    reinterpret_cast<const double*>(base + lon_off),
    reinterpret_cast<const float*>(base + rot_off), std::move(map));
}

}  // namespace

GridStore::GridStore(std::filesystem::path dir) : dir_(std::move(dir))
{
  std::filesystem::create_directories(dir_);
}

std::shared_ptr<const Grid> GridStore::put(
  const GridIdentity& id,
  GridData data)
{
  id.validate();
  const GridKey key = id.key();
  const std::size_t n = id.points();
  if (data.lat_deg.size() != n || data.lon_deg.size() != n)
  {
    throw std::invalid_argument("grid arrays do not match ni*nj");
  }
  if (data.rot_rad.empty())
  {
    data.rot_rad =
      derive_rotation(data.lat_deg, data.lon_deg, id.ni, id.nj, id.periodic_i);
  }
  else if (data.rot_rad.size() != n)
  {
    throw std::invalid_argument("rotation array does not match ni*nj");
  }

  std::lock_guard<std::mutex> lk(mu_);
  const auto path = path_for(key);
  if (!std::filesystem::exists(path))
  {
    // Validate by building the in-memory grid (checks finiteness etc.)
    // before anything touches the disk.
    (void)Grid::create(id, data);
    write_file(path, key, id, data);
  }
  auto g = load_locked(key);
  // Same key must mean same grid.
  if (
    g->identity() != id ||
    std::memcmp(g->lat_deg(), data.lat_deg.data(), n * sizeof(double)) != 0 ||
    std::memcmp(g->lon_deg(), data.lon_deg.data(), n * sizeof(double)) != 0)
  {
    throw std::runtime_error(
      "GridStore: key " + key_hex(key) +
      " already stored with different contents");
  }
  return g;
}

std::shared_ptr<const Grid> GridStore::get(GridKey key) const
{
  std::lock_guard<std::mutex> lk(mu_);
  if (!std::filesystem::exists(path_for(key)))
  {
    return nullptr;
  }
  return load_locked(key);
}

bool GridStore::contains(GridKey key) const
{
  std::lock_guard<std::mutex> lk(mu_);
  return std::filesystem::exists(path_for(key));
}

std::uint32_t GridStore::header_crc(GridKey key) const
{
  const auto path = path_for(key);
  auto bad = [&](const std::string& why)
  { return std::runtime_error("GridStore: " + path.string() + ": " + why); };
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    throw bad(std::strerror(errno));
  }
  FileHeader h;
  const ssize_t got = ::pread(fd, &h, sizeof h, 0);
  ::close(fd);
  if (got != static_cast<ssize_t>(sizeof h))
  {
    throw bad("truncated (shorter than header)");
  }
  if (std::memcmp(h.magic, "ATGRID01", 8) != 0)
  {
    throw bad("bad magic");
  }
  if (crc32(&h, offsetof(FileHeader, crc_header)) != h.crc_header)
  {
    throw bad("header checksum mismatch");
  }
  if (h.key != key)
  {
    throw bad("key does not match file name");
  }
  return h.crc_header;
}

std::vector<GridKey> GridStore::list() const
{
  std::vector<GridKey> keys;
  for (const auto& e : std::filesystem::directory_iterator(dir_))
  {
    const std::string name = e.path().filename().string();
    if (name.size() == 16 + 5 && name.substr(16) == ".grid")
    {
      try
      {
        keys.push_back(std::stoull(name.substr(0, 16), nullptr, 16));
      }
      catch (...)
      {
      }
    }
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::filesystem::path GridStore::path_for(GridKey key) const
{
  return dir_ / (key_hex(key) + ".grid");
}

void GridStore::write_file(
  const std::filesystem::path& path,
  GridKey key,
  const GridIdentity& id,
  const GridData& d) const
{
  const std::size_t n = id.points();
  FileHeader h;
  std::memset(&h, 0, sizeof h);
  std::memcpy(h.magic, "ATGRID01", 8);
  h.version = kVersion;
  h.flags = id.periodic_i ? 1u : 0u;
  h.key = key;
  h.ni = id.ni;
  h.nj = id.nj;
  h.canon_len = static_cast<std::uint32_t>(id.canon.size());
  h.crc_canon = crc32(id.canon.data(), id.canon.size());
  h.lat_off = kPage;
  h.lon_off = align_up(h.lat_off + n * sizeof(double));
  h.rot_off = align_up(h.lon_off + n * sizeof(double));
  h.crc_lat = crc32(d.lat_deg.data(), n * sizeof(double));
  h.crc_lon = crc32(d.lon_deg.data(), n * sizeof(double));
  h.crc_rot = crc32(d.rot_rad.data(), n * sizeof(float));
  h.crc_header = crc32(&h, offsetof(FileHeader, crc_header));
  const std::size_t total = align_up(h.rot_off + n * sizeof(float));

  const auto tmp =
    std::filesystem::path(path.string() + ".tmp." + std::to_string(::getpid()));
  const int fd =
    ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0)
  {
    throw std::runtime_error(
      "GridStore: cannot create " + tmp.string() + ": " + std::strerror(errno));
  }
  try
  {
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0)
    {
      throw std::runtime_error(
        std::string("GridStore: ftruncate: ") + std::strerror(errno));
    }
    write_all(fd, &h, sizeof h, 0);
    write_all(fd, id.canon.data(), id.canon.size(), kCanonOff);
    write_all(fd, d.lat_deg.data(), n * sizeof(double), h.lat_off);
    write_all(fd, d.lon_deg.data(), n * sizeof(double), h.lon_off);
    write_all(fd, d.rot_rad.data(), n * sizeof(float), h.rot_off);
    if (::fsync(fd) != 0)
    {
      throw std::runtime_error(
        std::string("GridStore: fsync: ") + std::strerror(errno));
    }
  }
  catch (...)
  {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0)
  {
    const int err = errno;
    ::unlink(tmp.c_str());
    throw std::runtime_error(
      std::string("GridStore: rename: ") + std::strerror(err));
  }
  const int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0)
  {
    ::fsync(dfd);
    ::close(dfd);
  }
}

std::shared_ptr<const Grid> GridStore::load_locked(GridKey key) const
{
  if (auto it = cache_.find(key); it != cache_.end())
  {
    if (auto g = it->second.lock())
    {
      return g;
    }
  }
  auto g = load_file(path_for(key), key);
  cache_[key] = g;
  return g;
}

}  // namespace atmo
