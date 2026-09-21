// Degraded Mode Fabric - version identity.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef DMF_VERSION_HPP
#define DMF_VERSION_HPP

#include <cstdint>
#include <string_view>

#define DMF_VERSION_MAJOR 1
#define DMF_VERSION_MINOR 0
#define DMF_VERSION_PATCH 0
#define DMF_VERSION_STRING "1.0.0"

namespace dmf {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::uint32_t kVersionCombined = 1U * 10000U + 0U * 100U + 0U;
inline constexpr std::string_view kVersionString = DMF_VERSION_STRING;

/// Durable format generation. Bumped only when an on-disk layout changes in a
/// way that older runtimes cannot interpret safely.
inline constexpr std::uint32_t kDurableFormatVersion = 1;

/// Wire protocol version bound into every frame header.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

}  // namespace dmf

#endif  // DMF_VERSION_HPP
