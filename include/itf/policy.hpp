// Inference Traffic Fabric - deterministic per-class treatment policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_POLICY_HPP
#define ITF_POLICY_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "itf/codec.hpp"
#include "itf/error.hpp"
#include "itf/ids.hpp"
#include "itf/traffic.hpp"

namespace itf {

/// Reserved weight space. Reserved weights are expressed in thousandths of the
/// fabric capacity; bulk-class traffic may never consume reserved capacity.
inline constexpr std::uint32_t kReservedWeightScale = 1000;

/// One rule per traffic class. Every field is explicit: nothing is inferred
/// from a default that a reviewer would have to guess.
struct ClassPolicyRule {
  bool enabled = false;
  PriorityClass priority = PriorityClass::Unknown;
  PacingClass pacing = PacingClass::Unknown;
  IntegrityClass integrity = IntegrityClass::Unknown;
  IsolationDomain isolation = IsolationDomain::Unknown;
  std::uint32_t reserved_weight = 0;
  std::uint64_t max_burst_bytes = 0;
  std::uint64_t rate_limit_bytes_per_sec = 0;
  std::uint64_t max_in_flight_bytes = 0;
  bool requires_current_topology = false;
  bool requires_slo_contract = false;
  bool requires_current_route = false;
  bool requires_model_generation = false;
  bool requires_state_generation = false;
  bool requires_deadline = false;

  /// Starvation protection: after this many consecutive defers, or after this
  /// long waiting since the first defer, capacity-based deferral is refused
  /// and the flow is admitted. Authority-based refusal is never bypassed.
  bool starvation_protected = false;
  std::uint32_t max_defer_count = 0;
  std::uint64_t max_defer_horizon_nanos = 0;

  /// Bulk-class flows yield to latency-critical pressure and to bulk capacity
  /// limits instead of competing with streaming traffic.
  bool yields_to_latency = false;
};

enum class PolicyVerdict : std::uint16_t {
  Unknown = 0,
  Valid = 1,
  MissingRule = 2,
  ReservedWeightOverflow = 3,
  BulkReserved = 4,
  UnsupportedValue = 5,
  BoundsExceeded = 6,
  Count = 7,
};

[[nodiscard]] std::string_view to_string(PolicyVerdict value) noexcept;

class PolicyConfig {
 public:
  PolicyConfig() = default;

  [[nodiscard]] PolicyGeneration generation() const noexcept { return generation_; }
  void set_generation(PolicyGeneration value) noexcept { generation_ = value; }

  [[nodiscard]] const ClassPolicyRule& rule(TrafficClass traffic) const noexcept {
    const std::size_t index = static_cast<std::size_t>(traffic);
    return index < kTrafficClassCount ? rules_[index] : rules_[0];
  }

  Status set_rule(TrafficClass traffic, const ClassPolicyRule& rule);

  [[nodiscard]] bool allow_unknown_stage() const noexcept { return allow_unknown_stage_; }
  void set_allow_unknown_stage(bool value) noexcept { allow_unknown_stage_ = value; }

  [[nodiscard]] bool allow_unknown_traffic_class() const noexcept {
    return allow_unknown_traffic_class_;
  }
  void set_allow_unknown_traffic_class(bool value) noexcept {
    allow_unknown_traffic_class_ = value;
  }

  [[nodiscard]] std::uint32_t max_explanation_steps() const noexcept {
    return max_explanation_steps_;
  }
  void set_max_explanation_steps(std::uint32_t value) noexcept {
    max_explanation_steps_ = value > kMaxExplanationHardLimit ? kMaxExplanationHardLimit : value;
  }

  [[nodiscard]] std::uint64_t bulk_in_flight_cap_bytes() const noexcept {
    return bulk_in_flight_cap_bytes_;
  }
  void set_bulk_in_flight_cap_bytes(std::uint64_t value) noexcept {
    bulk_in_flight_cap_bytes_ = value;
  }

  [[nodiscard]] std::uint32_t congestion_queue_depth() const noexcept {
    return congestion_queue_depth_;
  }
  void set_congestion_queue_depth(std::uint32_t value) noexcept { congestion_queue_depth_ = value; }

  [[nodiscard]] bool congestion_yields_bulk() const noexcept { return congestion_yields_bulk_; }
  void set_congestion_yields_bulk(bool value) noexcept { congestion_yields_bulk_ = value; }

  [[nodiscard]] std::uint64_t defer_hint_nanos() const noexcept { return defer_hint_nanos_; }
  void set_defer_hint_nanos(std::uint64_t value) noexcept { defer_hint_nanos_ = value; }

  [[nodiscard]] std::uint64_t stale_evidence_grace_nanos() const noexcept {
    return stale_evidence_grace_nanos_;
  }
  void set_stale_evidence_grace_nanos(std::uint64_t value) noexcept {
    stale_evidence_grace_nanos_ = value;
  }

  /// Sum of reserved weights of classes that are enabled and non-bulk.
  [[nodiscard]] std::uint32_t reserved_weight_total() const noexcept;

  /// Structural validation. Returns the first defect it finds, deterministically.
  [[nodiscard]] PolicyVerdict validate() const noexcept;
  [[nodiscard]] Status validate_status() const;

  /// Canonical default policy for inference serving. Deterministic: the same
  /// binary always produces the same document.
  [[nodiscard]] static PolicyConfig canonical_defaults(PolicyGeneration generation);

  /// Canonical encoding used for persistence and for shipping policy to peers.
  void encode(Writer& writer) const;
  [[nodiscard]] static Result<PolicyConfig> decode(Reader& reader);

  [[nodiscard]] std::string to_string() const;

  static constexpr std::uint32_t kMaxExplanationHardLimit = 32;

 private:
  PolicyGeneration generation_;
  std::array<ClassPolicyRule, kTrafficClassCount> rules_{};
  bool allow_unknown_stage_ = false;
  bool allow_unknown_traffic_class_ = false;
  std::uint32_t max_explanation_steps_ = 8;
  std::uint64_t bulk_in_flight_cap_bytes_ = 64ULL * 1024ULL * 1024ULL;
  std::uint32_t congestion_queue_depth_ = 64;
  bool congestion_yields_bulk_ = true;
  std::uint64_t defer_hint_nanos_ = 2000000ULL;
  std::uint64_t stale_evidence_grace_nanos_ = 0;
};

}  // namespace itf

#endif  // ITF_POLICY_HPP
