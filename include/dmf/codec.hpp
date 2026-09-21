// Degraded Mode Fabric - canonical serialisation for every durable and wire type.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// One canonical encoding serves both the durable store and the wire protocol, so
// a record written to disk and a message on the socket are byte-identical for
// equal values. Decoding is total (every byte string maps to a value or a
// Status) and validation happens before materialisation: element counts are
// bounded first, then the elements are read.
//
// Decoders are deliberately not noexcept. They materialise values, so an
// allocation failure must propagate as an exception rather than terminate the
// process; every other failure mode is a returned Status. Encoders keep their
// noexcept guarantee because the writer checks every bound before it writes.
#ifndef DMF_CODEC_HPP
#define DMF_CODEC_HPP

#include <cstdint>

#include "dmf/authority.hpp"
#include "dmf/contract.hpp"
#include "dmf/core.hpp"
#include "dmf/decision.hpp"
#include "dmf/evidence.hpp"
#include "dmf/grant.hpp"
#include "dmf/policy.hpp"

namespace dmf {

#define DMF_DECLARE_CODEC(Type)                                        \
  void encode(ByteWriter& writer, const Type& value);         \
  Status decode(ByteReader& reader, Type& value);

DMF_DECLARE_CODEC(Guarantee)
DMF_DECLARE_CODEC(GuaranteeSet)
DMF_DECLARE_CODEC(Concession)
DMF_DECLARE_CODEC(GuaranteeWithdrawal)
DMF_DECLARE_CODEC(GuaranteeDelta)
DMF_DECLARE_CODEC(ConcessionBound)
DMF_DECLARE_CODEC(GuaranteeEnvelope)
DMF_DECLARE_CODEC(ServiceContract)
DMF_DECLARE_CODEC(EvidenceItem)
DMF_DECLARE_CODEC(EvidenceVector)
DMF_DECLARE_CODEC(CapabilitySnapshot)
DMF_DECLARE_CODEC(AuthorityVector)
DMF_DECLARE_CODEC(AuthorityBinding)
DMF_DECLARE_CODEC(GuaranteeSupport)
DMF_DECLARE_CODEC(AllocationWork)
DMF_DECLARE_CODEC(AllocationEntry)
DMF_DECLARE_CODEC(AllocationPlan)
DMF_DECLARE_CODEC(Decision)
DMF_DECLARE_CODEC(RestorationPrecondition)
DMF_DECLARE_CODEC(PreconditionEvaluation)
DMF_DECLARE_CODEC(RestorationEvaluation)
DMF_DECLARE_CODEC(Grant)
DMF_DECLARE_CODEC(FenceRecord)
DMF_DECLARE_CODEC(PolicyRule)
DMF_DECLARE_CODEC(ClassProfile)
DMF_DECLARE_CODEC(Policy)

#undef DMF_DECLARE_CODEC

/// True when the reader consumed its whole input without error. A decoder that
/// leaves trailing bytes has been handed a malformed document.
Status decode_finish(const ByteReader& reader);

}  // namespace dmf

#endif  // DMF_CODEC_HPP
