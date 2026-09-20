// Inference Traffic Fabric - serving traffic authority coordinator.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_COORDINATOR_HPP
#define ITF_COORDINATOR_HPP

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "itf/accounting.hpp"
#include "itf/authority.hpp"
#include "itf/capability.hpp"
#include "itf/decision.hpp"
#include "itf/error.hpp"
#include "itf/evidence.hpp"
#include "itf/ledger.hpp"
#include "itf/policy.hpp"
#include "itf/time.hpp"
#include "itf/traffic.hpp"

namespace itf {

struct PersistedCoordinatorState;

struct CoordinatorLimits {
  LedgerLimits ledger;
  std::uint32_t max_slo_contracts = 4096;
  std::uint32_t max_route_decisions = 8192;
  std::uint32_t max_model_records = 256;
  std::uint32_t max_state_records = 4096;
  std::uint32_t max_transfer_records = 4096;
  std::uint32_t max_defer_records = 8192;
  std::uint64_t max_transfer_bytes = 1ULL << 40;
  std::uint64_t max_single_payload_bytes = 1ULL << 32;
};

enum class TransferState : std::uint16_t {
  Unknown = 0,
  Registered = 1,
  Authorized = 2,
  InProgress = 3,
  Completed = 4,
  Refused = 5,
  Abandoned = 6,
  Count = 7,
};

[[nodiscard]] std::string_view to_string(TransferState value) noexcept;

struct TransferRegistration {
  StateTransferId id;
  std::uint64_t state_hash = 0;
  std::uint64_t model_hash = 0;
  std::uint64_t payload_bytes = 0;
  TrafficClass declared_class = TrafficClass::Unknown;
  FlowDirection direction = FlowDirection::Lateral;
  IntegrityClass required_integrity = IntegrityClass::ChecksumAndVerify;
  RequestId request;
  AttemptId attempt;
  Instant deadline;
};

struct TransferRecord {
  StateTransferId id;
  std::uint64_t state_hash = 0;
  std::uint64_t model_hash = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t verified_bytes = 0;
  ModelGeneration model_generation;
  StateGeneration state_generation;
  RequestId request;
  AttemptId attempt;
  TrafficClass declared_class = TrafficClass::Unknown;
  FlowDirection direction = FlowDirection::Lateral;
  IntegrityClass required_integrity = IntegrityClass::Unknown;
  TransferState state = TransferState::Unknown;
  ReasonCode terminal_reason = ReasonCode::Unknown;
  Instant deadline;
  Instant registered_at;
  FlowId flow;
};

struct TransferAuthorization {
  StateTransferId transfer;
  TrafficClass traffic_class = TrafficClass::Unknown;
  IntegrityClass integrity = IntegrityClass::Unknown;
  PacingClass pacing = PacingClass::Unknown;
  PriorityClass priority = PriorityClass::Unknown;
  AuthorityStamp authority;
  ReasonCode reason = ReasonCode::Unknown;
  bool authorized = false;
  FlowId flow;
};

struct CoordinatorConfig {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  CoordinatorLimits limits;
  std::uint64_t default_evidence_max_age_nanos = 5000000000ULL;
  std::uint32_t max_retained_completion_records = 4096;
  /// Zero selects a random boot identity. A non-zero seed makes boot
  /// identities reproducible, which the property tests rely upon.
  std::uint64_t boot_seed = 0;
};

struct CoordinatorStatus {
  CoordinatorEpoch epoch;
  BootId incarnation;
  PolicyGeneration policy_generation;
  TopologyGeneration topology_generation;
  EvidenceState topology_state = EvidenceState::Unknown;
  EvidenceProvenance topology_provenance = EvidenceProvenance::Unknown;
  std::size_t live_requests = 0;
  std::size_t transfers = 0;
  std::uint32_t active_flows = 0;
  std::uint64_t authority_sequence = 0;

  [[nodiscard]] std::string to_string() const;
};

/// Authoritative coordinator for inference traffic treatment.
///
/// Locking discipline: every public method takes the internal mutex for the
/// whole operation and releases it before returning. No callback, logging sink
/// or user code is ever invoked while the mutex is held, and no method
/// re-enters the coordinator. export_state() copies under the lock and the
/// caller performs file I/O outside it.
class Coordinator {
 public:
  Coordinator(CoordinatorConfig config, Clock* clock);

  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  /// Fresh boot: advances the epoch past any restored state and marks every
  /// piece of restored dynamic evidence as requiring revalidation.
  Result<CoordinatorEpoch> boot_from(const PersistedCoordinatorState& restored);
  Result<CoordinatorEpoch> boot_fresh();

  [[nodiscard]] AuthorityStamp authority() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] BootId incarnation() const;
  [[nodiscard]] CoordinatorStatus status() const;
  [[nodiscard]] PolicyConfig policy() const;
  [[nodiscard]] Clock& clock() const noexcept { return *clock_; }

  Status apply_policy(PolicyConfig policy);
  Result<TopologyGeneration> publish_topology(TopologyEvidence evidence);
  [[nodiscard]] EvidenceState topology_state() const;
  [[nodiscard]] std::optional<TopologyEvidence> topology() const;

  Result<SloContract> register_slo_contract(RequestId request,
                                            std::uint64_t tail_latency_budget_nanos,
                                            std::uint64_t registration_deadline_nanos,
                                            std::uint32_t min_stream_share_per_mille);
  Result<RouteDecision> issue_route_decision(RequestId request, AttemptId attempt,
                                             std::uint32_t target_node, bool lateral,
                                             bool disaggregated_handoff);

  /// Compare-and-set model generation. A mismatched expectation is refused so
  /// that two writers cannot silently interleave residency changes.
  Result<ModelGeneration> set_model_generation(std::uint64_t model_hash,
                                               std::optional<ModelGeneration> expected_current);
  Result<ModelGeneration> revalidate_model_generation(std::uint64_t model_hash);
  Result<StateGeneration> advance_state_generation(std::uint64_t state_hash,
                                                   std::optional<StateGeneration> expected_current);
  Result<StateGeneration> revalidate_state_generation(std::uint64_t state_hash);
  [[nodiscard]] Result<ModelGeneration> model_generation(std::uint64_t model_hash) const;
  [[nodiscard]] Result<StateGeneration> state_generation(std::uint64_t state_hash) const;

  Result<RequestGeneration> register_request(RequestId request);
  Result<AttemptGeneration> register_attempt(RequestId request, AttemptId attempt,
                                             std::uint64_t model_hash);
  Result<AttemptGeneration> revalidate_request(RequestId request, AttemptId attempt);
  Status advance_stage(RequestId request, AttemptId attempt, ServingStage target);
  Status cancel_request(RequestId request, ReasonCode reason);
  Result<AttemptGeneration> fail_attempt(RequestId request, AttemptId attempt, ReasonCode reason);
  Result<AttemptGeneration> replace_attempt(RequestId request, AttemptId superseded,
                                            AttemptId replacement, std::uint64_t model_hash);

  Result<StateTransferId> register_transfer(const TransferRegistration& registration);
  Result<TransferAuthorization> authorize_transfer(const TrafficRequest& request);
  Status start_transfer(StateTransferId transfer, const TransferAuthorization& authorization);
  Status complete_transfer(StateTransferId transfer, std::uint64_t verified_bytes);
  Status abandon_transfer(StateTransferId transfer, ReasonCode reason);

  Decision evaluate(const TrafficRequest& request);

  /// Re-validates a decision before it is acted upon. Catches cancellation,
  /// supersession and epoch/policy changes that happened after it was minted.
  Status validate_decision(const Decision& decision) const;

  Result<FlowId> open_flow(const Decision& decision, std::uint64_t declared_bytes);
  Status release_flow(FlowId flow);
  Status record_flow_bytes(FlowId flow, std::uint64_t bytes, bool inbound);

  Result<PublishOutcome> publish_completion(const CompletionPublication& publication);

  [[nodiscard]] AccountingSnapshot accounting() const;
  [[nodiscard]] Status verify_invariants() const;
  [[nodiscard]] std::string explain_request(RequestId request) const;
  [[nodiscard]] std::optional<RequestRecord> request_record(RequestId request) const;

  /// Copies a bounded, self-consistent snapshot of durable state. The caller
  /// writes it outside the lock.
  Result<PersistedCoordinatorState> export_state() const;

  /// Restores terminal records and interrupted requests from a loaded
  /// snapshot. Only ever called from boot paths.
  Status restore_state(const PersistedCoordinatorState& state);

 private:
  struct DeferRecord {
    SubjectKind kind = SubjectKind::Unknown;
    RequestId request;
    AttemptId attempt;
    StateTransferId transfer;
    TrafficClass traffic = TrafficClass::Unknown;
    std::uint32_t count = 0;
    std::int64_t first_defer_nanos = 0;
    std::int64_t last_defer_nanos = 0;
    bool starvation_class = false;
  };

  struct FlowRecord {
    FlowId id;
    RequestId request;
    AttemptId attempt;
    StateTransferId transfer;
    TrafficClass traffic = TrafficClass::Unknown;
    std::uint64_t declared_bytes = 0;
    std::uint64_t observed_bytes = 0;
    AuthoritySequence sequence;
    bool bulk = false;
  };

  AuthorityStamp mint_stamp_locked();
  void reseed_boot_identity_locked();
  EvidenceState topology_state_locked(std::int64_t now) const;
  Decision evaluate_locked(const TrafficRequest& request);
  Status validate_decision_locked(const Decision& decision) const;
  Result<FlowId> open_flow_locked(const Decision& decision, std::uint64_t declared_bytes,
                                  std::int64_t now);
  Decision reject_locked(const TrafficRequest& request, const TrafficSubject& subject,
                         ReasonCode reason);
  void account_decision_locked(TrafficClass traffic, DecisionOutcome outcome, std::uint64_t bytes);
  void clear_state_locked();
  [[nodiscard]] std::uint32_t accounting_unlocked_active_flows(
      const AccountingSnapshot& ledger_snapshot) const noexcept;
  Status validate_restored_state_locked(const PersistedCoordinatorState& state) const;
  bool streaming_pressure_locked() const;
  void note_defer_locked(const TrafficSubject& subject, TrafficClass traffic, std::int64_t now,
                         std::uint32_t& count_out);
  void clear_defer_locked(const TrafficSubject& subject, TrafficClass traffic);
  void release_request_flows_locked(RequestId request);
  void release_transfer_flows_locked(StateTransferId transfer);
  void release_flow_locked(const FlowRecord& flow);
  void evict_defer_records_locked();
  [[nodiscard]] std::uint64_t defer_key(const TrafficSubject& subject,
                                        TrafficClass traffic) const noexcept;
  [[nodiscard]] bool defer_matches(const DeferRecord& record,
                                   const TrafficSubject& subject) const noexcept;
  [[nodiscard]] Result<ModelGeneration> require_model_generation_locked(
      std::uint64_t model_hash) const;
  [[nodiscard]] Result<StateGeneration> require_state_generation_locked(
      std::uint64_t state_hash) const;

  CoordinatorConfig config_;
  Clock* clock_;
  mutable std::mutex mutex_;
  CoordinatorEpoch epoch_;
  BootId incarnation_;
  AuthoritySequence authority_sequence_;
  PolicyConfig policy_;
  TopologyEvidence topology_;
  bool topology_present_ = false;
  std::unordered_map<std::uint64_t, ModelState> models_;
  std::unordered_map<std::uint64_t, StateGenerationRecord> states_;
  std::unordered_map<RequestId, SloContract> slo_contracts_;
  std::unordered_map<AttemptId, RouteDecision> route_decisions_;
  std::unordered_map<StateTransferId, TransferRecord> transfers_;
  std::unordered_map<FlowId, FlowRecord> flows_;
  std::unordered_multimap<RequestId, FlowId> flows_by_request_;
  std::unordered_multimap<StateTransferId, FlowId> flows_by_transfer_;
  std::unordered_multimap<RequestId, StateTransferId> transfers_by_request_;
  std::unordered_map<std::uint64_t, DeferRecord> defers_;
  std::unordered_map<RequestId, PublishOutcome> completions_;
  std::deque<RequestId> completion_order_;
  RequestLedger ledger_;
  std::array<ClassAccounting, kTrafficClassCount> transfer_class_accounting_{};
  std::uint64_t boot_count_ = 0;
  std::uint64_t bulk_in_flight_bytes_ = 0;
  std::uint32_t streaming_deferrals_ = 0;
  std::uint64_t transfers_started_ = 0;
  std::uint64_t transfers_completed_ = 0;
  std::uint64_t transfers_refused_ = 0;
  std::uint64_t next_flow_sequence_ = 0;
  std::mt19937_64 random_;
};

}  // namespace itf

#endif  // ITF_COORDINATOR_HPP
