#include "fileio.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace atmo::detail
{

void write_all(
  int fd,
  const void* buf,
  std::size_t n,
  std::size_t off,
  const char* who)
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
        std::string(who) + ": write failed: " + std::strerror(errno));
    }
    p += w;
    off += static_cast<std::size_t>(w);
    n -= static_cast<std::size_t>(w);
  }
}

void fsync_dir(const std::filesystem::path& dir, const char* who)
{
  const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0)
  {
    throw std::runtime_error(
      std::string(who) + ": cannot open directory " + dir.string());
  }
  const int rc = ::fsync(dfd);
  ::close(dfd);
  if (rc != 0)
  {
    throw std::runtime_error(
      std::string(who) + ": fsync of " + dir.string() + " failed");
  }
}

TempFile::TempFile(std::filesystem::path path, const char* who)
    : path_(std::move(path)), who_(who)
{
  namespace fs = std::filesystem;
  const fs::path dir =
    path_.has_parent_path() ? path_.parent_path() : fs::path(".");
  // Create missing directories, remembering which, so their own entries
  // can be made durable too.
  for (fs::path d = dir; !d.empty() && !fs::exists(d); d = d.parent_path())
  {
    created_.push_back(d);
    if (!d.has_parent_path() || d.parent_path() == d)
    {
      break;
    }
  }
  fs::create_directories(dir);

  static std::atomic<unsigned> counter{0};
  tmp_ = fs::path(
    path_.string() + ".tmp." + std::to_string(::getpid()) + "." +
    std::to_string(counter++));
  fd_ = ::open(tmp_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd_ < 0)
  {
    throw std::runtime_error(
      std::string(who_) + ": cannot create " + tmp_.string() + ": " +
      std::strerror(errno));
  }
}

TempFile::~TempFile()
{
  if (fd_ >= 0)
  {
    ::close(fd_);
  }
  if (!published_)
  {
    ::unlink(tmp_.c_str());
  }
}

void TempFile::write(const void* buf, std::size_t n, std::size_t off)
{
  write_all(fd_, buf, n, off, who_);
}

void TempFile::publish()
{
  namespace fs = std::filesystem;
  if (published_ || fd_ < 0)
  {
    throw std::logic_error(std::string(who_) + ": TempFile published twice");
  }
  if (::fsync(fd_) != 0)
  {
    throw std::runtime_error(
      std::string(who_) + ": fsync: " + std::strerror(errno));
  }
  ::close(fd_);
  fd_ = -1;
  // link() refuses to replace an existing name: published files are
  // immutable.
  if (::link(tmp_.c_str(), path_.c_str()) != 0)
  {
    throw std::runtime_error(
      std::string(who_) + ": cannot publish " + path_.string() + ": " +
      std::strerror(errno));
  }
  published_ = true;
  ::unlink(tmp_.c_str());
  const fs::path dir =
    path_.has_parent_path() ? path_.parent_path() : fs::path(".");
  fsync_dir(dir, who_);
  for (const fs::path& d : created_)
  {
    fsync_dir(d.has_parent_path() ? d.parent_path() : fs::path("."), who_);
  }
}

void publish_file(
  const std::filesystem::path& path,
  const std::vector<unsigned char>& bytes,
  const char* who)
{
  TempFile f(path, who);
  f.write(bytes.data(), bytes.size(), 0);
  f.publish();
}

std::shared_ptr<const MappedFile> MappedFile::open(
  const std::filesystem::path& path,
  std::size_t min_size,
  const char* who)
{
  auto bad = [&](const std::string& why)
  {
    return std::runtime_error(
      std::string(who) + ": " + path.string() + ": " + why);
  };
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
  if (fsize < min_size)
  {
    ::close(fd);
    throw bad("truncated (shorter than header page)");
  }
  void* p = ::mmap(nullptr, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED)
  {
    throw bad("mmap failed");
  }
  std::shared_ptr<MappedFile> m(new MappedFile());
  m->data_ = static_cast<const unsigned char*>(p);
  m->size_ = fsize;
  return m;
}

MappedFile::~MappedFile()
{
  if (data_ != nullptr)
  {
    ::munmap(const_cast<unsigned char*>(data_), size_);
  }
}

std::size_t align_up(std::size_t v, std::size_t a)
{
  return (v + a - 1) / a * a;
}

}  // namespace atmo::detail
