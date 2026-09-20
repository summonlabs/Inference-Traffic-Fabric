// Inference Traffic Fabric - protocol message bodies.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_MESSAGES_HPP
#define ITF_MESSAGES_HPP

#include <cstdint>
#include <string>

#include "itf/accounting.hpp"
#include "itf/authority.hpp"
#include "itf/codec.hpp"
#include "itf/coordinator.hpp"
#include "itf/decision.hpp"
#include "itf/error.hpp"
#include "itf/evidence.hpp"
#include "itf/ids.hpp"
#include "itf/policy.hpp"
#include "itf/serialize.hpp"
#include "itf/traffic.hpp"
#include "itf/version.hpp"
#include "itf/wire.hpp"

namespace itf::wire {

/// Session identity is minted by the coordinator per accepted connection. A
/// reconnecting peer never inherits the previous session identity, which is
/// what makes replay protection per-session rather than per-peer.
struct HelloRequest {
  std::uint16_t protocol_version = kWireProtocolVersion;
  std::uint32_t client_api_version = kApiVersion;
  std::string peer_name;
  std::string auth_token;
  std::string client_version;
  BootId client_boot;
  std::uint32_t max_frame_payload = kAbsoluteMaxPayloadBytes;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<HelloRequest> decode(Reader& reader);
};

struct HelloResponse {
  std::uint16_t protocol_version = kWireProtocolVersion;
  std::uint32_t server_api_version = kApiVersion;
  SessionId session;
  PeerId peer;
  BootId coordinator_boot;
  CoordinatorEpoch epoch;
  PolicyGeneration policy_generation;
  TopologyGeneration topology_generation;
  EvidenceState topology_state = EvidenceState::Unknown;
  std::uint32_t max_frame_payload = kAbsoluteMaxPayloadBytes;
  std::uint64_t authority_sequence = 0;
  std::string server_version;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<HelloResponse> decode(Reader& reader);
};

struct ErrorResponse {
  StatusCode code = StatusCode::Ok;
  MessageType offending_type = MessageType::Invalid;
  std::string detail;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<ErrorResponse> decode(Reader& reader);
};

struct SimpleAck {
  StatusCode code = StatusCode::Ok;
  std::string detail;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<SimpleAck> decode(Reader& reader);
};

struct RegisterRequestMessage {
  RequestId request;
  std::uint64_t deadline_after_nanos = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RegisterRequestMessage> decode(Reader& reader);
};

struct RegisterRequestResponse {
  RequestId request;
  RequestGeneration generation;
  AuthorityStamp authority;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RegisterRequestResponse> decode(Reader& reader);
};

struct RegisterAttemptMessage {
  RequestId request;
  AttemptId attempt;
  std::uint64_t model_hash = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RegisterAttemptMessage> decode(Reader& reader);
};

struct RegisterAttemptResponse {
  RequestId request;
  AttemptId attempt;
  AttemptGeneration generation;
  ModelGeneration model_generation;
  RouteDecisionGeneration route_generation;
  SloContractGeneration slo_generation;
  AuthorityStamp authority;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RegisterAttemptResponse> decode(Reader& reader);
};

struct StageUpdateMessage {
  RequestId request;
  AttemptId attempt;
  ServingStage stage = ServingStage::Unknown;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<StageUpdateMessage> decode(Reader& reader);
};

struct CancelRequestMessage {
  RequestId request;
  ReasonCode reason = ReasonCode::Cancelled;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<CancelRequestMessage> decode(Reader& reader);
};

struct FailAttemptMessage {
  RequestId request;
  AttemptId attempt;
  ReasonCode reason = ReasonCode::AttemptFailed;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<FailAttemptMessage> decode(Reader& reader);
};

struct ReplaceAttemptMessage {
  RequestId request;
  AttemptId superseded;
  AttemptId replacement;
  std::uint64_t model_hash = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<ReplaceAttemptMessage> decode(Reader& reader);
};

struct EvaluateTrafficMessage {
  TrafficRequest request;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<EvaluateTrafficMessage> decode(Reader& reader);
};

struct DecisionResponseMessage {
  Decision decision;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<DecisionResponseMessage> decode(Reader& reader);
};

/// Turns a previously issued decision into an accounted flow. The decision is
/// re-validated before any flow is opened, which is what makes cancellation
/// final even for decisions minted before the cancellation.
struct OpenFlowMessage {
  Decision decision;
  std::uint64_t declared_bytes = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<OpenFlowMessage> decode(Reader& reader);
};

struct OpenFlowResponse {
  FlowId flow;
  AuthorityStamp authority;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<OpenFlowResponse> decode(Reader& reader);
};

struct RegisterTransferMessage {
  TransferRegistration registration;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RegisterTransferMessage> decode(Reader& reader);
};

struct RegisterTransferResponse {
  StateTransferId transfer;
  ModelGeneration model_generation;
  StateGeneration state_generation;
  TransferState state = TransferState::Unknown;
  AuthorityStamp authority;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RegisterTransferResponse> decode(Reader& reader);
};

struct AuthorizeTransferMessage {
  TrafficRequest request;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<AuthorizeTransferMessage> decode(Reader& reader);
};

struct TransferDecisionResponse {
  TransferAuthorization authorization;
  TrafficSubject subject;
  GenerationBinding binding;
  AuthorityStamp authority;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<TransferDecisionResponse> decode(Reader& reader);
};

struct TransferCompleteMessage {
  StateTransferId transfer;
  std::uint64_t verified_bytes = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<TransferCompleteMessage> decode(Reader& reader);
};

struct CompletionPublishMessage {
  CompletionPublication publication;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<CompletionPublishMessage> decode(Reader& reader);
};

struct CompletionResponse {
  PublishOutcome outcome;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<CompletionResponse> decode(Reader& reader);
};

struct AccountingQueryMessage {
  std::uint32_t reserved = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<AccountingQueryMessage> decode(Reader& reader);
};

struct AccountingResponseMessage {
  AccountingSnapshot snapshot;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<AccountingResponseMessage> decode(Reader& reader);
};

struct StatusQueryMessage {
  std::uint32_t reserved = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<StatusQueryMessage> decode(Reader& reader);
};

struct StatusResponseMessage {
  CoordinatorStatus status;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<StatusResponseMessage> decode(Reader& reader);
};

struct PolicyQueryMessage {
  std::uint32_t reserved = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<PolicyQueryMessage> decode(Reader& reader);
};

struct PolicyResponseMessage {
  PolicyConfig policy;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<PolicyResponseMessage> decode(Reader& reader);
};

struct TopologyPublishMessage {
  TopologyEvidence evidence;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<TopologyPublishMessage> decode(Reader& reader);
};

struct TopologyResponseMessage {
  TopologyGeneration generation;
  EvidenceState state = EvidenceState::Unknown;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<TopologyResponseMessage> decode(Reader& reader);
};

struct PublishSloContractMessage {
  RequestId request;
  std::uint64_t tail_latency_budget_nanos = 0;
  std::uint64_t registration_deadline_nanos = 0;
  std::uint32_t min_stream_share_per_mille = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<PublishSloContractMessage> decode(Reader& reader);
};

struct SloContractResponse {
  SloContract contract;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<SloContractResponse> decode(Reader& reader);
};

struct IssueRouteDecisionMessage {
  RequestId request;
  AttemptId attempt;
  std::uint32_t target_node = 0;
  bool lateral = false;
  bool disaggregated_handoff = false;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<IssueRouteDecisionMessage> decode(Reader& reader);
};

struct RouteDecisionResponse {
  RouteDecision decision;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<RouteDecisionResponse> decode(Reader& reader);
};

struct AdvanceGenerationMessage {
  std::uint64_t target_hash = 0;
  std::uint64_t expected_generation = 0;
  bool has_expected = false;
  bool revalidate_only = false;
  /// Selects the state-generation registry instead of the model registry.
  bool state_target = false;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<AdvanceGenerationMessage> decode(Reader& reader);
};

struct ModelGenerationResponse {
  std::uint64_t model_hash = 0;
  ModelGeneration generation;
  StateGeneration state_generation;
  bool requires_revalidation = false;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<ModelGenerationResponse> decode(Reader& reader);
};

struct HeartbeatMessage {
  std::uint64_t client_time_nanos = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<HeartbeatMessage> decode(Reader& reader);
};

/// Data plane frame. The payload view aliases the decoded frame buffer and is
/// valid only while that buffer is alive.
struct DataChunkMessage {
  StateTransferId transfer;
  std::uint32_t chunk_index = 0;
  std::uint32_t total_chunks = 0;
  std::uint64_t total_bytes = 0;
  ByteSpan data;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<DataChunkMessage> decode(Reader& reader);
};

struct DataAckMessage {
  StateTransferId transfer;
  std::uint32_t chunk_index = 0;
  std::uint64_t received_bytes = 0;
  StatusCode code = StatusCode::Ok;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<DataAckMessage> decode(Reader& reader);
};

struct DataCompleteMessage {
  StateTransferId transfer;
  std::uint64_t total_bytes = 0;
  std::uint32_t total_chunks = 0;
  std::uint32_t content_crc32c = 0;

  void encode(Writer& writer) const;
  [[nodiscard]] static Result<DataCompleteMessage> decode(Reader& reader);
};

}  // namespace itf::wire

#endif  // ITF_MESSAGES_HPP
