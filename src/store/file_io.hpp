// Degraded Mode Fabric - durable file primitives (internal).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Small RAII wrapper over the platform file API so the store can guarantee
// append/flush/commit ordering. Not a public header: no part of it is installed.
#ifndef DMF_SRC_STORE_FILE_IO_HPP
#define DMF_SRC_STORE_FILE_IO_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dmf/core.hpp"

namespace dmf {
namespace detail {

/// Owns a file descriptor. Never copies; moving transfers ownership.
class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle();
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  static Result<FileHandle> open_append(const std::string& path);
  static Result<FileHandle> open_read(const std::string& path);
  /// Adopts an already-open descriptor, taking ownership of it.
  static FileHandle adopt(int descriptor) noexcept { return FileHandle(descriptor); }

  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }
  [[nodiscard]] std::uint64_t size() const noexcept;

  Status write_all(const std::uint8_t* data, std::size_t size);
  /// Reads up to \p size bytes. read_out receives the number actually read,
  /// which may be short only at end of file.
  Status read_some(std::uint8_t* data, std::size_t size, std::size_t& read_out);
  Status read_all(std::vector<std::uint8_t>& out, std::size_t max_bytes);
  Status flush();
  /// Truncates the file to \p size bytes. Used only to discard a genuine torn
  /// journal tail after the recovery path has proven it is a torn tail.
  Status truncate(std::uint64_t size);
  Status seek(std::uint64_t offset);
  Status close();

 private:
  explicit FileHandle(int descriptor) noexcept : descriptor_(descriptor) {}
  int descriptor_ = -1;
};

/// Exclusive advisory lock on a store root. Two runtimes can never open the
/// same root for writing at once, which is the only way the append-only journal
/// and its transactional snapshots stay coherent across processes.
class LockFile {
 public:
  LockFile() = default;
  ~LockFile();
  LockFile(LockFile&& other) noexcept;
  LockFile& operator=(LockFile&& other) noexcept;
  LockFile(const LockFile&) = delete;
  LockFile& operator=(const LockFile&) = delete;

  /// Creates and locks \p path exclusively. Fails with AlreadyExists when
  /// another process holds it.
  static Result<LockFile> acquire(const std::string& path);
  Status release();
  [[nodiscard]] bool held() const noexcept { return descriptor_ >= 0; }

 private:
  int descriptor_ = -1;
};

Status ensure_directory(const std::string& path);
Status sync_directory(const std::string& path);
Status file_exists(const std::string& path, bool& exists);
Status remove_file(const std::string& path);
Status list_directory(const std::string& path, std::vector<std::string>& names);
Status read_file(const std::string& path, std::vector<std::uint8_t>& out, std::size_t max_bytes);
/// Writes to a temporary sibling, flushes it, then renames over the target so a
/// crash can never leave a partially written file in place.
Status write_file_atomic(const std::string& path, const std::uint8_t* data, std::size_t size);
/// Rejects absolute paths and any component equal to ".." so a hostile config
/// can never escape the store root.
Status validate_relative_path(std::string_view name);

}  // namespace detail
}  // namespace dmf

#endif  // DMF_SRC_STORE_FILE_IO_HPP
