// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/decision.hpp"

namespace itf {

std::string_view to_string(DecisionOutcome value) noexcept {
  switch (value) {
    case DecisionOutcome::Unknown: return "UNKNOWN";
    case DecisionOutcome::Allowed: return "ALLOWED";
    case DecisionOutcome::Degraded: return "DEGRADED";
    case DecisionOutcome::Deferred: return "DEFERRED";
    case DecisionOutcome::Rejected: return "REJECTED";
    case DecisionOutcome::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(ReasonCode value) noexcept {
  switch (value) {
    case ReasonCode::Unknown: return "UNKNOWN";
    case ReasonCode::Admitted: return "ADMITTED";
    case ReasonCode::AdmittedStarvationGuard: return "ADMITTED_STARVATION_GUARD";
    case ReasonCode::AdmittedConservative: return "ADMITTED_CONSERVATIVE";
    case ReasonCode::DegradedStageClaimOverridden: return "DEGRADED_STAGE_CLAIM_OVERRIDDEN";
    case ReasonCode::DegradedClassRederived: return "DEGRADED_CLASS_REDERIVED";
    case ReasonCode::DegradedStaleTopology: return "DEGRADED_STALE_TOPOLOGY";
    case ReasonCode::DegradedMissingSloContract: return "DEGRADED_MISSING_SLO_CONTRACT";
    case ReasonCode::DegradedIntegrityRaised: return "DEGRADED_INTEGRITY_RAISED";
    case ReasonCode::DegradedPriorityLowered: return "DEGRADED_PRIORITY_LOWERED";
    case ReasonCode::DeferredCapacity: return "DEFERRED_CAPACITY";
    case ReasonCode::DeferredCongestion: return "DEFERRED_CONGESTION";
    case ReasonCode::DeferredBulkYield: return "DEFERRED_BULK_YIELD";
    case ReasonCode::DeferredBulkCapacity: return "DEFERRED_BULK_CAPACITY";
    case ReasonCode::DeferredBulkConcurrency: return "DEFERRED_BULK_CONCURRENCY";
    case ReasonCode::RejectedUnknownRequest: return "REJECTED_UNKNOWN_REQUEST";
    case ReasonCode::RejectedUnknownAttempt: return "REJECTED_UNKNOWN_ATTEMPT";
    case ReasonCode::RejectedRequestCancelled: return "REJECTED_REQUEST_CANCELLED";
    case ReasonCode::RejectedRequestTerminal: return "REJECTED_REQUEST_TERMINAL";
    case ReasonCode::RejectedStaleAttempt: return "REJECTED_STALE_ATTEMPT";
    case ReasonCode::RejectedStaleRequestGeneration: return "REJECTED_STALE_REQUEST_GENERATION";
    case ReasonCode::RejectedStaleRouteDecision: return "REJECTED_STALE_ROUTE_DECISION";
    case ReasonCode::RejectedStaleModelGeneration: return "REJECTED_STALE_MODEL_GENERATION";
    case ReasonCode::RejectedStaleStateGeneration: return "REJECTED_STALE_STATE_GENERATION";
    case ReasonCode::RejectedStaleSloContract: return "REJECTED_STALE_SLO_CONTRACT";
    case ReasonCode::RejectedStaleTopologyGeneration: return "REJECTED_STALE_TOPOLOGY_GENERATION";
    case ReasonCode::RejectedStalePolicyGeneration: return "REJECTED_STALE_POLICY_GENERATION";
    case ReasonCode::RejectedStaleEpoch: return "REJECTED_STALE_EPOCH";
    case ReasonCode::RejectedStaleAuthority: return "REJECTED_STALE_AUTHORITY";
    case ReasonCode::RejectedUnknownStage: return "REJECTED_UNKNOWN_STAGE";
    case ReasonCode::RejectedUnknownClass: return "REJECTED_UNKNOWN_CLASS";
    case ReasonCode::RejectedClassNotLegalForStage: return "REJECTED_CLASS_NOT_LEGAL_FOR_STAGE";
    case ReasonCode::RejectedClassDisabled: return "REJECTED_CLASS_DISABLED";
    case ReasonCode::RejectedSubjectKindMismatch: return "REJECTED_SUBJECT_KIND_MISMATCH";
    case ReasonCode::RejectedUnknownTransfer: return "REJECTED_UNKNOWN_TRANSFER";
    case ReasonCode::RejectedEvidenceUnknown: return "REJECTED_EVIDENCE_UNKNOWN";
    case ReasonCode::RejectedEvidenceStale: return "REJECTED_EVIDENCE_STALE";
    case ReasonCode::RejectedRevalidationRequired: return "REJECTED_REVALIDATION_REQUIRED";
    case ReasonCode::RejectedDeadlineExpired: return "REJECTED_DEADLINE_EXPIRED";
    case ReasonCode::RejectedCapacityExhausted: return "REJECTED_CAPACITY_EXHAUSTED";
    case ReasonCode::RejectedFlowLimit: return "REJECTED_FLOW_LIMIT";
    case ReasonCode::RejectedNotAuthorized: return "REJECTED_NOT_AUTHORIZED";
    case ReasonCode::RejectedIntegrityUnavailable: return "REJECTED_INTEGRITY_UNAVAILABLE";
    case ReasonCode::RejectedDuplicateCompletion: return "REJECTED_DUPLICATE_COMPLETION";
    case ReasonCode::RejectedTransferAlreadyTerminal: return "REJECTED_TRANSFER_ALREADY_TERMINAL";
    case ReasonCode::CommittedCompletion: return "COMMITTED_COMPLETION";
    case ReasonCode::DuplicateCompletionSuppressed: return "DUPLICATE_COMPLETION_SUPPRESSED";
    case ReasonCode::AttemptFailed: return "ATTEMPT_FAILED";
    case ReasonCode::AttemptSuperseded: return "ATTEMPT_SUPERSEDED";
    case ReasonCode::Cancelled: return "CANCELLED";
    case ReasonCode::Count: return "COUNT";
  }
  return "UNKNOWN";
}

bool is_rejection(ReasonCode value) noexcept {
  return static_cast<std::uint16_t>(value) >=
             static_cast<std::uint16_t>(ReasonCode::RejectedUnknownRequest) &&
         static_cast<std::uint16_t>(value) <=
             static_cast<std::uint16_t>(ReasonCode::RejectedTransferAlreadyTerminal);
}

bool is_deferral(ReasonCode value) noexcept {
  return static_cast<std::uint16_t>(value) >=
             static_cast<std::uint16_t>(ReasonCode::DeferredCapacity) &&
         static_cast<std::uint16_t>(value) <=
             static_cast<std::uint16_t>(ReasonCode::DeferredBulkConcurrency);
}

bool is_admission(ReasonCode value) noexcept {
  return value == ReasonCode::Admitted || value == ReasonCode::AdmittedStarvationGuard ||
         value == ReasonCode::AdmittedConservative ||
         value == ReasonCode::DegradedStageClaimOverridden ||
         value == ReasonCode::DegradedClassRederived ||
         value == ReasonCode::DegradedStaleTopology ||
         value == ReasonCode::DegradedMissingSloContract ||
         value == ReasonCode::DegradedIntegrityRaised ||
         value == ReasonCode::DegradedPriorityLowered;
}

bool Decision::is_authority_denial() const noexcept {
  if (outcome != DecisionOutcome::Rejected) return false;
  switch (reason) {
    case ReasonCode::RejectedUnknownRequest:
    case ReasonCode::RejectedUnknownAttempt:
    case ReasonCode::RejectedRequestCancelled:
    case ReasonCode::RejectedRequestTerminal:
    case ReasonCode::RejectedStaleAttempt:
    case ReasonCode::RejectedStaleRequestGeneration:
    case ReasonCode::RejectedStaleRouteDecision:
    case ReasonCode::RejectedStaleModelGeneration:
    case ReasonCode::RejectedStaleStateGeneration:
    case ReasonCode::RejectedStaleSloContract:
    case ReasonCode::RejectedStaleTopologyGeneration:
    case ReasonCode::RejectedStalePolicyGeneration:
    case ReasonCode::RejectedStaleEpoch:
    case ReasonCode::RejectedStaleAuthority:
    case ReasonCode::RejectedNotAuthorized:
    case ReasonCode::RejectedRevalidationRequired:
    case ReasonCode::RejectedUnknownTransfer:
      return true;
    default:
      return false;
  }
}

std::string Decision::to_string() const {
  std::string out;
  out.reserve(256);
  out.append(itf::to_string(outcome));
  out.push_back(' ');
  out.append(itf::to_string(reason));
  out.append(" [");
  out.append(subject.to_string());
  out.append("] ");
  out.append(treatment.to_string());
  out.append(" binding{");
  out.append(binding.to_string());
  out.append("} authority{");
  out.append(authority.to_string());
  out.push_back('}');
  return out;
}

std::string_view to_string(ExplanationRule value) noexcept {
  switch (value) {
    case ExplanationRule::Unknown: return "UNKNOWN";
    case ExplanationRule::AuthorityBinding: return "AUTHORITY_BINDING";
    case ExplanationRule::RequestLookup: return "REQUEST_LOOKUP";
    case ExplanationRule::StageResolution: return "STAGE_RESOLUTION";
    case ExplanationRule::ClassClassification: return "CLASS_CLASSIFICATION";
    case ExplanationRule::PolicyRule: return "POLICY_RULE";
    case ExplanationRule::TopologyEvidence: return "TOPOLOGY_EVIDENCE";
    case ExplanationRule::SloContract: return "SLO_CONTRACT";
    case ExplanationRule::RouteDecision: return "ROUTE_DECISION";
    case ExplanationRule::ModelGeneration: return "MODEL_GENERATION";
    case ExplanationRule::StateGeneration: return "STATE_GENERATION";
    case ExplanationRule::Capacity: return "CAPACITY";
    case ExplanationRule::Congestion: return "CONGESTION";
    case ExplanationRule::BulkIsolation: return "BULK_ISOLATION";
    case ExplanationRule::StarvationGuard: return "STARVATION_GUARD";
    case ExplanationRule::Deadline: return "DEADLINE";
    case ExplanationRule::DeferState: return "DEFER_STATE";
    case ExplanationRule::FlowAccounting: return "FLOW_ACCOUNTING";
    case ExplanationRule::TerminalState: return "TERMINAL_STATE";
    case ExplanationRule::TransferBinding: return "TRANSFER_BINDING";
    case ExplanationRule::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view describe(ExplanationRule value) noexcept {
  switch (value) {
    case ExplanationRule::Unknown: return "rule not identified";
    case ExplanationRule::AuthorityBinding:
      return "decision re-validated against the current epoch, incarnation and policy";
    case ExplanationRule::RequestLookup: return "request/attempt resolved in the ledger";
    case ExplanationRule::StageResolution: return "authoritative serving stage resolved";
    case ExplanationRule::ClassClassification: return "traffic class derived from the stage";
    case ExplanationRule::PolicyRule: return "per-class policy rule applied";
    case ExplanationRule::TopologyEvidence: return "topology evidence freshness consulted";
    case ExplanationRule::SloContract: return "SLO contract generation consulted";
    case ExplanationRule::RouteDecision: return "route decision generation consulted";
    case ExplanationRule::ModelGeneration: return "model generation fence consulted";
    case ExplanationRule::StateGeneration: return "state generation fence consulted";
    case ExplanationRule::Capacity: return "in-flight capacity compared with demand";
    case ExplanationRule::Congestion: return "congestion signal consulted";
    case ExplanationRule::BulkIsolation: return "bulk isolation from latency-critical classes";
    case ExplanationRule::StarvationGuard: return "starvation guard evaluation";
    case ExplanationRule::Deadline: return "deadline feasibility checked";
    case ExplanationRule::DeferState: return "per-flow deferral state consulted";
    case ExplanationRule::FlowAccounting: return "flow accounting updated";
    case ExplanationRule::TerminalState: return "terminal lifecycle state consulted";
    case ExplanationRule::TransferBinding: return "transfer generation binding consulted";
    case ExplanationRule::Count: return "count";
  }
  return "rule not identified";
}

std::string Decision::explain() const {
  std::string out = to_string();
  out.push_back('\n');
  for (std::uint32_t index = 0; index < explanation_count && index < kMaxExplanationSteps;
       ++index) {
    const ExplanationStep& step = explanation[index];
    out.append("  - ");
    out.append(itf::to_string(step.rule));
    out.append(": ");
    out.append(describe(step.rule));
    out.append(" [value=");
    out.append(std::to_string(step.value));
    out.append(" secondary=");
    out.append(std::to_string(step.secondary));
    out.append("]\n");
  }
  return out;
}

}  // namespace itf
