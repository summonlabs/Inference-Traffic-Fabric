// Inference Traffic Fabric - request/attempt traffic lifecycle ledger.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef ITF_LEDGER_HPP
#define ITF_LEDGER_HPP

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "itf/accounting.hpp"
#include "itf/decision.hpp"
#include "itf/error.hpp"
#include "itf/ids.hpp"
#include "itf/time.hpp"
#include "itf/traffic.hpp"

namespace itf {

/// Compile-time ceiling on attempts per request. LedgerLimits may lower it,
/// never raise it, so per-request memory stays fixed.
inline constexpr std::uint32_t kMaxAttemptsPerRequest = 8;

enum class RequestLifecycle : std::uint16_t {
  Unknown = 0,
  Registered = 1,
  Active = 2,
  Completing = 3,
  Completed = 4,
  Cancelled = 5,
  Failed = 6,
  Interrupted = 7,
  Count = 8,
};

[[nodiscard]] std::string_view to_string(RequestLifecycle value) noexcept;
[[nodiscard]] bool is_terminal(RequestLifecycle value) noexcept;

enum class AttemptLifecycle : std::uint16_t {
  Unknown = 0,
  Registered = 1,
  Active = 2,
  Draining = 3,
  Completed = 4,
  Failed = 5,
  Cancelled = 6,
  Superseded = 7,
  Count = 8,
};

[[nodiscard]] std::string_view to_string(AttemptLifecycle value) noexcept;
[[nodiscard]] bool is_terminal(AttemptLifecycle value) noexcept;

struct LedgerLimits {
  std::uint32_t max_live_requests = 4096;
  std::uint32_t max_attempts_per_request = kMaxAttemptsPerRequest;
  std::uint32_t max_retained_terminal_requests = 2048;
  std::uint32_t max_active_flows_per_request = 64;
  std::uint32_t max_active_flows_total = 65536;
};

struct AttemptRecord {
  AttemptId id;
  AttemptGeneration generation;
  ServingStage stage = ServingStage::Unknown;
  AttemptLifecycle lifecycle = AttemptLifecycle::Unknown;
  std::uint64_t model_hash = 0;
  ModelGeneration model_generation;
  RouteDecisionGeneration route_generation;
  bool success_published = false;
  bool failure_published = false;
  ReasonCode terminal_reason = ReasonCode::Unknown;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint32_t decisions = 0;
  std::uint32_t defers = 0;
  std::uint32_t rejections = 0;
  std::int64_t registered_at_nanos = 0;
  std::int64_t updated_at_nanos = 0;
};

struct RequestRecord {
  RequestId id;
  RequestGeneration generation;
  RequestLifecycle lifecycle = RequestLifecycle::Unknown;
  ServingStage stage = ServingStage::Unknown;
  AttemptId current_attempt;
  AttemptGeneration current_attempt_generation;
  std::uint32_t attempt_count = 0;
  std::array<AttemptRecord, kMaxAttemptsPerRequest> attempts{};
  ReasonCode terminal_reason = ReasonCode::Unknown;
  std::int64_t registered_at_nanos = 0;
  std::int64_t terminal_at_nanos = 0;
  bool requires_revalidation = false;
  bool accounting_closed = false;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint32_t decisions = 0;
  std::uint32_t defers = 0;
  std::uint32_t rejections = 0;
  std::uint32_t active_flows = 0;
  /// Authority sequence floor: no decision minted at or below this sequence may
  /// open a flow for this request again. Cancellation raises it.
  AuthoritySequence authority_floor;

  [[nodiscard]] const AttemptRecord* find_attempt(AttemptId id) const noexcept;
  [[nodiscard]] AttemptRecord* find_attempt(AttemptId id) noexcept;
  [[nodiscard]] bool is_current_attempt(AttemptId id) const noexcept;
};

struct CompletionPublication {
  RequestId request;
  AttemptId attempt;
  AttemptGeneration attempt_generation;
  bool success = true;
  ReasonCode reason = ReasonCode::Unknown;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
};

struct PublishOutcome {
  enum class Disposition : std::uint16_t {
    Unknown = 0,
    Committed = 1,
    DuplicateSuppressed = 2,
    Refused = 3,
    Count = 4,
  };

  Disposition disposition = Disposition::Unknown;
  StatusCode refusal = StatusCode::Ok;
  ReasonCode reason = ReasonCode::Unknown;
  std::uint64_t accounted_bytes_in = 0;
  std::uint64_t accounted_bytes_out = 0;
  std::uint32_t released_flows = 0;

  [[nodiscard]] bool committed() const noexcept { return disposition == Disposition::Committed; }
  [[nodiscard]] bool refused() const noexcept { return disposition == Disposition::Refused; }
};

/// Deterministic request/attempt lifecycle state machine plus per-flow
/// accounting. All operations are total: they either apply exactly one legal
/// transition or return a deterministic failure code, and they never mutate
/// state partially.
class RequestLedger {
 public:
  explicit RequestLedger(LedgerLimits limits = LedgerLimits{});

  [[nodiscard]] const LedgerLimits& limits() const noexcept { return limits_; }

  Result<RequestGeneration> register_request(RequestId id, std::int64_t now_nanos);
  Result<AttemptGeneration> register_attempt(RequestId id, AttemptId attempt,
                                             std::uint64_t model_hash,
                                             ModelGeneration model_generation,
                                             RouteDecisionGeneration route_generation,
                                             std::int64_t now_nanos);
  /// Rebinds the model identity of an existing attempt during a replacement.
  Status set_attempt_model(RequestId id, AttemptId attempt, std::uint64_t model_hash,
                           ModelGeneration model_generation);
  /// Re-establishes authority for a request that was interrupted by a
  /// coordinator restart. Returns the new attempt generation.
  Result<AttemptGeneration> revalidate_request(RequestId id, AttemptId attempt,
                                               std::int64_t now_nanos);

  Status advance_stage(RequestId id, AttemptId attempt, ServingStage target,
                       std::int64_t now_nanos);
  Result<AttemptGeneration> fail_attempt(RequestId id, AttemptId attempt, ReasonCode reason,
                                         std::int64_t now_nanos);
  Result<AttemptGeneration> supersede_attempt(RequestId id, AttemptId attempt,
                                              AttemptId replacement, std::int64_t now_nanos);
  Status cancel_request(RequestId id, ReasonCode reason, AuthoritySequence sequence,
                        std::int64_t now_nanos);
  Status mark_request_interrupted(RequestId id, std::int64_t now_nanos);

  Result<PublishOutcome> publish_completion(const CompletionPublication& publication,
                                            std::int64_t now_nanos);

  Result<FlowId> open_flow(RequestId id, AttemptId attempt, TrafficClass traffic,
                           std::uint64_t declared_bytes, std::int64_t now_nanos);
  Status release_flow(FlowId flow, std::int64_t now_nanos);
  Status record_flow_bytes(FlowId flow, std::uint64_t bytes, bool inbound);
  Status record_decision(RequestId id, AttemptId attempt, TrafficClass traffic,
                         DecisionOutcome outcome, ReasonCode reason, std::uint64_t bytes);

  [[nodiscard]] bool contains(RequestId id) const;
  [[nodiscard]] bool is_cancelled(RequestId id) const;
  [[nodiscard]] std::optional<RequestRecord> snapshot_request(RequestId id) const;
  [[nodiscard]] std::optional<RequestAccounting> snapshot_accounting(RequestId id) const;

  [[nodiscard]] const AccountingSnapshot& accounting() const noexcept { return accounting_; }
  /// Recomputes closure-sensitive counters from live records and compares them
  /// with the running counters. Any divergence is a defect, not a warning.
  [[nodiscard]] Status verify_closure() const;

  /// Restores a terminal or interrupted record during recovery. Live records
  /// are never restored as live authority.
  Status restore_record(const RequestRecord& record);

  [[nodiscard]] std::vector<RequestRecord> export_terminal_records(std::size_t max_records) const;
  /// Every non-terminal record. Restoring one yields an interrupted request
  /// that must re-establish authority rather than inheriting it.
  [[nodiscard]] std::vector<RequestRecord> export_live_records() const;

  void clear();

 private:
  struct FlowRecord {
    FlowId id;
    RequestId request;
    AttemptId attempt;
    TrafficClass traffic = TrafficClass::Unknown;
    std::uint64_t declared_bytes = 0;
    std::uint64_t observed_bytes = 0;
    std::int64_t opened_at_nanos = 0;
  };

  void evict_if_needed();
  void refresh_request_counts();
  void release_request_flows(RequestRecord& record);
  void account_flow_open(TrafficClass traffic);
  void account_flow_close(TrafficClass traffic);

  LedgerLimits limits_;
  std::unordered_map<RequestId, RequestRecord> requests_;
  std::deque<RequestId> terminal_order_;
  std::unordered_map<FlowId, FlowRecord> flows_;
  std::unordered_multimap<RequestId, FlowId> flows_by_request_;
  std::uint64_t next_flow_sequence_ = 0;
  AccountingSnapshot accounting_;
};

}  // namespace itf

#endif  // ITF_LEDGER_HPP
