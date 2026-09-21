// Degraded Mode Fabric - shared command line helpers for the tools.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
#ifndef DMF_TOOLS_TOOL_SUPPORT_HPP
#define DMF_TOOLS_TOOL_SUPPORT_HPP

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "dmf/core.hpp"
#include "dmf/runtime.hpp"

namespace dmf::tool {

/// Protocol body helpers, re-exported so a tool needs one namespace.
using dmf::decode_object_body;
using dmf::encode_attempt_body;
using dmf::encode_fence_body;
using dmf::encode_id_body;
using dmf::encode_object_body;

/// Minimal, order independent flag parser.
///
/// Both "--name value" and "--name=value" are accepted. A bare "--name" takes
/// the following token as its value unless the name is one of the declared
/// boolean switches, so a switch can never silently swallow a positional
/// argument and a value can never be mistaken for a switch.
class Args {
 public:
  Args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
      std::string token = argv[i];
      if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
        const std::string name = token.substr(2);
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
          values_[name.substr(0, equals)] = name.substr(equals + 1);
          continue;
        }
        if (is_switch(name)) {
          values_[name] = "true";
          continue;
        }
        if (i + 1 < argc) {
          const std::string next = argv[i + 1];
          if (next.empty() || next[0] != '-') {
            values_[name] = next;
            ++i;
            continue;
          }
        }
        values_[name] = "";
      } else if (!token.empty() && token[0] == '-') {
        values_[token.substr(1)] = "true";
      } else {
        positional_.push_back(token);
      }
    }
  }

  [[nodiscard]] bool has(const std::string& name) const { return values_.find(name) != values_.end(); }

  [[nodiscard]] std::string get(const std::string& name, const std::string& fallback = {}) const {
    const auto position = values_.find(name);
    if (position == values_.end()) return fallback;
    return position->second;
  }

  [[nodiscard]] std::uint64_t get_u64(const std::string& name, std::uint64_t fallback) const {
    const auto position = values_.find(name);
    if (position == values_.end()) return fallback;
    std::uint64_t value = 0;
    for (const char raw : position->second) {
      if (raw < '0' || raw > '9') return fallback;
      value = value * 10U + static_cast<std::uint64_t>(raw - '0');
    }
    return value;
  }

  [[nodiscard]] std::uint32_t get_u32(const std::string& name, std::uint32_t fallback) const {
    return static_cast<std::uint32_t>(get_u64(name, fallback));
  }

  [[nodiscard]] std::uint16_t get_port(const std::string& name, std::uint16_t fallback = 0) const {
    const std::uint64_t value = get_u64(name, fallback);
    if (value > 65535) return fallback;
    return static_cast<std::uint16_t>(value);
  }

  [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }

 private:
  /// Switches that never take a value.
  [[nodiscard]] static bool is_switch(const std::string& name) {
    static const char* const kSwitches[] = {
        "help",        "hold",           "apply",       "ack",
        "restore",     "allow-anonymous", "no-protect-first",
        "exit-after-ready", "reapply-attempt", "quiet",   "json"};
    for (const char* candidate : kSwitches) {
      if (name == candidate) return true;
    }
    return false;
  }

  std::map<std::string, std::string> values_{};
  std::vector<std::string> positional_{};
};

inline std::string outcome_text(DecisionOutcome outcome) { return std::string(to_string(outcome)); }

inline std::string hex64(std::uint64_t value) { return to_hex(value); }

/// Reads one line from standard input. Returns false at end of input.
inline bool read_stdin_line(std::string& line) {
  if (!std::getline(std::cin, line)) return false;
  return true;
}

/// Maps a topology name to its enum value.
inline bool parse_topology(const std::string& text, FabricTopologyClass& out) {
  if (text == "full") {
    out = FabricTopologyClass::FullMesh;
  } else if (text == "partial") {
    out = FabricTopologyClass::PartialMesh;
  } else if (text == "partitioned") {
    out = FabricTopologyClass::Partitioned;
  } else if (text == "isolated") {
    out = FabricTopologyClass::Isolated;
  } else {
    return false;
  }
  return true;
}

inline bool parse_service_class(const std::string& text, ServiceClass& out) {
  if (text == "protected") {
    out = ServiceClass::Protected;
  } else if (text == "standard") {
    out = ServiceClass::Standard;
  } else if (text == "best-effort" || text == "besteffort") {
    out = ServiceClass::BestEffort;
  } else if (text == "scavenger") {
    out = ServiceClass::Scavenger;
  } else {
    return false;
  }
  return true;
}

inline bool parse_fence_reason(const std::string& text, FenceReason& out) {
  if (text == "manual") {
    out = FenceReason::Manual;
  } else if (text == "revocation") {
    out = FenceReason::Revocation;
  } else if (text == "authority-unprovable") {
    out = FenceReason::AuthorityUnprovable;
  } else if (text == "session-loss") {
    out = FenceReason::SessionLoss;
  } else {
    return false;
  }
  return true;
}

/// Request body for messages that address exactly one contract.
inline std::vector<std::uint8_t> encode_contract_request(std::uint64_t contract) {
  return encode_id_body(contract);
}

inline void print_error(const Status& status) {
  std::cout << "DMF_ERROR code=" << to_string(status.code()) << " detail=" << status.detail()
            << std::endl;
}

/// Builds the standard demonstration policy used by the tools and the
/// walkthrough: protected obligations first, bounded concessions elsewhere.
inline Policy standard_policy(PolicyId id, PolicyGeneration generation) {
  Policy policy;
  policy.id = id;
  policy.generation = generation;
  policy.protect_first = true;
  policy.default_ttl_ticks = Tick{50};
  policy.minimum_ttl_ticks = Tick{5};
  policy.max_concessions_per_contract = 4;
  policy.minimum_dwell_ticks = 0;
  policy.search_node_budget = 20000;
  policy.evidence_freshness_ticks = 50;
  policy.unknown_evidence_action = DegradeAction::Refuse;
  policy.stale_evidence_action = DegradeAction::Refuse;
  policy.conflict_evidence_action = DegradeAction::Refuse;
  policy.invalid_evidence_action = DegradeAction::Refuse;
  policy.unsupported_evidence_action = DegradeAction::Refuse;

  PolicyRule degrade;
  degrade.id = RuleId::from_value(1);
  degrade.precedence = 10;
  degrade.action = DegradeAction::Degrade;
  policy.rules.push_back(degrade);

  PolicyRule protect;
  protect.id = RuleId::from_value(2);
  protect.precedence = 1;
  protect.service_class = ServiceClass::Protected;
  protect.action = DegradeAction::Protect;
  policy.rules.push_back(protect);

  ClassProfile protected_profile;
  protected_profile.service_class = ServiceClass::Protected;
  protected_profile.may_be_degraded = false;
  protected_profile.ceiling.set_max_concessions(0);
  policy.class_profiles.push_back(protected_profile);

  for (const ServiceClass service_class :
       {ServiceClass::Standard, ServiceClass::BestEffort, ServiceClass::Scavenger}) {
    ClassProfile profile;
    profile.service_class = service_class;
    profile.may_be_degraded = true;
    profile.ceiling.set_max_concessions(4);
    policy.class_profiles.push_back(profile);
  }
  return policy;
}

/// Builds a contract with the two contended/observed guarantees the fixtures use.
inline ServiceContract make_contract(ContractId id, ContractGeneration generation, ScopeId scope,
                                     SubjectId subject, SubjectGeneration subject_generation,
                                     ServiceClass service_class, std::uint32_t priority,
                                     std::uint64_t bandwidth_kbps, std::uint64_t latency_us,
                                     std::uint64_t bandwidth_floor_kbps,
                                     std::uint32_t max_concessions) {
  ServiceContract contract;
  contract.id = id;
  contract.generation = generation;
  contract.scope = scope;
  contract.subject = subject;
  contract.subject_generation = subject_generation;
  contract.service_class = service_class;
  contract.non_degradable = service_class == ServiceClass::Protected;
  contract.priority = priority;
  (void)contract.original.insert(GuaranteeKind::Bandwidth, bandwidth_kbps);
  (void)contract.original.insert(GuaranteeKind::LatencyP99, latency_us);
  (void)contract.original.insert(GuaranteeKind::PathDiversity, 1);
  (void)contract.original.insert(GuaranteeKind::Reachability, 900000);
  if (!contract.protected_obligation()) {
    (void)contract.envelope.insert(ConcessionBound{GuaranteeKind::Bandwidth, bandwidth_floor_kbps});
    (void)contract.envelope.insert(ConcessionBound{GuaranteeKind::LatencyP99, 100000});
    contract.envelope.set_max_concessions(max_concessions);
  }
  return contract;
}

}  // namespace dmf::tool

#endif  // DMF_TOOLS_TOOL_SUPPORT_HPP
