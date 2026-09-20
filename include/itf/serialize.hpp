// Inference Traffic Fabric - canonical encoders and decoders for every wire
// and durable structure.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_SERIALIZE_HPP
#define ITF_SERIALIZE_HPP

#include "itf/accounting.hpp"
#include "itf/codec.hpp"
#include "itf/coordinator.hpp"
#include "itf/decision.hpp"
#include "itf/error.hpp"
#include "itf/evidence.hpp"
#include "itf/ledger.hpp"
#include "itf/policy.hpp"
#include "itf/traffic.hpp"

namespace itf {

void encode(Writer& writer, const TrafficSubject& value);
void encode(Writer& writer, const GenerationBinding& value);
void encode(Writer& writer, const TrafficRequest& value);
void encode(Writer& writer, const TrafficTreatment& value);
void encode(Writer& writer, const ExplanationStep& value);
void encode(Writer& writer, const Decision& value);
void encode(Writer& writer, const AuthorityStamp& value);
void encode(Writer& writer, const ClassAccounting& value);
void encode(Writer& writer, const AccountingSnapshot& value);
void encode(Writer& writer, const AttemptRecord& value);
void encode(Writer& writer, const RequestRecord& value);
void encode(Writer& writer, const ModelState& value);
void encode(Writer& writer, const StateGenerationRecord& value);
void encode(Writer& writer, const TopologyEvidence& value);
void encode(Writer& writer, const SloContract& value);
void encode(Writer& writer, const RouteDecision& value);
void encode(Writer& writer, const TransferRegistration& value);
void encode(Writer& writer, const TransferAuthorization& value);
void encode(Writer& writer, const CompletionPublication& value);
void encode(Writer& writer, const PublishOutcome& value);
void encode(Writer& writer, const CoordinatorStatus& value);
void encode(Writer& writer, const Instant& value);

[[nodiscard]] Result<TrafficSubject> decode_traffic_subject(Reader& reader);
[[nodiscard]] Result<GenerationBinding> decode_generation_binding(Reader& reader);
[[nodiscard]] Result<TrafficRequest> decode_traffic_request(Reader& reader);
[[nodiscard]] Result<TrafficTreatment> decode_traffic_treatment(Reader& reader);
[[nodiscard]] Result<Decision> decode_decision(Reader& reader);
[[nodiscard]] Result<AuthorityStamp> decode_authority_stamp(Reader& reader);
[[nodiscard]] Result<AccountingSnapshot> decode_accounting_snapshot(Reader& reader);
[[nodiscard]] Result<RequestRecord> decode_request_record(Reader& reader);
[[nodiscard]] Result<ModelState> decode_model_state(Reader& reader);
[[nodiscard]] Result<StateGenerationRecord> decode_state_generation_record(Reader& reader);
/// When validate is false the document carries no topology at all and the
/// placeholder fields are not validated.
[[nodiscard]] Result<TopologyEvidence> decode_topology_evidence(Reader& reader,
                                                             bool validate = true);
[[nodiscard]] Result<SloContract> decode_slo_contract(Reader& reader);
[[nodiscard]] Result<RouteDecision> decode_route_decision(Reader& reader);
[[nodiscard]] Result<TransferRegistration> decode_transfer_registration(Reader& reader);
[[nodiscard]] Result<TransferAuthorization> decode_transfer_authorization(Reader& reader);
[[nodiscard]] Result<CompletionPublication> decode_completion_publication(Reader& reader);
[[nodiscard]] Result<PublishOutcome> decode_publish_outcome(Reader& reader);
[[nodiscard]] Result<CoordinatorStatus> decode_coordinator_status(Reader& reader);
[[nodiscard]] Result<Instant> decode_instant(Reader& reader);

}  // namespace itf

#endif  // ITF_SERIALIZE_HPP
