// Degraded Mode Fabric - transactional snapshot framing (internal).
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef DMF_SRC_STORE_SNAPSHOT_HPP
#define DMF_SRC_STORE_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dmf/core.hpp"

namespace dmf {
namespace detail {

inline constexpr std::uint8_t kSnapshotMagic[4] = {'D', 'M', 'F', 'S'};
inline constexpr std::uint8_t kSnapshotFooterMagic[4] = {'D', 'M', 'F', 'F'};
inline constexpr std::size_t kSnapshotHeaderSize = 32;
inline constexpr std::size_t kSnapshotFooterSize = 16;

struct SnapshotHeader {
  std::uint32_t version = kDurableFormatVersion;
  std::uint64_t sequence = 0;
  std::uint64_t payload_length = 0;
  std::uint32_t payload_crc = 0;
};

std::vector<std::uint8_t> encode_snapshot(const SnapshotHeader& header,
                                          const std::vector<std::uint8_t>& payload);
/// Validates the snapshot end to end: header CRC, version, declared length
/// against the real file length, payload CRC, footer magic and footer CRC.
Status decode_snapshot(const std::vector<std::uint8_t>& bytes, SnapshotHeader& header,
                       std::vector<std::uint8_t>& payload, std::size_t max_payload);

std::string snapshot_file_name(std::uint64_t sequence);
Status parse_snapshot_file_name(std::string_view name, std::uint64_t& sequence);
std::string journal_file_name(std::uint64_t base_sequence);
Status parse_journal_file_name(std::string_view name, std::uint64_t& base_sequence);

}  // namespace detail
}  // namespace dmf

#endif  // DMF_SRC_STORE_SNAPSHOT_HPP
