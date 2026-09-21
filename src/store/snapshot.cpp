// Degraded Mode Fabric - snapshot framing implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "store/snapshot.hpp"

#include <cstdio>

namespace dmf {
namespace detail {

namespace {

std::uint32_t read_u32_at(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint64_t read_u64_at(const std::uint8_t* bytes) noexcept {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  return value;
}

bool magic_at(const std::uint8_t* bytes, const std::uint8_t magic[4]) noexcept {
  return bytes[0] == magic[0] && bytes[1] == magic[1] && bytes[2] == magic[2] && bytes[3] == magic[3];
}

Status parse_sequence_name(std::string_view name, std::string_view prefix, std::string_view suffix,
                           std::uint64_t& sequence) {
  if (name.size() <= prefix.size() + suffix.size()) {
    return Status(ErrorCode::InvalidArgument, "name is too short");
  }
  if (name.substr(0, prefix.size()) != prefix) {
    return Status(ErrorCode::InvalidArgument, "name prefix does not match");
  }
  if (name.substr(name.size() - suffix.size()) != suffix) {
    return Status(ErrorCode::InvalidArgument, "name suffix does not match");
  }
  const std::string_view digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (digits.empty() || digits.size() > 20) {
    return Status(ErrorCode::InvalidArgument, "sequence field is empty or too long");
  }
  std::uint64_t value = 0;
  for (const char raw : digits) {
    if (raw < '0' || raw > '9') {
      return Status(ErrorCode::InvalidArgument, "sequence field is not decimal");
    }
    const auto shifted = checked_mul(value, std::uint64_t{10});
    if (!shifted.has_value()) return Status(ErrorCode::Overflow, "sequence field overflows");
    const auto next = checked_add(*shifted, static_cast<std::uint64_t>(raw - '0'));
    if (!next.has_value()) return Status(ErrorCode::Overflow, "sequence field overflows");
    value = *next;
  }
  sequence = value;
  return Status{};
}

std::string format_sequence_name(std::string_view prefix, std::uint64_t sequence,
                                 std::string_view suffix) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*s%020llu%.*s", static_cast<int>(prefix.size()),
                prefix.data(), static_cast<unsigned long long>(sequence),
                static_cast<int>(suffix.size()), suffix.data());
  return std::string(buffer);
}

}  // namespace

std::vector<std::uint8_t> encode_snapshot(const SnapshotHeader& header,
                                          const std::vector<std::uint8_t>& payload) {
  ByteWriter writer(kSnapshotHeaderSize + payload.size() + kSnapshotFooterSize);
  for (const std::uint8_t byte : kSnapshotMagic) writer.u8(byte);
  writer.u32(header.version);
  writer.u64(header.sequence);
  writer.u64(static_cast<std::uint64_t>(payload.size()));
  const std::uint32_t payload_crc = crc32c(payload.data(), payload.size());
  writer.u32(payload_crc);
  writer.u32(crc32c(writer.data(), writer.size()));
  writer.raw(payload.data(), payload.size());
  for (const std::uint8_t byte : kSnapshotFooterMagic) writer.u8(byte);
  writer.u32(header.version);
  writer.u32(payload_crc);
  writer.u32(crc32c(writer.data() + kSnapshotHeaderSize + payload.size(), 12));
  return writer.bytes();
}

Status decode_snapshot(const std::vector<std::uint8_t>& bytes, SnapshotHeader& header,
                       std::vector<std::uint8_t>& payload, std::size_t max_payload) {
  if (bytes.size() < kSnapshotHeaderSize + kSnapshotFooterSize) {
    return Status(ErrorCode::Truncated, "snapshot is shorter than its framing");
  }
  const std::uint8_t* data = bytes.data();
  if (!magic_at(data, kSnapshotMagic)) {
    return Status(ErrorCode::Corrupt, "snapshot magic does not match");
  }
  if (read_u32_at(data + 28) != crc32c(data, 28)) {
    return Status(ErrorCode::IntegrityFailure, "snapshot header fails its integrity check");
  }
  SnapshotHeader candidate;
  candidate.version = read_u32_at(data + 4);
  candidate.sequence = read_u64_at(data + 8);
  candidate.payload_length = read_u64_at(data + 16);
  candidate.payload_crc = read_u32_at(data + 24);
  if (candidate.version != kDurableFormatVersion) {
    return Status(ErrorCode::VersionUnsupported, "snapshot format version is not supported");
  }
  if (candidate.payload_length > max_payload) {
    return Status(ErrorCode::CapacityExceeded, "snapshot declares an oversized payload");
  }
  const auto expected = checked_add(
      checked_add(static_cast<std::uint64_t>(kSnapshotHeaderSize), candidate.payload_length)
          .value_or(std::numeric_limits<std::uint64_t>::max()),
      static_cast<std::uint64_t>(kSnapshotFooterSize));
  if (!expected.has_value() || *expected != static_cast<std::uint64_t>(bytes.size())) {
    return Status(ErrorCode::Corrupt, "snapshot length does not match its declared payload");
  }
  const std::uint8_t* payload_bytes = data + kSnapshotHeaderSize;
  const std::size_t payload_size = static_cast<std::size_t>(candidate.payload_length);
  if (crc32c(payload_bytes, payload_size) != candidate.payload_crc) {
    return Status(ErrorCode::IntegrityFailure, "snapshot payload fails its integrity check");
  }
  const std::uint8_t* footer = payload_bytes + payload_size;
  if (!magic_at(footer, kSnapshotFooterMagic)) {
    return Status(ErrorCode::Corrupt, "snapshot footer magic does not match");
  }
  if (read_u32_at(footer + 4) != kDurableFormatVersion) {
    return Status(ErrorCode::VersionUnsupported, "snapshot footer version is not supported");
  }
  if (read_u32_at(footer + 8) != candidate.payload_crc) {
    return Status(ErrorCode::IntegrityFailure, "snapshot footer disagrees with the header");
  }
  if (read_u32_at(footer + 12) != crc32c(footer, 12)) {
    return Status(ErrorCode::IntegrityFailure, "snapshot footer fails its integrity check");
  }
  payload.assign(payload_bytes, payload_bytes + payload_size);
  header = candidate;
  return Status{};
}

std::string snapshot_file_name(std::uint64_t sequence) {
  return format_sequence_name("snapshot-", sequence, ".dmfs");
}

Status parse_snapshot_file_name(std::string_view name, std::uint64_t& sequence) {
  return parse_sequence_name(name, "snapshot-", ".dmfs", sequence);
}

std::string journal_file_name(std::uint64_t base_sequence) {
  return format_sequence_name("journal-", base_sequence, ".dmfj");
}

Status parse_journal_file_name(std::string_view name, std::uint64_t& base_sequence) {
  return parse_sequence_name(name, "journal-", ".dmfj", base_sequence);
}

}  // namespace detail
}  // namespace dmf
