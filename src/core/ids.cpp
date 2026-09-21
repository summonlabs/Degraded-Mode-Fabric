// Degraded Mode Fabric - identity helpers.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#include "dmf/ids.hpp"

namespace dmf {

bool is_valid(OriginClass value) noexcept {
  switch (value) {
    case OriginClass::Unspecified:
    case OriginClass::Real:
    case OriginClass::Synthetic:
    case OriginClass::Unsupported:
      return true;
  }
  return false;
}

std::string_view to_string(OriginClass value) noexcept {
  switch (value) {
    case OriginClass::Unspecified: return "UNSPECIFIED";
    case OriginClass::Real: return "REAL";
    case OriginClass::Synthetic: return "SYNTHETIC";
    case OriginClass::Unsupported: return "UNSUPPORTED";
  }
  return "UNRECOGNISED";
}

void write_id(ByteWriter& writer, std::uint64_t value) noexcept { writer.u64(value); }

std::uint64_t read_id(ByteReader& reader) noexcept { return reader.u64(); }

}  // namespace dmf
