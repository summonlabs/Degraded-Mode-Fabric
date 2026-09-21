// Degraded Mode Fabric - durable file primitives.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifdef _MSC_VER
// The Microsoft C runtime reserves this macro name; defining it is the
// documented way to silence its non-standard deprecation of open/read/write,
// which this translation unit uses deliberately for their durability
// guarantees rather than for convenience.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "store/file_io.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/locking.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dmf {
namespace detail {

namespace {

std::string os_error_text() {
  const std::error_code code(errno, std::generic_category());
  return code.message();
}

int native_close(int descriptor) {
#ifdef _WIN32
  return _close(descriptor);
#else
  return ::close(descriptor);
#endif
}

}  // namespace

FileHandle::~FileHandle() {
  if (descriptor_ >= 0) {
    native_close(descriptor_);
    descriptor_ = -1;
  }
}

FileHandle::FileHandle(FileHandle&& other) noexcept : descriptor_(other.descriptor_) {
  other.descriptor_ = -1;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    if (descriptor_ >= 0) native_close(descriptor_);
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
  }
  return *this;
}

Result<FileHandle> FileHandle::open_append(const std::string& path) {
#ifdef _WIN32
  const int descriptor = _open(path.c_str(), _O_CREAT | _O_WRONLY | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
#endif
  if (descriptor < 0) {
    return Status(ErrorCode::IoError, std::string("cannot open for append: ") + os_error_text());
  }
  return FileHandle(descriptor);
}

Result<FileHandle> FileHandle::open_read(const std::string& path) {
#ifdef _WIN32
  const int descriptor = _open(path.c_str(), _O_RDONLY | _O_BINARY);
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
#endif
  if (descriptor < 0) {
    return Status(ErrorCode::IoError, std::string("cannot open for read: ") + os_error_text());
  }
  return FileHandle(descriptor);
}

std::uint64_t FileHandle::size() const noexcept {
  if (descriptor_ < 0) return 0;
#ifdef _WIN32
  struct _stat64 info {};
  if (_fstat64(descriptor_, &info) != 0) return 0;
  return static_cast<std::uint64_t>(info.st_size);
#else
  struct stat info {};
  if (::fstat(descriptor_, &info) != 0) return 0;
  return static_cast<std::uint64_t>(info.st_size);
#endif
}

Status FileHandle::write_all(const std::uint8_t* data, std::size_t size) {
  std::size_t written = 0;
  while (written < size) {
    const std::size_t remaining = size - written;
#ifdef _WIN32
    const unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(remaining, 1U << 20));
    const int result = _write(descriptor_, data + written, chunk);
#else
    const ssize_t result = ::write(descriptor_, data + written, remaining);
#endif
    if (result <= 0) {
      return Status(ErrorCode::IoError, std::string("write failed: ") + os_error_text());
    }
    written += static_cast<std::size_t>(result);
  }
  return Status{};
}

Status FileHandle::read_some(std::uint8_t* data, std::size_t size, std::size_t& read_out) {
  read_out = 0;
  if (size == 0) return Status{};
#ifdef _WIN32
  const unsigned int chunk = static_cast<unsigned int>(std::min<std::size_t>(size, 1U << 20));
  const int result = _read(descriptor_, data, chunk);
#else
  const ssize_t result = ::read(descriptor_, data, size);
#endif
  if (result < 0) {
    return Status(ErrorCode::IoError, std::string("read failed: ") + os_error_text());
  }
  read_out = static_cast<std::size_t>(result);
  return Status{};
}

Status FileHandle::read_all(std::vector<std::uint8_t>& out, std::size_t max_bytes) {
  const std::uint64_t total = size();
  if (total > max_bytes) {
    return Status(ErrorCode::CapacityExceeded, "file exceeds the configured size bound");
  }
  out.clear();
  out.resize(static_cast<std::size_t>(total));
  std::size_t offset = 0;
  while (offset < out.size()) {
    std::size_t read = 0;
    Status status = read_some(out.data() + offset, out.size() - offset, read);
    if (!status.ok()) return status;
    if (read == 0) {
      out.resize(offset);
      return Status(ErrorCode::Truncated, "file shrank while reading");
    }
    offset += read;
  }
  return Status{};
}

Status FileHandle::flush() {
  if (descriptor_ < 0) return Status(ErrorCode::InvalidState, "flush on a closed handle");
#ifdef _WIN32
  if (_commit(descriptor_) != 0) {
    return Status(ErrorCode::IoError, std::string("commit failed: ") + os_error_text());
  }
#else
  if (::fsync(descriptor_) != 0) {
    return Status(ErrorCode::IoError, std::string("fsync failed: ") + os_error_text());
  }
#endif
  return Status{};
}

Status FileHandle::truncate(std::uint64_t size) {
  if (descriptor_ < 0) return Status(ErrorCode::InvalidState, "truncate on a closed handle");
#ifdef _WIN32
  if (_chsize_s(descriptor_, static_cast<__int64>(size)) != 0) {
    return Status(ErrorCode::IoError, std::string("truncate failed: ") + os_error_text());
  }
#else
  if (::ftruncate(descriptor_, static_cast<off_t>(size)) != 0) {
    return Status(ErrorCode::IoError, std::string("truncate failed: ") + os_error_text());
  }
#endif
  return Status{};
}

Status FileHandle::seek(std::uint64_t offset) {
  if (descriptor_ < 0) return Status(ErrorCode::InvalidState, "seek on a closed handle");
#ifdef _WIN32
  if (_lseeki64(descriptor_, static_cast<__int64>(offset), SEEK_SET) < 0) {
    return Status(ErrorCode::IoError, std::string("seek failed: ") + os_error_text());
  }
#else
  if (::lseek(descriptor_, static_cast<off_t>(offset), SEEK_SET) < 0) {
    return Status(ErrorCode::IoError, std::string("seek failed: ") + os_error_text());
  }
#endif
  return Status{};
}

Status FileHandle::close() {
  if (descriptor_ < 0) return Status{};
  const int descriptor = descriptor_;
  descriptor_ = -1;
  if (native_close(descriptor) != 0) {
    return Status(ErrorCode::IoError, std::string("close failed: ") + os_error_text());
  }
  return Status{};
}

LockFile::~LockFile() {
  const Status status = release();
  (void)status;
}

LockFile::LockFile(LockFile&& other) noexcept : descriptor_(other.descriptor_) {
  other.descriptor_ = -1;
}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
  if (this != &other) {
    const Status status = release();
    (void)status;
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
  }
  return *this;
}

Result<LockFile> LockFile::acquire(const std::string& path) {
#ifdef _WIN32
  const int descriptor = _open(path.c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
#endif
  if (descriptor < 0) {
    return Status(ErrorCode::IoError, "cannot open the store lock file: " + os_error_text());
  }
  LockFile lock;
  lock.descriptor_ = descriptor;
#ifdef _WIN32
  if (_locking(descriptor, _LK_NBLCK, 1) != 0) {
    lock.descriptor_ = -1;
    native_close(descriptor);
    return Status(ErrorCode::AlreadyExists, "the store root is locked by another process");
  }
#else
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    lock.descriptor_ = -1;
    native_close(descriptor);
    return Status(ErrorCode::AlreadyExists, "the store root is locked by another process");
  }
#endif
  return lock;
}

Status LockFile::release() {
  if (descriptor_ < 0) return Status{};
  const int descriptor = descriptor_;
  descriptor_ = -1;
#ifdef _WIN32
  (void)_locking(descriptor, _LK_UNLCK, 1);
#else
  (void)::flock(descriptor, LOCK_UN);
#endif
  native_close(descriptor);
  return Status{};
}

Status ensure_directory(const std::string& path) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error && error.value() != 0) {
    return Status(ErrorCode::IoError, "cannot stat directory: " + error.message());
  }
  if (exists) {
    if (!std::filesystem::is_directory(path, error)) {
      return Status(ErrorCode::InvalidArgument, "store root exists and is not a directory");
    }
    return Status{};
  }
  std::filesystem::create_directories(path, error);
  if (error) {
    return Status(ErrorCode::IoError, "cannot create directory: " + error.message());
  }
  return Status{};
}

Status sync_directory(const std::string& path) {
#ifndef _WIN32
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Status(ErrorCode::IoError, "cannot open directory for sync");
  }
  const int result = ::fsync(descriptor);
  native_close(descriptor);
  if (result != 0) {
    return Status(ErrorCode::IoError, "directory fsync failed");
  }
#else
  // Windows has no portable directory handle flush; the rename is still
  // atomic and the file contents were flushed before it.
  (void)path;
#endif
  return Status{};
}

Status file_exists(const std::string& path, bool& exists) {
  std::error_code error;
  exists = std::filesystem::exists(path, error);
  if (error) {
    return Status(ErrorCode::IoError, "cannot stat path: " + error.message());
  }
  return Status{};
}

Status remove_file(const std::string& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    return Status(ErrorCode::IoError, "cannot remove file: " + error.message());
  }
  return Status{};
}

Status list_directory(const std::string& path, std::vector<std::string>& names) {
  names.clear();
  std::error_code error;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return Status(ErrorCode::IoError, "cannot enumerate directory: " + error.message());
  }
  for (const auto& entry : iterator) {
    if (names.size() >= kMaxCollectionItems) {
      return Status(ErrorCode::CapacityExceeded, "directory holds too many entries");
    }
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return Status{};
}

Status read_file(const std::string& path, std::vector<std::uint8_t>& out, std::size_t max_bytes) {
  auto handle = FileHandle::open_read(path);
  if (!handle.ok()) return handle.status();
  return handle.value().read_all(out, max_bytes);
}

Status write_file_atomic(const std::string& path, const std::uint8_t* data, std::size_t size) {
  const std::string temporary = path + ".staging";
  {
    std::error_code error;
    std::filesystem::remove(temporary, error);
  }
#ifdef _WIN32
  const int descriptor =
      _open(temporary.c_str(), _O_CREAT | _O_WRONLY | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
  const int descriptor = ::open(temporary.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
#endif
  if (descriptor < 0) {
    return Status(ErrorCode::IoError, "cannot create staging file: " + os_error_text());
  }
  FileHandle handle = FileHandle::adopt(descriptor);
  Status status = handle.write_all(data, size);
  if (!status.ok()) {
    handle.close();
    remove_file(temporary);
    return status;
  }
  status = handle.flush();
  if (!status.ok()) {
    handle.close();
    remove_file(temporary);
    return status;
  }
  status = handle.close();
  if (!status.ok()) {
    remove_file(temporary);
    return status;
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    remove_file(temporary);
    return Status(ErrorCode::IoError, "atomic rename failed: " + error.message());
  }
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) return sync_directory(parent.string());
  return Status{};
}

Status validate_relative_path(std::string_view name) {
  if (name.empty() || name.size() > kMaxPathBytes) {
    return Status(ErrorCode::InvalidArgument, "path component is empty or too long");
  }
  if (!is_text_safe(name)) {
    return Status(ErrorCode::InvalidArgument, "path component carries control characters");
  }
  const std::filesystem::path candidate{std::string(name)};
  if (candidate.is_absolute()) {
    return Status(ErrorCode::InvalidArgument, "absolute path components are rejected");
  }
  for (const auto& part : candidate) {
    if (part == "..") {
      return Status(ErrorCode::InvalidArgument, "path traversal is rejected");
    }
  }
  if (name.find(':') != std::string_view::npos) {
    return Status(ErrorCode::InvalidArgument, "drive-qualified path components are rejected");
  }
  return Status{};
}

}  // namespace detail
}  // namespace dmf
