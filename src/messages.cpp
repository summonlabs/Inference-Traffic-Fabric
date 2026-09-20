// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/messages.hpp"

#include <utility>

namespace itf::wire {
namespace {

template <class Tag>
void write_id(Writer& writer, const Id128<Tag>& value) {
  writer.u64(value.high());
  writer.u64(value.low());
}

template <class Tag>
void write_generation(Writer& writer, const Generation<Tag>& value) {
  writer.u64(value.value());
}

/// Reads a generation field that may legitimately be unset (value zero), such
/// as a route or SLO generation that has not been issued yet.
template <class Tag>
Generation<Tag> read_optional_generation(Reader& reader) {
  return Generation<Tag>::from(reader.u64());
}

/// Reads an identity that may legitimately be absent, such as the coordinator
/// incarnation advertised by an endpoint that has no coordinator authority.
template <class Tag>
Id128<Tag> read_optional_id(Reader& reader) {
  const std::uint64_t high = reader.u64();
  const std::uint64_t low = reader.u64();
  if (!reader.ok()) return Id128<Tag>();
  return Id128<Tag>(high, low);
}

}  // namespace

void HelloRequest::encode(Writer& writer) const {
  writer.u16(protocol_version);
  writer.u32(client_api_version);
  writer.string_field(peer_name);
  writer.string_field(auth_token);
  writer.string_field(client_version);
  write_id(writer, client_boot);
  writer.u32(max_frame_payload);
}

Result<HelloRequest> HelloRequest::decode(Reader& reader) {
  HelloRequest message;
  message.protocol_version = reader.u16();
  message.client_api_version = reader.u32();
  message.peer_name = reader.string_field_owned();
  message.auth_token = reader.string_field_owned();
  message.client_version = reader.string_field_owned();
  message.client_boot = reader.id<BootIdTag>();
  message.max_frame_payload = reader.u32();
  if (!reader.ok()) return Result<HelloRequest>::failure(reader.fail_code(), "hello request");
  if (message.protocol_version != kWireProtocolVersion) {
    return Result<HelloRequest>::failure(StatusCode::UnsupportedVersion,
                                         "peer protocol version is not supported");
  }
  if (message.client_api_version != kApiVersion) {
    return Result<HelloRequest>::failure(StatusCode::UnsupportedVersion,
                                         "peer API version is not supported");
  }
  if (!is_safe_label(message.peer_name)) {
    return Result<HelloRequest>::failure(StatusCode::MalformedInput,
                                         "peer name is not a well-formed label");
  }
  if (message.client_version.size() > 128 || !is_valid_utf8(message.client_version)) {
    return Result<HelloRequest>::failure(StatusCode::MalformedInput,
                                         "client version string is not acceptable");
  }
  if (message.auth_token.size() > 256) {
    return Result<HelloRequest>::failure(StatusCode::BoundsExceeded, "auth token is too long");
  }
  if (message.max_frame_payload > kAbsoluteMaxPayloadBytes) {
    return Result<HelloRequest>::failure(StatusCode::BoundsExceeded,
                                         "negotiated frame payload exceeds the protocol ceiling");
  }
  return Result<HelloRequest>::success(std::move(message));
}

void HelloResponse::encode(Writer& writer) const {
  writer.u16(protocol_version);
  writer.u32(server_api_version);
  write_id(writer, session);
  write_id(writer, peer);
  write_id(writer, coordinator_boot);
  write_generation(writer, epoch);
  write_generation(writer, policy_generation);
  write_generation(writer, topology_generation);
  writer.enumeration(topology_state);
  writer.u32(max_frame_payload);
  writer.u64(authority_sequence);
  writer.string_field(server_version);
}

Result<HelloResponse> HelloResponse::decode(Reader& reader) {
  HelloResponse message;
  message.protocol_version = reader.u16();
  message.server_api_version = reader.u32();
  message.session = reader.id<SessionIdTag>();
  message.peer = reader.id<PeerIdTag>();
  message.coordinator_boot = read_optional_id<BootIdTag>(reader);
  message.epoch = read_optional_generation<CoordinatorEpochTag>(reader);
  message.policy_generation = read_optional_generation<PolicyGenerationTag>(reader);
  message.topology_generation = read_optional_generation<TopologyGenerationTag>(reader);
  message.topology_state = reader.enumeration(EvidenceState::Count);
  message.max_frame_payload = reader.u32();
  message.authority_sequence = reader.u64();
  message.server_version = reader.string_field_owned();
  if (!reader.ok()) return Result<HelloResponse>::failure(reader.fail_code(), "hello response");
  if (message.protocol_version != kWireProtocolVersion) {
    return Result<HelloResponse>::failure(StatusCode::UnsupportedVersion,
                                          "server protocol version is not supported");
  }
  if (message.server_api_version != kApiVersion) {
    return Result<HelloResponse>::failure(StatusCode::UnsupportedVersion,
                                          "server API version is not supported");
  }
  if (message.max_frame_payload == 0 || message.max_frame_payload > kAbsoluteMaxPayloadBytes) {
    return Result<HelloResponse>::failure(StatusCode::BoundsExceeded,
                                          "server frame payload limit is not usable");
  }
  return Result<HelloResponse>::success(std::move(message));
}

void ErrorResponse::encode(Writer& writer) const {
  writer.u16(static_cast<std::uint16_t>(code));
  writer.u16(static_cast<std::uint16_t>(offending_type));
  writer.string_field(detail);
}

Result<ErrorResponse> ErrorResponse::decode(Reader& reader) {
  ErrorResponse message;
  const std::uint16_t code = reader.u16();
  const std::uint16_t offending = reader.u16();
  message.detail = reader.string_field_owned();
  if (!reader.ok()) return Result<ErrorResponse>::failure(reader.fail_code(), "error response");
  if (code > static_cast<std::uint16_t>(StatusCode::Refused)) {
    return Result<ErrorResponse>::failure(StatusCode::MalformedInput, "unknown status code");
  }
  if (offending >= static_cast<std::uint16_t>(MessageType::Count)) {
    return Result<ErrorResponse>::failure(StatusCode::MalformedInput, "unknown message type");
  }
  message.code = static_cast<StatusCode>(code);
  message.offending_type = static_cast<MessageType>(offending);
  return Result<ErrorResponse>::success(std::move(message));
}

void SimpleAck::encode(Writer& writer) const {
  writer.u16(static_cast<std::uint16_t>(code));
  writer.string_field(detail);
}

Result<SimpleAck> SimpleAck::decode(Reader& reader) {
  SimpleAck message;
  const std::uint16_t code = reader.u16();
  message.detail = reader.string_field_owned();
  if (!reader.ok()) return Result<SimpleAck>::failure(reader.fail_code(), "ack");
  if (code > static_cast<std::uint16_t>(StatusCode::Refused)) {
    return Result<SimpleAck>::failure(StatusCode::MalformedInput, "unknown status code");
  }
  message.code = static_cast<StatusCode>(code);
  return Result<SimpleAck>::success(std::move(message));
}

void RegisterRequestMessage::encode(Writer& writer) const {
  write_id(writer, request);
  writer.u64(deadline_after_nanos);
}

Result<RegisterRequestMessage> RegisterRequestMessage::decode(Reader& reader) {
  RegisterRequestMessage message;
  message.request = reader.id<RequestIdTag>();
  message.deadline_after_nanos = reader.u64();
  if (!reader.ok()) return Result<RegisterRequestMessage>::failure(reader.fail_code(), "register request");
  return Result<RegisterRequestMessage>::success(std::move(message));
}

void RegisterRequestResponse::encode(Writer& writer) const {
  write_id(writer, request);
  write_generation(writer, generation);
  itf::encode(writer, authority);
}

Result<RegisterRequestResponse> RegisterRequestResponse::decode(Reader& reader) {
  RegisterRequestResponse message;
  message.request = reader.id<RequestIdTag>();
  message.generation = reader.generation<RequestGenerationTag>();
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<RegisterRequestResponse>::failure(stamp.status());
  message.authority = std::move(stamp).value();
  if (!reader.ok()) return Result<RegisterRequestResponse>::failure(reader.fail_code(), "register response");
  return Result<RegisterRequestResponse>::success(std::move(message));
}

void RegisterAttemptMessage::encode(Writer& writer) const {
  write_id(writer, request);
  write_id(writer, attempt);
  writer.u64(model_hash);
}

Result<RegisterAttemptMessage> RegisterAttemptMessage::decode(Reader& reader) {
  RegisterAttemptMessage message;
  message.request = reader.id<RequestIdTag>();
  message.attempt = reader.id<AttemptIdTag>();
  message.model_hash = reader.u64();
  if (!reader.ok()) return Result<RegisterAttemptMessage>::failure(reader.fail_code(), "register attempt");
  return Result<RegisterAttemptMessage>::success(std::move(message));
}

void RegisterAttemptResponse::encode(Writer& writer) const {
  write_id(writer, request);
  write_id(writer, attempt);
  write_generation(writer, generation);
  write_generation(writer, model_generation);
  write_generation(writer, route_generation);
  write_generation(writer, slo_generation);
  itf::encode(writer, authority);
}

Result<RegisterAttemptResponse> RegisterAttemptResponse::decode(Reader& reader) {
  RegisterAttemptResponse message;
  message.request = reader.id<RequestIdTag>();
  message.attempt = reader.id<AttemptIdTag>();
  message.generation = reader.generation<AttemptGenerationTag>();
  message.model_generation = read_optional_generation<ModelGenerationTag>(reader);
  message.route_generation = read_optional_generation<RouteDecisionGenerationTag>(reader);
  message.slo_generation = read_optional_generation<SloContractGenerationTag>(reader);
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<RegisterAttemptResponse>::failure(stamp.status());
  message.authority = std::move(stamp).value();
  if (!reader.ok()) return Result<RegisterAttemptResponse>::failure(reader.fail_code(), "register attempt response");
  return Result<RegisterAttemptResponse>::success(std::move(message));
}

void StageUpdateMessage::encode(Writer& writer) const {
  write_id(writer, request);
  write_id(writer, attempt);
  writer.enumeration(stage);
}

Result<StageUpdateMessage> StageUpdateMessage::decode(Reader& reader) {
  StageUpdateMessage message;
  message.request = reader.id<RequestIdTag>();
  message.attempt = reader.id<AttemptIdTag>();
  message.stage = reader.enumeration(ServingStage::Count);
  if (!reader.ok()) return Result<StageUpdateMessage>::failure(reader.fail_code(), "stage update");
  return Result<StageUpdateMessage>::success(std::move(message));
}

void CancelRequestMessage::encode(Writer& writer) const {
  write_id(writer, request);
  writer.enumeration(reason);
}

Result<CancelRequestMessage> CancelRequestMessage::decode(Reader& reader) {
  CancelRequestMessage message;
  message.request = reader.id<RequestIdTag>();
  message.reason = reader.enumeration(ReasonCode::Count);
  if (!reader.ok()) return Result<CancelRequestMessage>::failure(reader.fail_code(), "cancel request");
  return Result<CancelRequestMessage>::success(std::move(message));
}

void FailAttemptMessage::encode(Writer& writer) const {
  write_id(writer, request);
  write_id(writer, attempt);
  writer.enumeration(reason);
}

Result<FailAttemptMessage> FailAttemptMessage::decode(Reader& reader) {
  FailAttemptMessage message;
  message.request = reader.id<RequestIdTag>();
  message.attempt = reader.id<AttemptIdTag>();
  message.reason = reader.enumeration(ReasonCode::Count);
  if (!reader.ok()) return Result<FailAttemptMessage>::failure(reader.fail_code(), "fail attempt");
  return Result<FailAttemptMessage>::success(std::move(message));
}

void ReplaceAttemptMessage::encode(Writer& writer) const {
  write_id(writer, request);
  write_id(writer, superseded);
  write_id(writer, replacement);
  writer.u64(model_hash);
}

Result<ReplaceAttemptMessage> ReplaceAttemptMessage::decode(Reader& reader) {
  ReplaceAttemptMessage message;
  message.request = reader.id<RequestIdTag>();
  message.superseded = reader.id<AttemptIdTag>();
  message.replacement = reader.id<AttemptIdTag>();
  message.model_hash = reader.u64();
  if (!reader.ok()) {
    return Result<ReplaceAttemptMessage>::failure(reader.fail_code(), "replace attempt");
  }
  return Result<ReplaceAttemptMessage>::success(std::move(message));
}

void EvaluateTrafficMessage::encode(Writer& writer) const { itf::encode(writer, request); }

Result<EvaluateTrafficMessage> EvaluateTrafficMessage::decode(Reader& reader) {
  EvaluateTrafficMessage message;
  auto request = decode_traffic_request(reader);
  if (!request.ok()) return Result<EvaluateTrafficMessage>::failure(request.status());
  message.request = std::move(request).value();
  if (!reader.ok()) return Result<EvaluateTrafficMessage>::failure(reader.fail_code(), "evaluate traffic");
  return Result<EvaluateTrafficMessage>::success(std::move(message));
}

void DecisionResponseMessage::encode(Writer& writer) const { itf::encode(writer, decision); }

Result<DecisionResponseMessage> DecisionResponseMessage::decode(Reader& reader) {
  DecisionResponseMessage message;
  auto decision = decode_decision(reader);
  if (!decision.ok()) return Result<DecisionResponseMessage>::failure(decision.status());
  message.decision = std::move(decision).value();
  if (!reader.ok()) return Result<DecisionResponseMessage>::failure(reader.fail_code(), "decision response");
  return Result<DecisionResponseMessage>::success(std::move(message));
}

void OpenFlowMessage::encode(Writer& writer) const {
  itf::encode(writer, decision);
  writer.u64(declared_bytes);
}

Result<OpenFlowMessage> OpenFlowMessage::decode(Reader& reader) {
  OpenFlowMessage message;
  auto decision = decode_decision(reader);
  if (!decision.ok()) return Result<OpenFlowMessage>::failure(decision.status());
  message.decision = std::move(decision).value();
  message.declared_bytes = reader.u64();
  if (!reader.ok()) return Result<OpenFlowMessage>::failure(reader.fail_code(), "open flow");
  return Result<OpenFlowMessage>::success(std::move(message));
}

void OpenFlowResponse::encode(Writer& writer) const {
  writer.id(flow);
  itf::encode(writer, authority);
}

Result<OpenFlowResponse> OpenFlowResponse::decode(Reader& reader) {
  OpenFlowResponse message;
  message.flow = reader.id<FlowIdTag>();
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<OpenFlowResponse>::failure(stamp.status());
  message.authority = std::move(stamp).value();
  if (!reader.ok()) return Result<OpenFlowResponse>::failure(reader.fail_code(), "open flow response");
  return Result<OpenFlowResponse>::success(std::move(message));
}

void RegisterTransferMessage::encode(Writer& writer) const { itf::encode(writer, registration); }

Result<RegisterTransferMessage> RegisterTransferMessage::decode(Reader& reader) {
  RegisterTransferMessage message;
  auto registration = decode_transfer_registration(reader);
  if (!registration.ok()) return Result<RegisterTransferMessage>::failure(registration.status());
  message.registration = std::move(registration).value();
  if (!reader.ok()) return Result<RegisterTransferMessage>::failure(reader.fail_code(), "register transfer");
  return Result<RegisterTransferMessage>::success(std::move(message));
}

void RegisterTransferResponse::encode(Writer& writer) const {
  write_id(writer, transfer);
  write_generation(writer, model_generation);
  write_generation(writer, state_generation);
  writer.enumeration(state);
  itf::encode(writer, authority);
}

Result<RegisterTransferResponse> RegisterTransferResponse::decode(Reader& reader) {
  RegisterTransferResponse message;
  message.transfer = reader.id<StateTransferIdTag>();
  message.model_generation = read_optional_generation<ModelGenerationTag>(reader);
  message.state_generation = read_optional_generation<StateGenerationTag>(reader);
  message.state = reader.enumeration(TransferState::Count);
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<RegisterTransferResponse>::failure(stamp.status());
  message.authority = std::move(stamp).value();
  if (!reader.ok()) return Result<RegisterTransferResponse>::failure(reader.fail_code(), "register transfer response");
  return Result<RegisterTransferResponse>::success(std::move(message));
}

void AuthorizeTransferMessage::encode(Writer& writer) const { itf::encode(writer, request); }

Result<AuthorizeTransferMessage> AuthorizeTransferMessage::decode(Reader& reader) {
  AuthorizeTransferMessage message;
  auto request = decode_traffic_request(reader);
  if (!request.ok()) return Result<AuthorizeTransferMessage>::failure(request.status());
  message.request = std::move(request).value();
  if (!reader.ok()) return Result<AuthorizeTransferMessage>::failure(reader.fail_code(), "authorize transfer");
  return Result<AuthorizeTransferMessage>::success(std::move(message));
}

void TransferDecisionResponse::encode(Writer& writer) const {
  itf::encode(writer, authorization);
  itf::encode(writer, subject);
  itf::encode(writer, binding);
  itf::encode(writer, authority);
}

Result<TransferDecisionResponse> TransferDecisionResponse::decode(Reader& reader) {
  TransferDecisionResponse message;
  auto authorization = decode_transfer_authorization(reader);
  if (!authorization.ok()) return Result<TransferDecisionResponse>::failure(authorization.status());
  message.authorization = std::move(authorization).value();
  auto subject = decode_traffic_subject(reader);
  if (!subject.ok()) return Result<TransferDecisionResponse>::failure(subject.status());
  message.subject = std::move(subject).value();
  auto binding = decode_generation_binding(reader);
  if (!binding.ok()) return Result<TransferDecisionResponse>::failure(binding.status());
  message.binding = std::move(binding).value();
  auto stamp = decode_authority_stamp(reader);
  if (!stamp.ok()) return Result<TransferDecisionResponse>::failure(stamp.status());
  message.authority = std::move(stamp).value();
  if (!reader.ok()) return Result<TransferDecisionResponse>::failure(reader.fail_code(), "transfer decision");
  return Result<TransferDecisionResponse>::success(std::move(message));
}

void TransferCompleteMessage::encode(Writer& writer) const {
  write_id(writer, transfer);
  writer.u64(verified_bytes);
}

Result<TransferCompleteMessage> TransferCompleteMessage::decode(Reader& reader) {
  TransferCompleteMessage message;
  message.transfer = reader.id<StateTransferIdTag>();
  message.verified_bytes = reader.u64();
  if (!reader.ok()) return Result<TransferCompleteMessage>::failure(reader.fail_code(), "transfer complete");
  return Result<TransferCompleteMessage>::success(std::move(message));
}

void CompletionPublishMessage::encode(Writer& writer) const { itf::encode(writer, publication); }

Result<CompletionPublishMessage> CompletionPublishMessage::decode(Reader& reader) {
  CompletionPublishMessage message;
  auto publication = decode_completion_publication(reader);
  if (!publication.ok()) return Result<CompletionPublishMessage>::failure(publication.status());
  message.publication = std::move(publication).value();
  if (!reader.ok()) return Result<CompletionPublishMessage>::failure(reader.fail_code(), "completion publish");
  return Result<CompletionPublishMessage>::success(std::move(message));
}

void CompletionResponse::encode(Writer& writer) const { itf::encode(writer, outcome); }

Result<CompletionResponse> CompletionResponse::decode(Reader& reader) {
  CompletionResponse message;
  auto outcome = decode_publish_outcome(reader);
  if (!outcome.ok()) return Result<CompletionResponse>::failure(outcome.status());
  message.outcome = std::move(outcome).value();
  if (!reader.ok()) return Result<CompletionResponse>::failure(reader.fail_code(), "completion response");
  return Result<CompletionResponse>::success(std::move(message));
}

void AccountingQueryMessage::encode(Writer& writer) const { writer.u32(reserved); }

Result<AccountingQueryMessage> AccountingQueryMessage::decode(Reader& reader) {
  AccountingQueryMessage message;
  message.reserved = reader.u32();
  if (!reader.ok()) return Result<AccountingQueryMessage>::failure(reader.fail_code(), "accounting query");
  return Result<AccountingQueryMessage>::success(std::move(message));
}

void AccountingResponseMessage::encode(Writer& writer) const { itf::encode(writer, snapshot); }

Result<AccountingResponseMessage> AccountingResponseMessage::decode(Reader& reader) {
  AccountingResponseMessage message;
  auto snapshot = decode_accounting_snapshot(reader);
  if (!snapshot.ok()) return Result<AccountingResponseMessage>::failure(snapshot.status());
  message.snapshot = std::move(snapshot).value();
  if (!reader.ok()) return Result<AccountingResponseMessage>::failure(reader.fail_code(), "accounting response");
  return Result<AccountingResponseMessage>::success(std::move(message));
}

void StatusQueryMessage::encode(Writer& writer) const { writer.u32(reserved); }

Result<StatusQueryMessage> StatusQueryMessage::decode(Reader& reader) {
  StatusQueryMessage message;
  message.reserved = reader.u32();
  if (!reader.ok()) return Result<StatusQueryMessage>::failure(reader.fail_code(), "status query");
  return Result<StatusQueryMessage>::success(std::move(message));
}

void StatusResponseMessage::encode(Writer& writer) const { itf::encode(writer, status); }

Result<StatusResponseMessage> StatusResponseMessage::decode(Reader& reader) {
  StatusResponseMessage message;
  auto status = decode_coordinator_status(reader);
  if (!status.ok()) return Result<StatusResponseMessage>::failure(status.status());
  message.status = std::move(status).value();
  if (!reader.ok()) return Result<StatusResponseMessage>::failure(reader.fail_code(), "status response");
  return Result<StatusResponseMessage>::success(std::move(message));
}

void PolicyQueryMessage::encode(Writer& writer) const { writer.u32(reserved); }

Result<PolicyQueryMessage> PolicyQueryMessage::decode(Reader& reader) {
  PolicyQueryMessage message;
  message.reserved = reader.u32();
  if (!reader.ok()) return Result<PolicyQueryMessage>::failure(reader.fail_code(), "policy query");
  return Result<PolicyQueryMessage>::success(std::move(message));
}

void PolicyResponseMessage::encode(Writer& writer) const { policy.encode(writer); }

Result<PolicyResponseMessage> PolicyResponseMessage::decode(Reader& reader) {
  PolicyResponseMessage message;
  auto policy = PolicyConfig::decode(reader);
  if (!policy.ok()) return Result<PolicyResponseMessage>::failure(policy.status());
  message.policy = std::move(policy).value();
  if (!reader.ok()) return Result<PolicyResponseMessage>::failure(reader.fail_code(), "policy response");
  return Result<PolicyResponseMessage>::success(std::move(message));
}

void TopologyPublishMessage::encode(Writer& writer) const { itf::encode(writer, evidence); }

Result<TopologyPublishMessage> TopologyPublishMessage::decode(Reader& reader) {
  TopologyPublishMessage message;
  auto evidence = decode_topology_evidence(reader);
  if (!evidence.ok()) return Result<TopologyPublishMessage>::failure(evidence.status());
  message.evidence = std::move(evidence).value();
  if (!reader.ok()) return Result<TopologyPublishMessage>::failure(reader.fail_code(), "topology publish");
  return Result<TopologyPublishMessage>::success(std::move(message));
}

void TopologyResponseMessage::encode(Writer& writer) const {
  write_generation(writer, generation);
  writer.enumeration(state);
  writer.enumeration(provenance);
}

Result<TopologyResponseMessage> TopologyResponseMessage::decode(Reader& reader) {
  TopologyResponseMessage message;
  message.generation = read_optional_generation<TopologyGenerationTag>(reader);
  message.state = reader.enumeration(EvidenceState::Count);
  message.provenance = reader.enumeration(EvidenceProvenance::Count);
  if (!reader.ok()) return Result<TopologyResponseMessage>::failure(reader.fail_code(), "topology response");
  return Result<TopologyResponseMessage>::success(std::move(message));
}

void PublishSloContractMessage::encode(Writer& writer) const {
  write_id(writer, request);
  writer.u64(tail_latency_budget_nanos);
  writer.u64(registration_deadline_nanos);
  writer.u32(min_stream_share_per_mille);
}

Result<PublishSloContractMessage> PublishSloContractMessage::decode(Reader& reader) {
  PublishSloContractMessage message;
  message.request = reader.id<RequestIdTag>();
  message.tail_latency_budget_nanos = reader.u64();
  message.registration_deadline_nanos = reader.u64();
  message.min_stream_share_per_mille = reader.u32();
  if (!reader.ok()) return Result<PublishSloContractMessage>::failure(reader.fail_code(), "slo contract publish");
  return Result<PublishSloContractMessage>::success(std::move(message));
}

void SloContractResponse::encode(Writer& writer) const { itf::encode(writer, contract); }

Result<SloContractResponse> SloContractResponse::decode(Reader& reader) {
  SloContractResponse message;
  auto contract = decode_slo_contract(reader);
  if (!contract.ok()) return Result<SloContractResponse>::failure(contract.status());
  message.contract = std::move(contract).value();
  if (!reader.ok()) return Result<SloContractResponse>::failure(reader.fail_code(), "slo contract response");
  return Result<SloContractResponse>::success(std::move(message));
}

void IssueRouteDecisionMessage::encode(Writer& writer) const {
  write_id(writer, request);
  write_id(writer, attempt);
  writer.u32(target_node);
  writer.boolean(lateral);
  writer.boolean(disaggregated_handoff);
}

Result<IssueRouteDecisionMessage> IssueRouteDecisionMessage::decode(Reader& reader) {
  IssueRouteDecisionMessage message;
  message.request = reader.id<RequestIdTag>();
  message.attempt = reader.id<AttemptIdTag>();
  message.target_node = reader.u32();
  message.lateral = reader.boolean();
  message.disaggregated_handoff = reader.boolean();
  if (!reader.ok()) return Result<IssueRouteDecisionMessage>::failure(reader.fail_code(), "issue route decision");
  return Result<IssueRouteDecisionMessage>::success(std::move(message));
}

void RouteDecisionResponse::encode(Writer& writer) const { itf::encode(writer, decision); }

Result<RouteDecisionResponse> RouteDecisionResponse::decode(Reader& reader) {
  RouteDecisionResponse message;
  auto decision = decode_route_decision(reader);
  if (!decision.ok()) return Result<RouteDecisionResponse>::failure(decision.status());
  message.decision = std::move(decision).value();
  if (!reader.ok()) return Result<RouteDecisionResponse>::failure(reader.fail_code(), "route decision response");
  return Result<RouteDecisionResponse>::success(std::move(message));
}

void AdvanceGenerationMessage::encode(Writer& writer) const {
  writer.u64(target_hash);
  writer.u64(expected_generation);
  writer.boolean(has_expected);
  writer.boolean(revalidate_only);
  writer.boolean(state_target);
}

Result<AdvanceGenerationMessage> AdvanceGenerationMessage::decode(Reader& reader) {
  AdvanceGenerationMessage message;
  message.target_hash = reader.u64();
  message.expected_generation = reader.u64();
  message.has_expected = reader.boolean();
  message.revalidate_only = reader.boolean();
  message.state_target = reader.boolean();
  if (!reader.ok()) return Result<AdvanceGenerationMessage>::failure(reader.fail_code(), "advance generation");
  if (message.target_hash == 0) {
    return Result<AdvanceGenerationMessage>::failure(StatusCode::MalformedInput, "nil generation target");
  }
  return Result<AdvanceGenerationMessage>::success(std::move(message));
}

void ModelGenerationResponse::encode(Writer& writer) const {
  writer.u64(model_hash);
  write_generation(writer, generation);
  write_generation(writer, state_generation);
  writer.boolean(requires_revalidation);
}

Result<ModelGenerationResponse> ModelGenerationResponse::decode(Reader& reader) {
  ModelGenerationResponse message;
  message.model_hash = reader.u64();
  message.generation = read_optional_generation<ModelGenerationTag>(reader);
  message.state_generation = read_optional_generation<StateGenerationTag>(reader);
  message.requires_revalidation = reader.boolean();
  if (!reader.ok()) return Result<ModelGenerationResponse>::failure(reader.fail_code(), "model generation response");
  return Result<ModelGenerationResponse>::success(std::move(message));
}

void HeartbeatMessage::encode(Writer& writer) const { writer.u64(client_time_nanos); }

Result<HeartbeatMessage> HeartbeatMessage::decode(Reader& reader) {
  HeartbeatMessage message;
  message.client_time_nanos = reader.u64();
  if (!reader.ok()) return Result<HeartbeatMessage>::failure(reader.fail_code(), "heartbeat");
  return Result<HeartbeatMessage>::success(std::move(message));
}

void DataChunkMessage::encode(Writer& writer) const {
  write_id(writer, transfer);
  writer.u32(chunk_index);
  writer.u32(total_chunks);
  writer.u64(total_bytes);
  const std::uint32_t bounded =
      data.size() > kMaxChunkBytes ? kMaxChunkBytes : static_cast<std::uint32_t>(data.size());
  writer.bytes_field(ByteSpan(data.data(), bounded));
}

Result<DataChunkMessage> DataChunkMessage::decode(Reader& reader) {
  DataChunkMessage message;
  message.transfer = reader.id<StateTransferIdTag>();
  message.chunk_index = reader.u32();
  message.total_chunks = reader.u32();
  message.total_bytes = reader.u64();
  message.data = reader.bytes_field();
  if (!reader.ok()) return Result<DataChunkMessage>::failure(reader.fail_code(), "data chunk");
  if (message.data.size() > kMaxChunkBytes) {
    return Result<DataChunkMessage>::failure(StatusCode::BoundsExceeded, "data chunk is too large");
  }
  if (message.total_chunks == 0 || message.chunk_index >= message.total_chunks) {
    return Result<DataChunkMessage>::failure(StatusCode::MalformedInput,
                                             "data chunk index is out of range");
  }
  if (message.data.empty()) {
    return Result<DataChunkMessage>::failure(StatusCode::MalformedInput, "data chunk is empty");
  }
  return Result<DataChunkMessage>::success(message);
}

void DataAckMessage::encode(Writer& writer) const {
  write_id(writer, transfer);
  writer.u32(chunk_index);
  writer.u64(received_bytes);
  writer.u16(static_cast<std::uint16_t>(code));
}

Result<DataAckMessage> DataAckMessage::decode(Reader& reader) {
  DataAckMessage message;
  message.transfer = reader.id<StateTransferIdTag>();
  message.chunk_index = reader.u32();
  message.received_bytes = reader.u64();
  const std::uint16_t code = reader.u16();
  if (!reader.ok()) return Result<DataAckMessage>::failure(reader.fail_code(), "data ack");
  if (code > static_cast<std::uint16_t>(StatusCode::Refused)) {
    return Result<DataAckMessage>::failure(StatusCode::MalformedInput, "unknown status code");
  }
  message.code = static_cast<StatusCode>(code);
  return Result<DataAckMessage>::success(std::move(message));
}

void DataCompleteMessage::encode(Writer& writer) const {
  write_id(writer, transfer);
  writer.u64(total_bytes);
  writer.u32(total_chunks);
  writer.u32(content_crc32c);
}

Result<DataCompleteMessage> DataCompleteMessage::decode(Reader& reader) {
  DataCompleteMessage message;
  message.transfer = reader.id<StateTransferIdTag>();
  message.total_bytes = reader.u64();
  message.total_chunks = reader.u32();
  message.content_crc32c = reader.u32();
  if (!reader.ok()) return Result<DataCompleteMessage>::failure(reader.fail_code(), "data complete");
  if (message.total_chunks == 0) {
    return Result<DataCompleteMessage>::failure(StatusCode::MalformedInput,
                                                "data transfer declares no chunks");
  }
  return Result<DataCompleteMessage>::success(std::move(message));
}

}  // namespace itf::wire
