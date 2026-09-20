// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/serialize.hpp"

#include <utility>

namespace itf {
namespace {

template <class Tag>
void write_id(Writer& writer, const Id128<Tag>& value) {
  writer.u64(value.high());
  writer.u64(value.low());
}

template <class Tag>
Id128<Tag> read_id(Reader& reader) {
  const std::uint64_t high = reader.u64();
  const std::uint64_t low = reader.u64();
  if (!reader.ok()) return Id128<Tag>();
  return Id128<Tag>(high, low);
}

template <class Tag>
void write_optional_id(Writer& writer, const Id128<Tag>& value) {
  writer.boolean(!value.is_nil());
  if (!value.is_nil()) write_id(writer, value);
}

template <class Tag>
Id128<Tag> read_optional_id(Reader& reader) {
  const bool present = reader.boolean();
  if (!reader.ok()) return Id128<Tag>();
  if (!present) return Id128<Tag>();
  return read_id<Tag>(reader);
}

template <class Tag>
void write_generation(Writer& writer, const Generation<Tag>& value) {
  writer.u64(value.value());
}

template <class Tag>
Generation<Tag> read_generation(Reader& reader) {
  return Generation<Tag>::from(reader.u64());
}

template <class Tag>
void write_optional_generation(Writer& writer, const std::optional<Generation<Tag>>& value) {
  if (value.has_value() && value->is_unset()) {
    // Emitting an absent generation as present would produce a document that
    // no conforming decoder may accept.
    writer.boolean(true);
    writer.u64(0);
    return;
  }
  writer.boolean(value.has_value());
  if (value.has_value()) writer.u64(value->value());
}

template <class Tag>
std::optional<Generation<Tag>> read_optional_generation(Reader& reader) {
  const bool present = reader.boolean();
  if (!reader.ok()) return std::nullopt;
  if (!present) return std::nullopt;
  const std::uint64_t value = reader.u64();
  if (!reader.ok() || value == 0) {
    reader.fail(StatusCode::MalformedInput);
    return std::nullopt;
  }
  return Generation<Tag>::from(value);
}

void write_instant(Writer& writer, const Instant& value) {
  writer.boolean(value.is_set());
  if (value.is_set()) writer.i64(value.nanos());
}

Instant read_instant(Reader& reader) {
  const bool present = reader.boolean();
  if (!reader.ok()) return Instant();
  if (!present) return Instant();
  return Instant::at(reader.i64());
}

}  // namespace

void encode(Writer& writer, const TrafficSubject& value) {
  writer.enumeration(value.kind);
  write_id(writer, value.request);
  write_id(writer, value.attempt);
  write_generation(writer, value.request_generation);
  write_id(writer, value.transfer);
}

Result<TrafficSubject> decode_traffic_subject(Reader& reader) {
  TrafficSubject subject;
  subject.kind = reader.enumeration(SubjectKind::Count);
  subject.request = read_id<RequestIdTag>(reader);
  subject.attempt = read_id<AttemptIdTag>(reader);
  subject.request_generation = read_generation<RequestGenerationTag>(reader);
  subject.transfer = read_id<StateTransferIdTag>(reader);
  if (!reader.ok()) {
    return Result<TrafficSubject>::failure(reader.fail_code(), "traffic subject");
  }
  if (subject.kind == SubjectKind::Serving &&
      (subject.request.is_nil() || subject.attempt.is_nil() ||
       subject.request_generation.is_unset())) {
    return Result<TrafficSubject>::failure(StatusCode::MalformedInput,
                                           "serving subject is missing request identity");
  }
  if (subject.kind == SubjectKind::Transfer && subject.transfer.is_nil()) {
    return Result<TrafficSubject>::failure(StatusCode::MalformedInput,
                                           "transfer subject is missing transfer identity");
  }
  return Result<TrafficSubject>::success(subject);
}

void encode(Writer& writer, const GenerationBinding& value) {
  write_optional_generation(writer, value.request_generation);
  write_optional_generation(writer, value.model_generation);
  write_optional_generation(writer, value.state_generation);
  write_optional_generation(writer, value.route_decision);
  write_optional_generation(writer, value.slo_contract);
  write_optional_generation(writer, value.topology);
  write_optional_generation(writer, value.policy);
  write_optional_generation(writer, value.coordinator_epoch);
}

Result<GenerationBinding> decode_generation_binding(Reader& reader) {
  GenerationBinding binding;
  binding.request_generation = read_optional_generation<RequestGenerationTag>(reader);
  binding.model_generation = read_optional_generation<ModelGenerationTag>(reader);
  binding.state_generation = read_optional_generation<StateGenerationTag>(reader);
  binding.route_decision = read_optional_generation<RouteDecisionGenerationTag>(reader);
  binding.slo_contract = read_optional_generation<SloContractGenerationTag>(reader);
  binding.topology = read_optional_generation<TopologyGenerationTag>(reader);
  binding.policy = read_optional_generation<PolicyGenerationTag>(reader);
  binding.coordinator_epoch = read_optional_generation<CoordinatorEpochTag>(reader);
  if (!reader.ok()) {
    return Result<GenerationBinding>::failure(reader.fail_code(), "generation binding");
  }
  return Result<GenerationBinding>::success(binding);
}

void encode(Writer& writer, const TrafficRequest& value) {
  writer.boolean(value.subject.has_value());
  if (value.subject.has_value()) encode(writer, *value.subject);
  writer.enumeration(value.declared_class);
  writer.enumeration(value.declared_stage);
  writer.enumeration(value.direction);
  writer.enumeration(value.priority_hint);
  writer.u64(value.payload_bytes);
  writer.u64(value.observed_bytes);
  writer.u32(value.token_count);
  writer.u32(value.chunk_index);
  writer.u32(value.total_chunks);
  writer.u64(value.tenant_hash);
  writer.u64(value.model_hash);
  writer.u64(value.adapter_hash);
  write_instant(writer, value.deadline);
  writer.u64(value.deadline_after_nanos);
  encode(writer, value.binding);
}

Result<TrafficRequest> decode_traffic_request(Reader& reader) {
  TrafficRequest request;
  const bool has_subject = reader.boolean();
  if (!reader.ok()) return Result<TrafficRequest>::failure(reader.fail_code(), "traffic request");
  if (has_subject) {
    auto subject = decode_traffic_subject(reader);
    if (!subject.ok()) return Result<TrafficRequest>::failure(subject.status());
    request.subject = std::move(subject).value();
  }
  request.declared_class = reader.enumeration(TrafficClass::Count);
  request.declared_stage = reader.enumeration(ServingStage::Count);
  request.direction = reader.enumeration(FlowDirection::Count);
  request.priority_hint = reader.enumeration(PriorityHint::Count);
  request.payload_bytes = reader.u64();
  request.observed_bytes = reader.u64();
  request.token_count = reader.u32();
  request.chunk_index = reader.u32();
  request.total_chunks = reader.u32();
  request.tenant_hash = reader.u64();
  request.model_hash = reader.u64();
  request.adapter_hash = reader.u64();
  request.deadline = read_instant(reader);
  request.deadline_after_nanos = reader.u64();
  auto binding = decode_generation_binding(reader);
  if (!binding.ok()) return Result<TrafficRequest>::failure(binding.status());
  request.binding = std::move(binding).value();
  if (!reader.ok()) return Result<TrafficRequest>::failure(reader.fail_code(), "traffic request");
  if (!request.structurally_valid()) {
    return Result<TrafficRequest>::failure(StatusCode::MalformedInput,
                                           "traffic request is internally inconsistent");
  }
  return Result<TrafficRequest>::success(request);
}

void encode(Writer& writer, const TrafficTreatment& value) {
  writer.enumeration(value.traffic_class);
  writer.enumeration(value.priority);
  writer.enumeration(value.pacing);
  writer.enumeration(value.integrity);
  writer.enumeration(value.isolation);
  writer.u32(value.reserved_weight);
  writer.u64(value.max_burst_bytes);
  writer.u64(value.rate_limit_bytes_per_sec);
  write_id(writer, value.flow);
  write_instant(writer, value.hold_until);
  writer.u32(value.defer_count);
}

Result<TrafficTreatment> decode_traffic_treatment(Reader& reader) {
  TrafficTreatment treatment;
  treatment.traffic_class = reader.enumeration(TrafficClass::Count);
  treatment.priority = reader.enumeration(PriorityClass::Count);
  treatment.pacing = reader.enumeration(PacingClass::Count);
  treatment.integrity = reader.enumeration(IntegrityClass::Count);
  treatment.isolation = reader.enumeration(IsolationDomain::Count);
  treatment.reserved_weight = reader.u32();
  treatment.max_burst_bytes = reader.u64();
  treatment.rate_limit_bytes_per_sec = reader.u64();
  treatment.flow = read_id<FlowIdTag>(reader);
  treatment.hold_until = read_instant(reader);
  treatment.defer_count = reader.u32();
  if (!reader.ok()) {
    return Result<TrafficTreatment>::failure(reader.fail_code(), "traffic treatment");
  }
  return Result<TrafficTreatment>::success(treatment);
}

void encode(Writer& writer, const ExplanationStep& value) {
  writer.enumeration(value.rule);
  writer.i64(value.value);
  writer.i64(value.secondary);
}

void encode(Writer& writer, const AuthorityStamp& value) {
  write_generation(writer, value.epoch);
  write_id(writer, value.incarnation);
  write_generation(writer, value.policy);
  write_generation(writer, value.sequence);
}

Result<AuthorityStamp> decode_authority_stamp(Reader& reader) {
  AuthorityStamp stamp;
  stamp.epoch = read_generation<CoordinatorEpochTag>(reader);
  stamp.incarnation = read_id<BootIdTag>(reader);
  stamp.policy = read_generation<PolicyGenerationTag>(reader);
  stamp.sequence = read_generation<AuthoritySequenceTag>(reader);
  if (!reader.ok()) {
    return Result<AuthorityStamp>::failure(reader.fail_code(), "authority stamp");
  }
  if (!stamp.is_valid()) {
    return Result<AuthorityStamp>::failure(StatusCode::MalformedInput,
                                           "authority stamp is incomplete");
  }
  return Result<AuthorityStamp>::success(stamp);
}

void encode(Writer& writer, const Decision& value) {
  writer.enumeration(value.outcome);
  writer.enumeration(value.reason);
  encode(writer, value.treatment);
  writer.boolean(value.subject.is_valid());
  if (value.subject.is_valid()) encode(writer, value.subject);
  encode(writer, value.binding);
  encode(writer, value.authority);
  writer.u64(value.defer_hint_nanos);
  const std::uint32_t count =
      value.explanation_count > kMaxExplanationSteps ? static_cast<std::uint32_t>(kMaxExplanationSteps)
                                                     : value.explanation_count;
  writer.u32(count);
  for (std::uint32_t index = 0; index < count; ++index) encode(writer, value.explanation[index]);
}

Result<Decision> decode_decision(Reader& reader) {
  Decision decision;
  decision.outcome = reader.enumeration(DecisionOutcome::Count);
  decision.reason = reader.enumeration(ReasonCode::Count);
  auto treatment = decode_traffic_treatment(reader);
  if (!treatment.ok()) return Result<Decision>::failure(treatment.status());
  decision.treatment = std::move(treatment).value();
  const bool has_subject = reader.boolean();
  if (!reader.ok()) return Result<Decision>::failure(reader.fail_code(), "decision subject");
  if (has_subject) {
    auto subject = decode_traffic_subject(reader);
    if (!subject.ok()) return Result<Decision>::failure(subject.status());
    decision.subject = std::move(subject).value();
  }
  auto binding = decode_generation_binding(reader);
  if (!binding.ok()) return Result<Decision>::failure(binding.status());
  decision.binding = std::move(binding).value();
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<Decision>::failure(stamp.status());
  decision.authority = std::move(stamp).value();
  decision.defer_hint_nanos = reader.u64();
  const std::uint32_t count = reader.count(kMaxExplanationSteps);
  if (!reader.ok()) return Result<Decision>::failure(reader.fail_code(), "decision");
  if (count > kMaxExplanationSteps) {
    return Result<Decision>::failure(StatusCode::BoundsExceeded, "explanation count out of range");
  }
  decision.explanation_count = count;
  for (std::uint32_t index = 0; index < count; ++index) {
    ExplanationStep step;
    step.rule = reader.enumeration(ExplanationRule::Count);
    step.value = reader.i64();
    step.secondary = reader.i64();
    if (!reader.ok()) return Result<Decision>::failure(reader.fail_code(), "explanation step");
    decision.explanation[index] = step;
  }
  return Result<Decision>::success(decision);
}

void encode(Writer& writer, const ClassAccounting& value) {
  writer.u64(value.allowed);
  writer.u64(value.degraded);
  writer.u64(value.deferred);
  writer.u64(value.rejected);
  writer.u64(value.bytes_allowed);
  writer.u64(value.bytes_deferred);
  writer.u64(value.bytes_rejected);
  writer.u32(value.active_flows);
  writer.u32(value.peak_active_flows);
  writer.u32(value.starvation_admissions);
}

namespace {

ClassAccounting decode_class_accounting(Reader& reader) {
  ClassAccounting value;
  value.allowed = reader.u64();
  value.degraded = reader.u64();
  value.deferred = reader.u64();
  value.rejected = reader.u64();
  value.bytes_allowed = reader.u64();
  value.bytes_deferred = reader.u64();
  value.bytes_rejected = reader.u64();
  value.active_flows = reader.u32();
  value.peak_active_flows = reader.u32();
  value.starvation_admissions = reader.u32();
  return value;
}

}  // namespace

void encode(Writer& writer, const AccountingSnapshot& value) {
  writer.u16(static_cast<std::uint16_t>(kTrafficClassCount));
  for (const ClassAccounting& entry : value.per_class) encode(writer, entry);
  writer.u64(value.requests_registered);
  writer.u64(value.requests_completed);
  writer.u64(value.requests_cancelled);
  writer.u64(value.requests_failed);
  writer.u64(value.requests_interrupted);
  writer.u64(value.requests_evicted);
  writer.u64(value.attempts_registered);
  writer.u64(value.attempts_completed);
  writer.u64(value.attempts_failed);
  writer.u64(value.attempts_superseded);
  writer.u64(value.completions_committed);
  writer.u64(value.completions_suppressed);
  writer.u64(value.completions_refused);
  writer.u64(value.transfers_started);
  writer.u64(value.transfers_completed);
  writer.u64(value.transfers_refused);
  writer.u32(value.active_flows_total);
  writer.u32(value.peak_active_flows_total);
  writer.u32(value.pending_deferrals);
  writer.u64(static_cast<std::uint64_t>(value.live_requests));
  writer.u64(static_cast<std::uint64_t>(value.retained_requests));
}

Result<AccountingSnapshot> decode_accounting_snapshot(Reader& reader) {
  AccountingSnapshot snapshot;
  const std::uint16_t class_count = reader.u16();
  if (!reader.ok()) return Result<AccountingSnapshot>::failure(reader.fail_code(), "accounting");
  if (class_count != static_cast<std::uint16_t>(kTrafficClassCount)) {
    return Result<AccountingSnapshot>::failure(StatusCode::MalformedInput,
                                               "accounting class count mismatch");
  }
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    snapshot.per_class[index] = decode_class_accounting(reader);
  }
  snapshot.requests_registered = reader.u64();
  snapshot.requests_completed = reader.u64();
  snapshot.requests_cancelled = reader.u64();
  snapshot.requests_failed = reader.u64();
  snapshot.requests_interrupted = reader.u64();
  snapshot.requests_evicted = reader.u64();
  snapshot.attempts_registered = reader.u64();
  snapshot.attempts_completed = reader.u64();
  snapshot.attempts_failed = reader.u64();
  snapshot.attempts_superseded = reader.u64();
  snapshot.completions_committed = reader.u64();
  snapshot.completions_suppressed = reader.u64();
  snapshot.completions_refused = reader.u64();
  snapshot.transfers_started = reader.u64();
  snapshot.transfers_completed = reader.u64();
  snapshot.transfers_refused = reader.u64();
  snapshot.active_flows_total = reader.u32();
  snapshot.peak_active_flows_total = reader.u32();
  snapshot.pending_deferrals = reader.u32();
  snapshot.live_requests = static_cast<std::size_t>(reader.u64());
  snapshot.retained_requests = static_cast<std::size_t>(reader.u64());
  if (!reader.ok()) return Result<AccountingSnapshot>::failure(reader.fail_code(), "accounting");
  return Result<AccountingSnapshot>::success(snapshot);
}

void encode(Writer& writer, const AttemptRecord& value) {
  write_id(writer, value.id);
  write_generation(writer, value.generation);
  writer.enumeration(value.stage);
  writer.enumeration(value.lifecycle);
  writer.u64(value.model_hash);
  write_generation(writer, value.model_generation);
  write_generation(writer, value.route_generation);
  writer.boolean(value.success_published);
  writer.boolean(value.failure_published);
  writer.enumeration(value.terminal_reason);
  writer.u64(value.bytes_in);
  writer.u64(value.bytes_out);
  writer.u32(value.decisions);
  writer.u32(value.defers);
  writer.u32(value.rejections);
  writer.i64(value.registered_at_nanos);
  writer.i64(value.updated_at_nanos);
}

namespace {

AttemptRecord decode_attempt_record(Reader& reader, bool& ok) {
  AttemptRecord value;
  value.id = read_id<AttemptIdTag>(reader);
  value.generation = read_generation<AttemptGenerationTag>(reader);
  value.stage = reader.enumeration(ServingStage::Count);
  value.lifecycle = reader.enumeration(AttemptLifecycle::Count);
  value.model_hash = reader.u64();
  value.model_generation = read_generation<ModelGenerationTag>(reader);
  value.route_generation = read_generation<RouteDecisionGenerationTag>(reader);
  value.success_published = reader.boolean();
  value.failure_published = reader.boolean();
  value.terminal_reason = reader.enumeration(ReasonCode::Count);
  value.bytes_in = reader.u64();
  value.bytes_out = reader.u64();
  value.decisions = reader.u32();
  value.defers = reader.u32();
  value.rejections = reader.u32();
  value.registered_at_nanos = reader.i64();
  value.updated_at_nanos = reader.i64();
  ok = reader.ok();
  return value;
}

}  // namespace

void encode(Writer& writer, const RequestRecord& value) {
  write_id(writer, value.id);
  write_generation(writer, value.generation);
  writer.enumeration(value.lifecycle);
  writer.enumeration(value.stage);
  write_id(writer, value.current_attempt);
  write_generation(writer, value.current_attempt_generation);
  const std::uint32_t attempt_count =
      value.attempt_count > kMaxAttemptsPerRequest ? kMaxAttemptsPerRequest : value.attempt_count;
  writer.u32(attempt_count);
  for (std::uint32_t index = 0; index < attempt_count; ++index) encode(writer, value.attempts[index]);
  writer.enumeration(value.terminal_reason);
  writer.i64(value.registered_at_nanos);
  writer.i64(value.terminal_at_nanos);
  writer.boolean(value.requires_revalidation);
  writer.boolean(value.accounting_closed);
  writer.u64(value.bytes_in);
  writer.u64(value.bytes_out);
  writer.u32(value.decisions);
  writer.u32(value.defers);
  writer.u32(value.rejections);
  writer.u32(value.active_flows);
  write_generation(writer, value.authority_floor);
}

Result<RequestRecord> decode_request_record(Reader& reader) {
  RequestRecord record;
  record.id = read_id<RequestIdTag>(reader);
  record.generation = read_generation<RequestGenerationTag>(reader);
  record.lifecycle = reader.enumeration(RequestLifecycle::Count);
  record.stage = reader.enumeration(ServingStage::Count);
  record.current_attempt = read_id<AttemptIdTag>(reader);
  record.current_attempt_generation = read_generation<AttemptGenerationTag>(reader);
  const std::uint32_t attempt_count = reader.count(kMaxAttemptsPerRequest);
  if (!reader.ok()) return Result<RequestRecord>::failure(reader.fail_code(), "request record");
  record.attempt_count = attempt_count;
  for (std::uint32_t index = 0; index < attempt_count; ++index) {
    bool ok = false;
    record.attempts[index] = decode_attempt_record(reader, ok);
    if (!ok) return Result<RequestRecord>::failure(reader.fail_code(), "attempt record");
  }
  record.terminal_reason = reader.enumeration(ReasonCode::Count);
  record.registered_at_nanos = reader.i64();
  record.terminal_at_nanos = reader.i64();
  record.requires_revalidation = reader.boolean();
  record.accounting_closed = reader.boolean();
  record.bytes_in = reader.u64();
  record.bytes_out = reader.u64();
  record.decisions = reader.u32();
  record.defers = reader.u32();
  record.rejections = reader.u32();
  const std::uint32_t active_flows = reader.u32();
  record.active_flows = active_flows;
  record.authority_floor = read_generation<AuthoritySequenceTag>(reader);
  if (!reader.ok()) return Result<RequestRecord>::failure(reader.fail_code(), "request record");
  if (record.id.is_nil()) {
    return Result<RequestRecord>::failure(StatusCode::MalformedInput, "request record has nil id");
  }
  if (attempt_count > 0 && record.current_attempt.is_nil()) {
    return Result<RequestRecord>::failure(StatusCode::MalformedInput,
                                          "request record is missing its current attempt");
  }
  if (record.lifecycle != RequestLifecycle::Unknown && attempt_count == 0 &&
      record.lifecycle != RequestLifecycle::Registered) {
    return Result<RequestRecord>::failure(StatusCode::MalformedInput,
                                          "request record has no attempts");
  }
  return Result<RequestRecord>::success(record);
}

void encode(Writer& writer, const ModelState& value) {
  writer.u64(value.model_hash);
  write_generation(writer, value.generation);
  writer.boolean(value.requires_revalidation);
}

Result<ModelState> decode_model_state(Reader& reader) {
  ModelState value;
  value.model_hash = reader.u64();
  value.generation = read_generation<ModelGenerationTag>(reader);
  value.requires_revalidation = reader.boolean();
  if (!reader.ok()) return Result<ModelState>::failure(reader.fail_code(), "model state");
  if (!value.is_valid()) {
    return Result<ModelState>::failure(StatusCode::MalformedInput, "model state is invalid");
  }
  return Result<ModelState>::success(value);
}

void encode(Writer& writer, const StateGenerationRecord& value) {
  writer.u64(value.state_hash);
  write_generation(writer, value.generation);
  writer.boolean(value.requires_revalidation);
}

Result<StateGenerationRecord> decode_state_generation_record(Reader& reader) {
  StateGenerationRecord value;
  value.state_hash = reader.u64();
  value.generation = read_generation<StateGenerationTag>(reader);
  value.requires_revalidation = reader.boolean();
  if (!reader.ok()) return Result<StateGenerationRecord>::failure(reader.fail_code(), "state record");
  if (!value.is_valid()) {
    return Result<StateGenerationRecord>::failure(StatusCode::MalformedInput,
                                                  "state generation record is invalid");
  }
  return Result<StateGenerationRecord>::success(value);
}

void encode(Writer& writer, const TopologyEvidence& value) {
  write_generation(writer, value.generation);
  writer.enumeration(value.provenance);
  write_id(writer, value.observed_boot);
  writer.i64(value.observed_at_nanos);
  writer.u64(value.max_age_nanos);
  writer.u32(value.node_count);
  writer.u32(value.accelerator_count);
  writer.u32(value.link_count);
  writer.u64(value.fabric_bytes_per_sec);
  writer.u32(value.queue_depth);
  writer.boolean(value.congestion_signalled);
  writer.boolean(value.disaggregated);
}

Result<TopologyEvidence> decode_topology_evidence(Reader& reader, bool validate) {
  TopologyEvidence value;
  value.generation = read_generation<TopologyGenerationTag>(reader);
  value.provenance = reader.enumeration(EvidenceProvenance::Count);
  value.observed_boot = read_id<BootIdTag>(reader);
  value.observed_at_nanos = reader.i64();
  value.max_age_nanos = reader.u64();
  value.node_count = reader.u32();
  value.accelerator_count = reader.u32();
  value.link_count = reader.u32();
  value.fabric_bytes_per_sec = reader.u64();
  value.queue_depth = reader.u32();
  value.congestion_signalled = reader.boolean();
  value.disaggregated = reader.boolean();
  if (!reader.ok()) return Result<TopologyEvidence>::failure(reader.fail_code(), "topology evidence");
  // Only the content is validated here: the generation and the observing
  // incarnation are assigned by the coordinator and are not the reporter's to
  // claim. An absent topology is a placeholder and is never validated.
  if (validate && !value.is_valid()) {
    return Result<TopologyEvidence>::failure(StatusCode::MalformedInput,
                                             "topology evidence is invalid");
  }
  return Result<TopologyEvidence>::success(value);
}

void encode(Writer& writer, const SloContract& value) {
  write_generation(writer, value.generation);
  write_id(writer, value.request);
  writer.u64(value.tail_latency_budget_nanos);
  writer.u64(value.registration_deadline_nanos);
  writer.u32(value.min_stream_share_per_mille);
}

Result<SloContract> decode_slo_contract(Reader& reader) {
  SloContract value;
  value.generation = read_generation<SloContractGenerationTag>(reader);
  value.request = read_id<RequestIdTag>(reader);
  value.tail_latency_budget_nanos = reader.u64();
  value.registration_deadline_nanos = reader.u64();
  value.min_stream_share_per_mille = reader.u32();
  if (!reader.ok()) return Result<SloContract>::failure(reader.fail_code(), "slo contract");
  if (!value.is_valid()) {
    return Result<SloContract>::failure(StatusCode::MalformedInput, "slo contract is invalid");
  }
  return Result<SloContract>::success(value);
}

void encode(Writer& writer, const RouteDecision& value) {
  write_generation(writer, value.generation);
  write_id(writer, value.request);
  write_id(writer, value.attempt);
  writer.u32(value.target_node);
  writer.boolean(value.lateral);
  writer.boolean(value.disaggregated_handoff);
}

Result<RouteDecision> decode_route_decision(Reader& reader) {
  RouteDecision value;
  value.generation = read_generation<RouteDecisionGenerationTag>(reader);
  value.request = read_id<RequestIdTag>(reader);
  value.attempt = read_id<AttemptIdTag>(reader);
  value.target_node = reader.u32();
  value.lateral = reader.boolean();
  value.disaggregated_handoff = reader.boolean();
  if (!reader.ok()) return Result<RouteDecision>::failure(reader.fail_code(), "route decision");
  if (!value.is_valid()) {
    return Result<RouteDecision>::failure(StatusCode::MalformedInput, "route decision is invalid");
  }
  return Result<RouteDecision>::success(value);
}

void encode(Writer& writer, const TransferRegistration& value) {
  write_id(writer, value.id);
  writer.u64(value.state_hash);
  writer.u64(value.model_hash);
  writer.u64(value.payload_bytes);
  writer.enumeration(value.declared_class);
  writer.enumeration(value.direction);
  writer.enumeration(value.required_integrity);
  write_optional_id(writer, value.request);
  write_optional_id(writer, value.attempt);
  write_instant(writer, value.deadline);
}

Result<TransferRegistration> decode_transfer_registration(Reader& reader) {
  TransferRegistration value;
  value.id = read_id<StateTransferIdTag>(reader);
  value.state_hash = reader.u64();
  value.model_hash = reader.u64();
  value.payload_bytes = reader.u64();
  value.declared_class = reader.enumeration(TrafficClass::Count);
  value.direction = reader.enumeration(FlowDirection::Count);
  value.required_integrity = reader.enumeration(IntegrityClass::Count);
  value.request = read_optional_id<RequestIdTag>(reader);
  value.attempt = read_optional_id<AttemptIdTag>(reader);
  value.deadline = read_instant(reader);
  if (!reader.ok()) {
    return Result<TransferRegistration>::failure(reader.fail_code(), "transfer registration");
  }
  if (value.id.is_nil()) {
    return Result<TransferRegistration>::failure(StatusCode::MalformedInput,
                                                 "transfer registration has nil identity");
  }
  return Result<TransferRegistration>::success(value);
}

void encode(Writer& writer, const TransferAuthorization& value) {
  write_id(writer, value.transfer);
  writer.enumeration(value.traffic_class);
  writer.enumeration(value.integrity);
  writer.enumeration(value.pacing);
  writer.enumeration(value.priority);
  encode(writer, value.authority);
  writer.enumeration(value.reason);
  writer.boolean(value.authorized);
  write_id(writer, value.flow);
}

Result<TransferAuthorization> decode_transfer_authorization(Reader& reader) {
  TransferAuthorization value;
  value.transfer = read_id<StateTransferIdTag>(reader);
  value.traffic_class = reader.enumeration(TrafficClass::Count);
  value.integrity = reader.enumeration(IntegrityClass::Count);
  value.pacing = reader.enumeration(PacingClass::Count);
  value.priority = reader.enumeration(PriorityClass::Count);
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<TransferAuthorization>::failure(stamp.status());
  value.authority = std::move(stamp).value();
  value.reason = reader.enumeration(ReasonCode::Count);
  value.authorized = reader.boolean();
  value.flow = read_id<FlowIdTag>(reader);
  if (!reader.ok()) {
    return Result<TransferAuthorization>::failure(reader.fail_code(), "transfer authorization");
  }
  return Result<TransferAuthorization>::success(value);
}

void encode(Writer& writer, const CompletionPublication& value) {
  write_id(writer, value.request);
  write_id(writer, value.attempt);
  write_generation(writer, value.attempt_generation);
  writer.boolean(value.success);
  writer.enumeration(value.reason);
  writer.u64(value.bytes_in);
  writer.u64(value.bytes_out);
}

Result<CompletionPublication> decode_completion_publication(Reader& reader) {
  CompletionPublication value;
  value.request = read_id<RequestIdTag>(reader);
  value.attempt = read_id<AttemptIdTag>(reader);
  value.attempt_generation = read_generation<AttemptGenerationTag>(reader);
  value.success = reader.boolean();
  value.reason = reader.enumeration(ReasonCode::Count);
  value.bytes_in = reader.u64();
  value.bytes_out = reader.u64();
  if (!reader.ok()) {
    return Result<CompletionPublication>::failure(reader.fail_code(), "completion publication");
  }
  if (value.request.is_nil() || value.attempt.is_nil()) {
    return Result<CompletionPublication>::failure(StatusCode::MalformedInput,
                                                  "completion publication has nil identity");
  }
  return Result<CompletionPublication>::success(value);
}

void encode(Writer& writer, const PublishOutcome& value) {
  writer.enumeration(value.disposition);
  writer.u16(static_cast<std::uint16_t>(value.refusal));
  writer.enumeration(value.reason);
  writer.u64(value.accounted_bytes_in);
  writer.u64(value.accounted_bytes_out);
  writer.u32(value.released_flows);
}

Result<PublishOutcome> decode_publish_outcome(Reader& reader) {
  PublishOutcome value;
  value.disposition = reader.enumeration(PublishOutcome::Disposition::Count);
  const std::uint16_t refusal = reader.u16();
  if (refusal > static_cast<std::uint16_t>(StatusCode::Refused)) {
    reader.fail(StatusCode::MalformedInput);
  }
  value.refusal = static_cast<StatusCode>(refusal);
  value.reason = reader.enumeration(ReasonCode::Count);
  value.accounted_bytes_in = reader.u64();
  value.accounted_bytes_out = reader.u64();
  value.released_flows = reader.u32();
  if (!reader.ok()) return Result<PublishOutcome>::failure(reader.fail_code(), "publish outcome");
  return Result<PublishOutcome>::success(value);
}

void encode(Writer& writer, const CoordinatorStatus& value) {
  write_generation(writer, value.epoch);
  write_id(writer, value.incarnation);
  write_generation(writer, value.policy_generation);
  write_generation(writer, value.topology_generation);
  writer.enumeration(value.topology_state);
  writer.enumeration(value.topology_provenance);
  writer.u64(static_cast<std::uint64_t>(value.live_requests));
  writer.u64(static_cast<std::uint64_t>(value.transfers));
  writer.u32(value.active_flows);
  writer.u64(value.authority_sequence);
}

Result<CoordinatorStatus> decode_coordinator_status(Reader& reader) {
  CoordinatorStatus value;
  value.epoch = read_generation<CoordinatorEpochTag>(reader);
  value.incarnation = read_id<BootIdTag>(reader);
  value.policy_generation = read_generation<PolicyGenerationTag>(reader);
  value.topology_generation = read_generation<TopologyGenerationTag>(reader);
  value.topology_state = reader.enumeration(EvidenceState::Count);
  value.topology_provenance = reader.enumeration(EvidenceProvenance::Count);
  value.live_requests = static_cast<std::size_t>(reader.u64());
  value.transfers = static_cast<std::size_t>(reader.u64());
  value.active_flows = reader.u32();
  value.authority_sequence = reader.u64();
  if (!reader.ok()) return Result<CoordinatorStatus>::failure(reader.fail_code(), "status");
  return Result<CoordinatorStatus>::success(value);
}

void encode(Writer& writer, const Instant& value) { write_instant(writer, value); }

Result<Instant> decode_instant(Reader& reader) { return Result<Instant>::success(read_instant(reader)); }

}  // namespace itf
