// Internal file helpers shared by the immutable-file writers and readers.
// Not installed; include only from src/*.cpp.
#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace atmo::detail
{

// pwrite() all of buf at off; std::runtime_error (prefixed with `who`) on
// failure.
void write_all(
  int fd,
  const void* buf,
  std::size_t n,
  std::size_t off,
  const char* who);

// fsync a directory; std::runtime_error on failure.
void fsync_dir(const std::filesystem::path& dir, const char* who);

// A file written in place under a temporary name next to `path`
// ("<path>.tmp.<pid>.<n>"; gc removes leftovers) and published as `path`
// by publish(): fsync -> link() (fails if `path` exists) -> unlink temp ->
// fsync directory (and any parent directories the constructor created).
// The destructor removes the temp file unless it was published.
class TempFile
{
 public:
  TempFile(std::filesystem::path path, const char* who);
  ~TempFile();
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  int fd() const { return fd_; }
  const std::filesystem::path& path() const { return path_; }
  // pwrite() all of buf at off; std::runtime_error on failure.
  void write(const void* buf, std::size_t n, std::size_t off);
  void publish();

 private:
  std::filesystem::path path_, tmp_;
  const char* who_;
  std::vector<std::filesystem::path> created_;
  int fd_ = -1;
  bool published_ = false;
};

// Publishes `bytes` as `path`: temp file in the same directory -> fsync ->
// link() to the final name (fails if it exists, so a published file is
// never replaced) -> unlink temp -> fsync directory.  Creates missing
// parent directories and fsyncs their entries too.
void publish_file(
  const std::filesystem::path& path,
  const std::vector<unsigned char>& bytes,
  const char* who);

// A whole file mapped read-only.  open() throws std::runtime_error
// (prefixed with `who` and the path) if the file cannot be opened or
// mapped, or is shorter than `min_size`.
class MappedFile
{
 public:
  static std::shared_ptr<const MappedFile> open(
    const std::filesystem::path& path,
    std::size_t min_size,
    const char* who);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const unsigned char* data() const { return data_; }
  std::size_t size() const { return size_; }

 private:
  MappedFile() = default;

  const unsigned char* data_ = nullptr;
  std::size_t size_ = 0;
};

// Round v up to a multiple of a.
std::size_t align_up(std::size_t v, std::size_t a);

}  // namespace atmo::detail
