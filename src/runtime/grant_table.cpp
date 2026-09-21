// Degraded Mode Fabric - grant table index.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The index is derived state only. It is rebuilt from the durable grant table
// after a snapshot prunes it, so a stale index can never outlive the records it
// points at.
#include "dmf/runtime.hpp"

namespace dmf {

bool is_valid(PrincipalRole value) noexcept {
  switch (value) {
    case PrincipalRole::Observer:
    case PrincipalRole::Operator:
    case PrincipalRole::Administrator:
      return true;
  }
  return false;
}

std::string_view to_string(PrincipalRole value) noexcept {
  switch (value) {
    case PrincipalRole::Observer: return "OBSERVER";
    case PrincipalRole::Operator: return "OPERATOR";
    case PrincipalRole::Administrator: return "ADMINISTRATOR";
  }
  return "UNRECOGNISED";
}

}  // namespace dmf
