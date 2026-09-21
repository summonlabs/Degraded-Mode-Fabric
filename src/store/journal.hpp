// Degraded Mode Fabric - journal framing (internal).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Framing rules
// -------------
//   * every fixed header carries its own CRC-32C, so a torn payload can be told
//     apart from a corrupted header;
//   * a record's payload carries its own CRC, so a torn tail is detectable at
//     byte granularity;
//   * declared payload lengths are refused before allocation;
//   * sequences are strictly incrementing, so replay and regression are
//     rejected rather than tolerated.
#ifndef DMF_SRC_STORE_JOURNAL_HPP
#define DMF_SRC_STORE_JOURNAL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "dmf/core.hpp"
#include "dmf/store.hpp"
#include "store/file_io.hpp"

namespace dmf {
namespace detail {

inline constexpr std::uint8_t kJournalMagic[4] = {'D', 'M', 'F', 'J'};
inline constexpr std::uint8_t kRecordMagic[4] = {'D', 'M', 'F', 'R'};
inline constexpr std::uint8_t kJournalFooterMagic[4] = {'D', 'M', 'F', 'E'};

inline constexpr std::size_t kJournalHeaderSize = 28;
inline constexpr std::size_t kRecordHeaderSize = 28;
inline constexpr std::size_t kRecordTrailerSize = 4;
inline constexpr std::size_t kJournalFooterSize = 28;
inline constexpr std::uint32_t kMaxRecordPayload = 4U << 20;

struct JournalHeader {
  std::uint32_t version = kDurableFormatVersion;
  BootIncarnation boot{};
  std::uint64_t base_sequence = 0;
};

struct RecordHeader {
  std::uint32_t version = kDurableFormatVersion;
  RecordType type = RecordType::AccountingCheckpoint;
  std::uint16_t flags = 0;
  std::uint64_t sequence = 0;
  std::uint32_t payload_length = 0;
};

struct JournalFooter {
  std::uint64_t record_count = 0;
  std::uint64_t last_sequence = 0;
};

/// Encodes a fixed-size journal file header.
std::vector<std::uint8_t> encode_journal_header(const JournalHeader& header);
/// Decodes and fully validates a fixed-size journal file header, including its
/// trailing CRC and the durable format version.
Status decode_journal_header(const std::uint8_t* bytes, std::size_t size, JournalHeader& header);

/// Encodes one record: header, payload, payload CRC.
std::vector<std::uint8_t> encode_record(const RecordHeader& header,
                                        const std::vector<std::uint8_t>& payload);
/// Decodes and validates a fixed-size record header, including its trailing CRC.
Status decode_record_header(const std::uint8_t* bytes, std::size_t size, RecordHeader& header);

/// Encodes the segment footer written on a clean close.
std::vector<std::uint8_t> encode_journal_footer(const JournalFooter& footer);
/// Decodes and validates the segment footer, including its trailing CRC.
Status decode_journal_footer(const std::uint8_t* bytes, std::size_t size, JournalFooter& footer);

}  // namespace detail
}  // namespace dmf

#endif  // DMF_SRC_STORE_JOURNAL_HPP
