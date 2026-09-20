// Inference Traffic Fabric - evidence, freshness and provenance.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_EVIDENCE_HPP
#define ITF_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "itf/ids.hpp"
#include "itf/time.hpp"

namespace itf {

/// Freshness of a piece of evidence. Unknown and Stale are distinct states and
/// neither is ever treated as Current.
enum class EvidenceState : std::uint16_t {
  Unknown = 0,
  Current = 1,
  Stale = 2,
  RevalidationRequired = 3,
  Count = 4,
};

[[nodiscard]] std::string_view to_string(EvidenceState value) noexcept;

/// How the evidence was obtained. Measured means observed on this host by a
/// real execution path; Synthetic means a declared simulator model. The fabric
/// never promotes Synthetic evidence to Measured.
enum class EvidenceProvenance : std::uint16_t {
  Unknown = 0,
  Synthetic = 1,
  Measured = 2,
  Count = 3,
};

[[nodiscard]] std::string_view to_string(EvidenceProvenance value) noexcept;

struct TopologyEvidence {
  TopologyGeneration generation;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  BootId observed_boot;
  std::int64_t observed_at_nanos = 0;
  std::uint64_t max_age_nanos = 0;
  std::uint32_t node_count = 0;
  std::uint32_t accelerator_count = 0;
  std::uint32_t link_count = 0;
  std::uint64_t fabric_bytes_per_sec = 0;
  std::uint32_t queue_depth = 0;
  bool congestion_signalled = false;
  bool disaggregated = false;

  /// Content-level validity: what the reporter is responsible for.
  [[nodiscard]] bool is_valid() const noexcept;
  /// Content plus the authority fields the coordinator assigns (generation and
  /// the observing incarnation). Only the coordinator may assert this.
  [[nodiscard]] bool is_authoritative() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

struct SloContract {
  SloContractGeneration generation;
  RequestId request;
  std::uint64_t tail_latency_budget_nanos = 0;
  std::uint64_t registration_deadline_nanos = 0;
  std::uint32_t min_stream_share_per_mille = 0;

  [[nodiscard]] bool is_valid() const noexcept;
};

struct RouteDecision {
  RouteDecisionGeneration generation;
  RequestId request;
  AttemptId attempt;
  std::uint32_t target_node = 0;
  bool lateral = false;
  bool disaggregated_handoff = false;

  [[nodiscard]] bool is_valid() const noexcept;
};

struct ModelState {
  std::uint64_t model_hash = 0;
  ModelGeneration generation;
  /// Set when the record survived a coordinator restart. Residency claims from
  /// a previous incarnation are not current authority until re-established.
  bool requires_revalidation = false;

  [[nodiscard]] bool is_valid() const noexcept;
};

/// State-generation registry entry. A transfer bound to generation N is
/// refused once the registry for the same state identity advances past N.
struct StateGenerationRecord {
  std::uint64_t state_hash = 0;
  StateGeneration generation;
  /// Set when the record survived a coordinator restart; see ModelState.
  bool requires_revalidation = false;

  [[nodiscard]] bool is_valid() const noexcept;
};

}  // namespace itf

#endif  // ITF_EVIDENCE_HPP
