// Degraded Mode Fabric - journal framing implementation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "store/journal.hpp"

namespace dmf {
namespace detail {

namespace {

void write_magic(ByteWriter& writer, const std::uint8_t magic[4]) noexcept {
  writer.u8(magic[0]);
  writer.u8(magic[1]);
  writer.u8(magic[2]);
  writer.u8(magic[3]);
}

bool magic_matches(const std::uint8_t* bytes, const std::uint8_t magic[4]) noexcept {
  return bytes[0] == magic[0] && bytes[1] == magic[1] && bytes[2] == magic[2] &&
         bytes[3] == magic[3];
}

/// Validates the trailing CRC-32C over bytes[0, size-4).
Status check_trailing_crc(const std::uint8_t* bytes, std::size_t size, const char* what) {
  if (size < 4) return Status(ErrorCode::Truncated, std::string(what) + " is shorter than its CRC");
  const std::uint32_t stored =
      static_cast<std::uint32_t>(bytes[size - 4]) |
      (static_cast<std::uint32_t>(bytes[size - 3]) << 8) |
      (static_cast<std::uint32_t>(bytes[size - 2]) << 16) |
      (static_cast<std::uint32_t>(bytes[size - 1]) << 24);
  const std::uint32_t actual = crc32c(bytes, size - 4);
  if (stored != actual) {
    return Status(ErrorCode::IntegrityFailure, std::string(what) + " fails its integrity check");
  }
  return Status{};
}

}  // namespace

std::vector<std::uint8_t> encode_journal_header(const JournalHeader& header) {
  ByteWriter writer(64);
  write_magic(writer, kJournalMagic);
  writer.u32(header.version);
  writer.u64(header.boot.value());
  writer.u64(header.base_sequence);
  writer.u32(crc32c(writer.data(), writer.size()));
  return writer.bytes();
}

Status decode_journal_header(const std::uint8_t* bytes, std::size_t size, JournalHeader& header) {
  if (size != kJournalHeaderSize) {
    return Status(ErrorCode::Truncated, "journal header has the wrong size");
  }
  if (!magic_matches(bytes, kJournalMagic)) {
    return Status(ErrorCode::Corrupt, "journal header magic does not match");
  }
  Status status = check_trailing_crc(bytes, size, "journal header");
  if (!status.ok()) return status;
  ByteReader reader(bytes, size);
  reader.raw(4);
  JournalHeader candidate;
  candidate.version = reader.u32();
  candidate.boot = BootIncarnation::from_value(reader.u64());
  candidate.base_sequence = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  if (candidate.version != kDurableFormatVersion) {
    return Status(ErrorCode::VersionUnsupported, "journal format version is not supported");
  }
  if (!candidate.boot.valid()) {
    return Status(ErrorCode::Corrupt, "journal header carries an unset boot incarnation");
  }
  header = candidate;
  return Status{};
}

std::vector<std::uint8_t> encode_record(const RecordHeader& header,
                                        const std::vector<std::uint8_t>& payload) {
  ByteWriter writer(kRecordHeaderSize + payload.size() + kRecordTrailerSize);
  write_magic(writer, kRecordMagic);
  writer.u32(header.version);
  writer.u16(static_cast<std::uint16_t>(header.type));
  writer.u16(header.flags);
  writer.u64(header.sequence);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u32(crc32c(writer.data(), writer.size()));
  writer.raw(payload.data(), payload.size());
  writer.u32(crc32c(payload.data(), payload.size()));
  return writer.bytes();
}

Status decode_record_header(const std::uint8_t* bytes, std::size_t size, RecordHeader& header) {
  if (size != kRecordHeaderSize) {
    return Status(ErrorCode::Truncated, "record header has the wrong size");
  }
  if (!magic_matches(bytes, kRecordMagic)) {
    return Status(ErrorCode::Corrupt, "record magic does not match");
  }
  Status status = check_trailing_crc(bytes, size, "record header");
  if (!status.ok()) return status;
  ByteReader reader(bytes, size);
  reader.raw(4);
  RecordHeader candidate;
  candidate.version = reader.u32();
  candidate.type = read_enum<RecordType>(reader, RecordType::AccountingCheckpoint);
  candidate.flags = reader.u16();
  candidate.sequence = reader.u64();
  candidate.payload_length = reader.u32();
  if (!reader.ok()) return Status(reader.error());
  if (candidate.version != kDurableFormatVersion) {
    return Status(ErrorCode::VersionUnsupported, "record format version is not supported");
  }
  if (candidate.payload_length > kMaxRecordPayload) {
    return Status(ErrorCode::CapacityExceeded, "record declares an oversized payload");
  }
  if (candidate.flags != 0) {
    return Status(ErrorCode::Invalid, "record header carries unknown flags");
  }
  header = candidate;
  return Status{};
}

std::vector<std::uint8_t> encode_journal_footer(const JournalFooter& footer) {
  ByteWriter writer(kJournalFooterSize);
  write_magic(writer, kJournalFooterMagic);
  writer.u32(kDurableFormatVersion);
  writer.u64(footer.record_count);
  writer.u64(footer.last_sequence);
  writer.u32(crc32c(writer.data(), writer.size()));
  return writer.bytes();
}

Status decode_journal_footer(const std::uint8_t* bytes, std::size_t size, JournalFooter& footer) {
  if (size != kJournalFooterSize) {
    return Status(ErrorCode::Truncated, "journal footer has the wrong size");
  }
  if (!magic_matches(bytes, kJournalFooterMagic)) {
    return Status(ErrorCode::Corrupt, "journal footer magic does not match");
  }
  Status status = check_trailing_crc(bytes, size, "journal footer");
  if (!status.ok()) return status;
  ByteReader reader(bytes, size);
  reader.raw(4);
  const std::uint32_t version = reader.u32();
  JournalFooter candidate;
  candidate.record_count = reader.u64();
  candidate.last_sequence = reader.u64();
  if (!reader.ok()) return Status(reader.error());
  if (version != kDurableFormatVersion) {
    return Status(ErrorCode::VersionUnsupported, "journal footer version is not supported");
  }
  footer = candidate;
  return Status{};
}

}  // namespace detail
}  // namespace dmf
