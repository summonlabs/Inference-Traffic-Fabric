// Inference Traffic Fabric - inference traffic classes, serving stages and
// treatment vocabulary.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_TRAFFIC_HPP
#define ITF_TRAFFIC_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "itf/ids.hpp"
#include "itf/time.hpp"

namespace itf {

/// Semantic class of inference traffic. This is the unit the fabric reasons
/// about; it is never inferred from ports or protocols.
enum class TrafficClass : std::uint16_t {
  Unknown = 0,
  RequestIngress = 1,
  PrefillInput = 2,
  PrefillDecodeHandoff = 3,
  KvTransfer = 4,
  StateTransfer = 5,
  ModelTransfer = 6,
  AdapterTransfer = 7,
  DecodeStream = 8,
  SpeculativeBranch = 9,
  ResponseEgress = 10,
  Control = 11,
  Count = 12,
};

inline constexpr std::size_t kTrafficClassCount = static_cast<std::size_t>(TrafficClass::Count);

/// SCREAMING_SNAKE label matching the external vocabulary of the fabric.
[[nodiscard]] std::string_view label(TrafficClass value) noexcept;
[[nodiscard]] std::string_view to_string(TrafficClass value) noexcept;
[[nodiscard]] std::optional<TrafficClass> traffic_class_from_label(std::string_view text) noexcept;

/// Serving stage of a request/attempt. Unknown is a first-class value: it is
/// not equivalent to "no stage restrictions".
enum class ServingStage : std::uint16_t {
  Unknown = 0,
  Admitted = 1,
  Routing = 2,
  PrefillQueued = 3,
  PrefillRunning = 4,
  HandoffPending = 5,
  DecodeQueued = 6,
  DecodeRunning = 7,
  Streaming = 8,
  Completing = 9,
  Completed = 10,
  Cancelled = 11,
  Failed = 12,
  Count = 13,
};

inline constexpr std::size_t kServingStageCount = static_cast<std::size_t>(ServingStage::Count);

[[nodiscard]] std::string_view label(ServingStage value) noexcept;
[[nodiscard]] std::string_view to_string(ServingStage value) noexcept;
[[nodiscard]] std::optional<ServingStage> serving_stage_from_label(std::string_view text) noexcept;
[[nodiscard]] bool is_terminal(ServingStage value) noexcept;
[[nodiscard]] bool is_prefill_stage(ServingStage value) noexcept;
[[nodiscard]] bool is_decode_stage(ServingStage value) noexcept;
[[nodiscard]] bool is_serving_active(ServingStage value) noexcept;

/// True when the transition from -> to is a legal lifecycle step. Terminal
/// stages have no outgoing transitions.
[[nodiscard]] bool is_legal_stage_transition(ServingStage from, ServingStage to) noexcept;

/// The single traffic class that a stage authorises when the caller offers no
/// usable declaration.
[[nodiscard]] TrafficClass canonical_class_for_stage(ServingStage stage) noexcept;

/// Stage-aware semantic classification guard. A declared class is accepted
/// only when the serving stage legally produces it.
[[nodiscard]] bool is_class_legal_for_stage(TrafficClass traffic, ServingStage stage) noexcept;

enum class FlowDirection : std::uint16_t {
  Unknown = 0,
  Ingress = 1,
  Egress = 2,
  Lateral = 3,
  Count = 4,
};

[[nodiscard]] std::string_view to_string(FlowDirection value) noexcept;

enum class PriorityClass : std::uint16_t {
  Unknown = 0,
  LatencyCritical = 1,
  Interactive = 2,
  Standard = 3,
  Background = 4,
  Bulk = 5,
  Count = 6,
};

[[nodiscard]] std::string_view to_string(PriorityClass value) noexcept;

enum class PacingClass : std::uint16_t {
  Unknown = 0,
  Immediate = 1,
  Paced = 2,
  RateLimited = 3,
  BestEffort = 4,
  Count = 5,
};

[[nodiscard]] std::string_view to_string(PacingClass value) noexcept;

/// Integrity metadata the transfer must carry. Checksum means per-frame
/// integrity; ChecksumAndVerify additionally requires post-transfer
/// verification before the transfer may be reported complete.
enum class IntegrityClass : std::uint16_t {
  Unknown = 0,
  None = 1,
  Checksum = 2,
  ChecksumAndVerify = 3,
  Count = 4,
};

[[nodiscard]] std::string_view to_string(IntegrityClass value) noexcept;

enum class IsolationDomain : std::uint16_t {
  Unknown = 0,
  LatencyCritical = 1,
  Interactive = 2,
  Background = 3,
  Bulk = 4,
  Control = 5,
  Count = 6,
};

[[nodiscard]] std::string_view to_string(IsolationDomain value) noexcept;

enum class PriorityHint : std::uint16_t {
  None = 0,
  LatencyCritical = 1,
  Interactive = 2,
  Batch = 3,
  Background = 4,
  Count = 5,
};

[[nodiscard]] std::string_view to_string(PriorityHint value) noexcept;

/// What the traffic is about. Either a serving request/attempt, or a
/// fabric-level transfer that is not tied to a request (model or adapter
/// movement). The two are governed by different rule sets.
enum class SubjectKind : std::uint16_t {
  Unknown = 0,
  Serving = 1,
  Transfer = 2,
  Count = 3,
};

[[nodiscard]] std::string_view to_string(SubjectKind value) noexcept;

struct TrafficSubject {
  SubjectKind kind = SubjectKind::Unknown;
  RequestId request;
  AttemptId attempt;
  RequestGeneration request_generation;
  StateTransferId transfer;

  [[nodiscard]] bool is_valid() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

/// Generation fence attached to a decision request. Absent fields mean the
/// caller did not bind that dimension, which is materially different from a
/// bound dimension matching.
struct GenerationBinding {
  std::optional<RequestGeneration> request_generation;
  std::optional<ModelGeneration> model_generation;
  std::optional<StateGeneration> state_generation;
  std::optional<RouteDecisionGeneration> route_decision;
  std::optional<SloContractGeneration> slo_contract;
  std::optional<TopologyGeneration> topology;
  std::optional<PolicyGeneration> policy;
  std::optional<CoordinatorEpoch> coordinator_epoch;

  [[nodiscard]] std::string to_string() const;
};

/// Wire-level traffic description. It carries metadata only: sizes, counts,
/// stage/class claims, generations and opaque hashes. Prompt text, token ids
/// and payload bytes are never required to classify traffic and are never
/// carried here.
struct TrafficRequest {
  std::optional<TrafficSubject> subject;
  TrafficClass declared_class = TrafficClass::Unknown;
  ServingStage declared_stage = ServingStage::Unknown;
  FlowDirection direction = FlowDirection::Unknown;
  PriorityHint priority_hint = PriorityHint::None;
  std::uint64_t payload_bytes = 0;
  std::uint64_t observed_bytes = 0;
  std::uint32_t token_count = 0;
  std::uint32_t chunk_index = 0;
  std::uint32_t total_chunks = 0;
  std::uint64_t tenant_hash = 0;
  std::uint64_t model_hash = 0;
  std::uint64_t adapter_hash = 0;
  /// Absolute deadline in the local clock domain. Used by in-process callers.
  Instant deadline;
  /// Relative deadline carried on the wire. The coordinator converts it to an
  /// absolute instant when the request is accepted. Setting both an absolute
  /// deadline and a relative deadline is contradictory and is refused.
  std::uint64_t deadline_after_nanos = 0;
  GenerationBinding binding;

  [[nodiscard]] bool structurally_valid() const noexcept;
};

/// Concrete network treatment granted by a decision.
struct TrafficTreatment {
  TrafficClass traffic_class = TrafficClass::Unknown;
  PriorityClass priority = PriorityClass::Unknown;
  PacingClass pacing = PacingClass::Unknown;
  IntegrityClass integrity = IntegrityClass::Unknown;
  IsolationDomain isolation = IsolationDomain::Unknown;
  std::uint32_t reserved_weight = 0;
  std::uint64_t max_burst_bytes = 0;
  std::uint64_t rate_limit_bytes_per_sec = 0;
  FlowId flow;
  Instant hold_until;
  std::uint32_t defer_count = 0;

  [[nodiscard]] std::string to_string() const;
};

}  // namespace itf

#endif  // ITF_TRAFFIC_HPP
