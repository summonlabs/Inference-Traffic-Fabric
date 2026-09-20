// Inference Traffic Fabric - decisions, reason codes and explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_DECISION_HPP
#define ITF_DECISION_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "itf/authority.hpp"
#include "itf/traffic.hpp"

namespace itf {

/// Outcome of evaluating one traffic request. There is no implicit "maybe":
/// deferral, rejection and degradation are all explicit.
enum class DecisionOutcome : std::uint16_t {
  Unknown = 0,
  Allowed = 1,
  Degraded = 2,
  Deferred = 3,
  Rejected = 4,
  Count = 5,
};

[[nodiscard]] std::string_view to_string(DecisionOutcome value) noexcept;

/// Deterministic machine-checkable reason. Every decision carries exactly one.
enum class ReasonCode : std::uint16_t {
  Unknown = 0,
  Admitted = 1,
  AdmittedStarvationGuard = 2,
  AdmittedConservative = 3,
  DegradedStageClaimOverridden = 4,
  DegradedClassRederived = 5,
  DegradedStaleTopology = 6,
  DegradedMissingSloContract = 7,
  DegradedIntegrityRaised = 8,
  DegradedPriorityLowered = 9,
  DeferredCapacity = 10,
  DeferredCongestion = 11,
  DeferredBulkYield = 12,
  DeferredBulkCapacity = 13,
  DeferredBulkConcurrency = 14,
  RejectedUnknownRequest = 15,
  RejectedUnknownAttempt = 16,
  RejectedRequestCancelled = 17,
  RejectedRequestTerminal = 18,
  RejectedStaleAttempt = 19,
  RejectedStaleRequestGeneration = 20,
  RejectedStaleRouteDecision = 21,
  RejectedStaleModelGeneration = 22,
  RejectedStaleStateGeneration = 23,
  RejectedStaleSloContract = 24,
  RejectedStaleTopologyGeneration = 25,
  RejectedStalePolicyGeneration = 26,
  RejectedStaleEpoch = 27,
  RejectedStaleAuthority = 28,
  RejectedUnknownStage = 29,
  RejectedUnknownClass = 30,
  RejectedClassNotLegalForStage = 31,
  RejectedClassDisabled = 32,
  RejectedSubjectKindMismatch = 33,
  RejectedUnknownTransfer = 34,
  RejectedEvidenceUnknown = 35,
  RejectedEvidenceStale = 36,
  RejectedRevalidationRequired = 37,
  RejectedDeadlineExpired = 38,
  RejectedCapacityExhausted = 39,
  RejectedFlowLimit = 40,
  RejectedNotAuthorized = 41,
  RejectedIntegrityUnavailable = 42,
  RejectedDuplicateCompletion = 43,
  RejectedTransferAlreadyTerminal = 44,
  CommittedCompletion = 45,
  DuplicateCompletionSuppressed = 46,
  AttemptFailed = 47,
  AttemptSuperseded = 48,
  Cancelled = 49,
  Count = 50,
};

[[nodiscard]] std::string_view to_string(ReasonCode value) noexcept;
[[nodiscard]] bool is_rejection(ReasonCode value) noexcept;
[[nodiscard]] bool is_deferral(ReasonCode value) noexcept;
[[nodiscard]] bool is_admission(ReasonCode value) noexcept;

/// Stable identity of one rule application, so that an explanation survives
/// the wire and the durable format without carrying free text.
enum class ExplanationRule : std::uint16_t {
  Unknown = 0,
  AuthorityBinding = 1,
  RequestLookup = 2,
  StageResolution = 3,
  ClassClassification = 4,
  PolicyRule = 5,
  TopologyEvidence = 6,
  SloContract = 7,
  RouteDecision = 8,
  ModelGeneration = 9,
  StateGeneration = 10,
  Capacity = 11,
  Congestion = 12,
  BulkIsolation = 13,
  StarvationGuard = 14,
  Deadline = 15,
  DeferState = 16,
  FlowAccounting = 17,
  TerminalState = 18,
  TransferBinding = 19,
  Count = 20,
};

[[nodiscard]] std::string_view to_string(ExplanationRule value) noexcept;
/// Static human-readable description of what the rule decides.
[[nodiscard]] std::string_view describe(ExplanationRule value) noexcept;

inline constexpr std::size_t kMaxExplanationSteps = 24;

/// One rule application with its numeric context. Rules are never rendered
/// from caller-supplied text, so an explanation can never be used to smuggle
/// unbounded data.
struct ExplanationStep {
  ExplanationRule rule = ExplanationRule::Unknown;
  std::int64_t value = 0;
  std::int64_t secondary = 0;
};

/// Bounded, allocation-free step accumulator.
class ExplanationLog {
 public:
  void add(ExplanationRule rule, std::int64_t value = 0, std::int64_t secondary = 0,
           std::uint32_t limit = kMaxExplanationSteps) noexcept {
    if (count_ >= kMaxExplanationSteps || count_ >= limit) return;
    steps_[count_] = ExplanationStep{rule, value, secondary};
    ++count_;
  }

  [[nodiscard]] std::uint32_t count() const noexcept { return count_; }
  [[nodiscard]] const ExplanationStep& operator[](std::size_t index) const noexcept {
    return steps_[index];
  }
  [[nodiscard]] const std::array<ExplanationStep, kMaxExplanationSteps>& steps() const noexcept {
    return steps_;
  }

 private:
  std::array<ExplanationStep, kMaxExplanationSteps> steps_{};
  std::uint32_t count_ = 0;
};

struct Decision {
  DecisionOutcome outcome = DecisionOutcome::Unknown;
  ReasonCode reason = ReasonCode::Unknown;
  TrafficTreatment treatment;
  TrafficSubject subject;
  GenerationBinding binding;
  AuthorityStamp authority;
  std::uint64_t defer_hint_nanos = 0;
  std::uint32_t explanation_count = 0;
  std::array<ExplanationStep, kMaxExplanationSteps> explanation{};

  [[nodiscard]] bool permits_traffic() const noexcept {
    return outcome == DecisionOutcome::Allowed || outcome == DecisionOutcome::Degraded;
  }
  [[nodiscard]] bool is_authority_denial() const noexcept;

  /// Canonical single-line rendering used by the CLI, the tests and the
  /// coordinator's inspection surface.
  [[nodiscard]] std::string to_string() const;

  /// Multi-line explanation including every recorded rule application.
  [[nodiscard]] std::string explain() const;
};

}  // namespace itf

#endif  // ITF_DECISION_HPP
