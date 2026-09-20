// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/ledger.hpp"

#include <algorithm>

#include "itf/bytes.hpp"
#include <string>
#include <utility>

namespace itf {
namespace {

[[nodiscard]] bool is_live_lifecycle(RequestLifecycle lifecycle) noexcept {
  return lifecycle == RequestLifecycle::Registered || lifecycle == RequestLifecycle::Active ||
         lifecycle == RequestLifecycle::Completing || lifecycle == RequestLifecycle::Interrupted;
}

}  // namespace

std::string_view to_string(RequestLifecycle value) noexcept {
  switch (value) {
    case RequestLifecycle::Unknown: return "UNKNOWN";
    case RequestLifecycle::Registered: return "REGISTERED";
    case RequestLifecycle::Active: return "ACTIVE";
    case RequestLifecycle::Completing: return "COMPLETING";
    case RequestLifecycle::Completed: return "COMPLETED";
    case RequestLifecycle::Cancelled: return "CANCELLED";
    case RequestLifecycle::Failed: return "FAILED";
    case RequestLifecycle::Interrupted: return "INTERRUPTED";
    case RequestLifecycle::Count: return "COUNT";
  }
  return "UNKNOWN";
}

bool is_terminal(RequestLifecycle value) noexcept {
  return value == RequestLifecycle::Completed || value == RequestLifecycle::Cancelled ||
         value == RequestLifecycle::Failed;
}

std::string_view to_string(AttemptLifecycle value) noexcept {
  switch (value) {
    case AttemptLifecycle::Unknown: return "UNKNOWN";
    case AttemptLifecycle::Registered: return "REGISTERED";
    case AttemptLifecycle::Active: return "ACTIVE";
    case AttemptLifecycle::Draining: return "DRAINING";
    case AttemptLifecycle::Completed: return "COMPLETED";
    case AttemptLifecycle::Failed: return "FAILED";
    case AttemptLifecycle::Cancelled: return "CANCELLED";
    case AttemptLifecycle::Superseded: return "SUPERSEDED";
    case AttemptLifecycle::Count: return "COUNT";
  }
  return "UNKNOWN";
}

bool is_terminal(AttemptLifecycle value) noexcept {
  return value == AttemptLifecycle::Completed || value == AttemptLifecycle::Failed ||
         value == AttemptLifecycle::Cancelled || value == AttemptLifecycle::Superseded;
}

const AttemptRecord* RequestRecord::find_attempt(AttemptId attempt_id) const noexcept {
  for (std::uint32_t index = 0; index < attempt_count && index < kMaxAttemptsPerRequest; ++index) {
    if (attempts[index].id == attempt_id) return &attempts[index];
  }
  return nullptr;
}

AttemptRecord* RequestRecord::find_attempt(AttemptId attempt_id) noexcept {
  for (std::uint32_t index = 0; index < attempt_count && index < kMaxAttemptsPerRequest; ++index) {
    if (attempts[index].id == attempt_id) return &attempts[index];
  }
  return nullptr;
}

bool RequestRecord::is_current_attempt(AttemptId attempt_id) const noexcept {
  return !current_attempt.is_nil() && current_attempt == attempt_id;
}

RequestLedger::RequestLedger(LedgerLimits limits) : limits_(limits) {
  if (limits_.max_attempts_per_request == 0 ||
      limits_.max_attempts_per_request > kMaxAttemptsPerRequest) {
    limits_.max_attempts_per_request = kMaxAttemptsPerRequest;
  }
  if (limits_.max_live_requests == 0) limits_.max_live_requests = 1;
  if (limits_.max_active_flows_total == 0) limits_.max_active_flows_total = 1;
  if (limits_.max_active_flows_per_request == 0) limits_.max_active_flows_per_request = 1;
  const std::size_t reserve_target =
      std::min<std::size_t>(static_cast<std::size_t>(limits_.max_live_requests) +
                                static_cast<std::size_t>(limits_.max_retained_terminal_requests),
                            65536U);
  requests_.reserve(reserve_target);
  flows_.reserve(std::min<std::size_t>(limits_.max_active_flows_total, 65536U));
}

void RequestLedger::clear() {
  requests_.clear();
  terminal_order_.clear();
  flows_.clear();
  flows_by_request_.clear();
  next_flow_sequence_ = 0;
  accounting_ = AccountingSnapshot{};
}

void RequestLedger::account_flow_open(TrafficClass traffic) {
  accounting_.active_flows_total += 1;
  if (accounting_.active_flows_total > accounting_.peak_active_flows_total) {
    accounting_.peak_active_flows_total = accounting_.active_flows_total;
  }
  const std::size_t index = static_cast<std::size_t>(traffic);
  if (index >= kTrafficClassCount) return;
  ClassAccounting& entry = accounting_.per_class[index];
  entry.active_flows += 1;
  if (entry.active_flows > entry.peak_active_flows) entry.peak_active_flows = entry.active_flows;
}

void RequestLedger::account_flow_close(TrafficClass traffic) {
  if (accounting_.active_flows_total > 0) accounting_.active_flows_total -= 1;
  const std::size_t index = static_cast<std::size_t>(traffic);
  if (index >= kTrafficClassCount) return;
  ClassAccounting& entry = accounting_.per_class[index];
  if (entry.active_flows > 0) entry.active_flows -= 1;
}

void RequestLedger::release_request_flows(RequestRecord& record) {
  const auto range = flows_by_request_.equal_range(record.id);
  for (auto iterator = range.first; iterator != range.second;) {
    const auto flow_iterator = flows_.find(iterator->second);
    if (flow_iterator != flows_.end()) {
      account_flow_close(flow_iterator->second.traffic);
      flows_.erase(flow_iterator);
    }
    iterator = flows_by_request_.erase(iterator);
  }
  record.active_flows = 0;
  record.accounting_closed = true;
}

Result<RequestGeneration> RequestLedger::register_request(RequestId id, std::int64_t now_nanos) {
  if (id.is_nil()) {
    return Result<RequestGeneration>::failure(StatusCode::InvalidArgument, "nil request id");
  }
  const auto existing = requests_.find(id);
  if (existing != requests_.end()) {
    if (is_terminal(existing->second.lifecycle)) {
      return Result<RequestGeneration>::failure(
          StatusCode::Conflict, "request id already exists in terminal state");
    }
    return Result<RequestGeneration>::failure(StatusCode::AlreadyExists,
                                              "request id already registered");
  }
  const std::size_t live = requests_.size() - terminal_order_.size();
  if (live >= limits_.max_live_requests) {
    return Result<RequestGeneration>::failure(StatusCode::CapacityExhausted,
                                              "live request limit reached");
  }
  RequestRecord record;
  record.id = id;
  record.generation = RequestGeneration::from(1);
  record.lifecycle = RequestLifecycle::Registered;
  record.stage = ServingStage::Unknown;
  record.registered_at_nanos = now_nanos;
  requests_.emplace(id, record);
  accounting_.requests_registered += 1;
  refresh_request_counts();
  return Result<RequestGeneration>::success(record.generation);
}

Result<AttemptGeneration> RequestLedger::register_attempt(RequestId id, AttemptId attempt,
                                                          std::uint64_t model_hash,
                                                          ModelGeneration model_generation,
                                                          RouteDecisionGeneration route_generation,
                                                          std::int64_t now_nanos) {
  if (attempt.is_nil()) {
    return Result<AttemptGeneration>::failure(StatusCode::InvalidArgument, "nil attempt id");
  }
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) {
    return Result<AttemptGeneration>::failure(StatusCode::NotFound, "unknown request");
  }
  RequestRecord& record = iterator->second;
  if (is_terminal(record.lifecycle)) {
    return Result<AttemptGeneration>::failure(StatusCode::Conflict,
                                              "request is terminal; a new attempt cannot be bound");
  }
  if (record.requires_revalidation) {
    return Result<AttemptGeneration>::failure(
        StatusCode::RevalidationRequired,
        "request interrupted by coordinator restart requires revalidation");
  }
  if (record.find_attempt(attempt) != nullptr) {
    return Result<AttemptGeneration>::failure(StatusCode::AlreadyExists,
                                              "attempt id already bound to this request");
  }
  if (record.attempt_count >= limits_.max_attempts_per_request) {
    return Result<AttemptGeneration>::failure(StatusCode::CapacityExhausted,
                                              "attempt limit reached for request");
  }
  AttemptRecord entry;
  entry.id = attempt;
  entry.generation = AttemptGeneration::from(record.attempt_count + 1);
  entry.stage = ServingStage::Unknown;
  entry.lifecycle = AttemptLifecycle::Registered;
  entry.model_hash = model_hash;
  entry.model_generation = model_generation;
  entry.route_generation = route_generation;
  entry.registered_at_nanos = now_nanos;
  entry.updated_at_nanos = now_nanos;
  record.attempts[record.attempt_count] = entry;
  record.attempt_count += 1;
  record.current_attempt = attempt;
  record.current_attempt_generation = entry.generation;
  record.stage = ServingStage::Unknown;
  if (record.lifecycle == RequestLifecycle::Interrupted) {
    record.lifecycle = RequestLifecycle::Active;
  }
  record.requires_revalidation = false;
  accounting_.attempts_registered += 1;
  return Result<AttemptGeneration>::success(entry.generation);
}

Result<AttemptGeneration> RequestLedger::revalidate_request(RequestId id, AttemptId attempt,
                                                            std::int64_t now_nanos) {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) {
    return Result<AttemptGeneration>::failure(StatusCode::NotFound, "unknown request");
  }
  RequestRecord& record = iterator->second;
  if (is_terminal(record.lifecycle)) {
    return Result<AttemptGeneration>::failure(StatusCode::Conflict,
                                              "terminal request cannot be revalidated");
  }
  if (!record.requires_revalidation) {
    return Result<AttemptGeneration>::failure(StatusCode::Conflict,
                                              "request does not require revalidation");
  }
  if (attempt.is_nil()) {
    return Result<AttemptGeneration>::failure(StatusCode::InvalidArgument, "nil attempt id");
  }
  if (record.attempt_count >= limits_.max_attempts_per_request) {
    return Result<AttemptGeneration>::failure(StatusCode::CapacityExhausted,
                                              "attempt limit reached for request");
  }
  AttemptRecord entry;
  entry.id = attempt;
  entry.generation = AttemptGeneration::from(record.attempt_count + 1);
  entry.stage = ServingStage::Unknown;
  entry.lifecycle = AttemptLifecycle::Registered;
  entry.registered_at_nanos = now_nanos;
  entry.updated_at_nanos = now_nanos;
  record.attempts[record.attempt_count] = entry;
  record.attempt_count += 1;
  record.current_attempt = attempt;
  record.current_attempt_generation = entry.generation;
  record.stage = ServingStage::Unknown;
  record.lifecycle = RequestLifecycle::Active;
  record.requires_revalidation = false;
  record.generation = record.generation.next();
  accounting_.attempts_registered += 1;
  return Result<AttemptGeneration>::success(entry.generation);
}

Status RequestLedger::advance_stage(RequestId id, AttemptId attempt, ServingStage target,
                                    std::int64_t now_nanos) {
  if (is_terminal(target)) {
    return Status(StatusCode::InvalidArgument,
                  "terminal stages are reached through publish_completion or cancel_request");
  }
  if (target == ServingStage::Unknown) {
    return Status(StatusCode::InvalidArgument, "stage cannot regress to UNKNOWN");
  }
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return Status(StatusCode::NotFound, "unknown request");
  RequestRecord& record = iterator->second;
  if (record.lifecycle == RequestLifecycle::Cancelled) {
    return Status(StatusCode::Cancelled, "request is cancelled");
  }
  if (is_terminal(record.lifecycle)) {
    return Status(StatusCode::Conflict, "request is terminal");
  }
  if (record.requires_revalidation) {
    return Status(StatusCode::RevalidationRequired,
                  "request requires revalidation after coordinator restart");
  }
  AttemptRecord* entry = record.find_attempt(attempt);
  if (entry == nullptr) return Status(StatusCode::NotFound, "unknown attempt for request");
  if (!record.is_current_attempt(attempt)) {
    return Status(StatusCode::StaleGeneration, "attempt is no longer current");
  }
  if (is_terminal(entry->lifecycle)) {
    return Status(StatusCode::Conflict, "attempt is terminal");
  }
  if (!is_legal_stage_transition(entry->stage, target)) {
    std::string detail = "illegal stage transition ";
    detail.append(itf::to_string(entry->stage));
    detail.append(" -> ");
    detail.append(itf::to_string(target));
    return Status(StatusCode::InvalidTransition, std::move(detail));
  }
  entry->stage = target;
  entry->updated_at_nanos = now_nanos;
  if (entry->lifecycle == AttemptLifecycle::Registered) {
    entry->lifecycle = AttemptLifecycle::Active;
  }
  if (target == ServingStage::Completing) entry->lifecycle = AttemptLifecycle::Draining;
  record.stage = target;
  if (target == ServingStage::Completing) record.lifecycle = RequestLifecycle::Completing;
  return Status::success();
}

Result<AttemptGeneration> RequestLedger::fail_attempt(RequestId id, AttemptId attempt,
                                                      ReasonCode reason, std::int64_t now_nanos) {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) {
    return Result<AttemptGeneration>::failure(StatusCode::NotFound, "unknown request");
  }
  RequestRecord& record = iterator->second;
  AttemptRecord* entry = record.find_attempt(attempt);
  if (entry == nullptr) {
    return Result<AttemptGeneration>::failure(StatusCode::NotFound, "unknown attempt");
  }
  if (!record.is_current_attempt(attempt)) {
    return Result<AttemptGeneration>::failure(StatusCode::StaleGeneration,
                                              "attempt is no longer current");
  }
  if (is_terminal(entry->lifecycle)) {
    if (entry->lifecycle == AttemptLifecycle::Failed) {
      return Result<AttemptGeneration>::success(entry->generation);
    }
    return Result<AttemptGeneration>::failure(StatusCode::Conflict, "attempt is terminal");
  }
  entry->lifecycle = AttemptLifecycle::Failed;
  entry->stage = ServingStage::Failed;
  entry->failure_published = true;
  entry->terminal_reason = reason;
  entry->updated_at_nanos = now_nanos;
  record.lifecycle = RequestLifecycle::Failed;
  record.stage = ServingStage::Failed;
  record.terminal_reason = reason;
  record.terminal_at_nanos = now_nanos;
  release_request_flows(record);
  terminal_order_.push_back(id);
  accounting_.attempts_failed += 1;
  accounting_.requests_failed += 1;
  evict_if_needed();
  return Result<AttemptGeneration>::success(entry->generation);
}

Result<AttemptGeneration> RequestLedger::supersede_attempt(RequestId id, AttemptId attempt,
                                                           AttemptId replacement,
                                                           std::int64_t now_nanos) {
  if (replacement.is_nil()) {
    return Result<AttemptGeneration>::failure(StatusCode::InvalidArgument, "nil replacement id");
  }
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) {
    return Result<AttemptGeneration>::failure(StatusCode::NotFound, "unknown request");
  }
  RequestRecord& record = iterator->second;
  if (is_terminal(record.lifecycle)) {
    return Result<AttemptGeneration>::failure(StatusCode::Conflict, "request is terminal");
  }
  AttemptRecord* entry = record.find_attempt(attempt);
  if (entry == nullptr) {
    return Result<AttemptGeneration>::failure(StatusCode::NotFound, "unknown attempt");
  }
  if (!record.is_current_attempt(attempt)) {
    return Result<AttemptGeneration>::failure(StatusCode::StaleGeneration,
                                              "attempt is no longer current");
  }
  if (record.find_attempt(replacement) != nullptr) {
    return Result<AttemptGeneration>::failure(StatusCode::AlreadyExists,
                                              "replacement attempt id already used");
  }
  if (record.attempt_count >= limits_.max_attempts_per_request) {
    return Result<AttemptGeneration>::failure(StatusCode::CapacityExhausted,
                                              "attempt limit reached for request");
  }
  const ModelGeneration model_generation = entry->model_generation;
  const std::uint64_t model_hash = entry->model_hash;
  entry->lifecycle = AttemptLifecycle::Superseded;
  entry->terminal_reason = ReasonCode::AttemptSuperseded;
  entry->updated_at_nanos = now_nanos;

  AttemptRecord fresh;
  fresh.id = replacement;
  fresh.generation = AttemptGeneration::from(record.attempt_count + 1);
  fresh.stage = ServingStage::Unknown;
  fresh.lifecycle = AttemptLifecycle::Registered;
  fresh.model_hash = model_hash;
  fresh.model_generation = model_generation;
  fresh.registered_at_nanos = now_nanos;
  fresh.updated_at_nanos = now_nanos;
  record.attempts[record.attempt_count] = fresh;
  record.attempt_count += 1;
  record.current_attempt = replacement;
  record.current_attempt_generation = fresh.generation;
  record.stage = ServingStage::Unknown;
  record.generation = record.generation.next();
  accounting_.attempts_superseded += 1;
  accounting_.attempts_registered += 1;
  return Result<AttemptGeneration>::success(fresh.generation);
}

Status RequestLedger::set_attempt_model(RequestId id, AttemptId attempt, std::uint64_t model_hash,
                                       ModelGeneration model_generation) {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return Status(StatusCode::NotFound, "unknown request");
  AttemptRecord* entry = iterator->second.find_attempt(attempt);
  if (entry == nullptr) return Status(StatusCode::NotFound, "unknown attempt");
  entry->model_hash = model_hash;
  entry->model_generation = model_generation;
  return Status::success();
}

Status RequestLedger::cancel_request(RequestId id, ReasonCode reason, AuthoritySequence sequence,
                                     std::int64_t now_nanos) {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return Status(StatusCode::NotFound, "unknown request");
  RequestRecord& record = iterator->second;
  if (record.lifecycle == RequestLifecycle::Cancelled) {
    // Cancellation is idempotent; the authority floor is never lowered.
    if (!sequence.is_unset() && sequence > record.authority_floor) {
      record.authority_floor = sequence;
    }
    return Status::success();
  }
  if (is_terminal(record.lifecycle)) {
    return Status(StatusCode::Conflict, "request already completed or failed");
  }
  record.lifecycle = RequestLifecycle::Cancelled;
  record.stage = ServingStage::Cancelled;
  record.terminal_reason = reason;
  record.terminal_at_nanos = now_nanos;
  record.requires_revalidation = false;
  if (!sequence.is_unset() && sequence > record.authority_floor) {
    record.authority_floor = sequence;
  }
  for (std::uint32_t index = 0; index < record.attempt_count && index < kMaxAttemptsPerRequest;
       ++index) {
    AttemptRecord& entry = record.attempts[index];
    if (!is_terminal(entry.lifecycle)) {
      entry.lifecycle = AttemptLifecycle::Cancelled;
      entry.stage = ServingStage::Cancelled;
      entry.terminal_reason = reason;
      entry.updated_at_nanos = now_nanos;
    }
  }
  release_request_flows(record);
  terminal_order_.push_back(id);
  accounting_.requests_cancelled += 1;
  evict_if_needed();
  return Status::success();
}

Status RequestLedger::mark_request_interrupted(RequestId id, std::int64_t now_nanos) {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return Status(StatusCode::NotFound, "unknown request");
  RequestRecord& record = iterator->second;
  if (is_terminal(record.lifecycle)) return Status::success();
  record.lifecycle = RequestLifecycle::Interrupted;
  record.requires_revalidation = true;
  for (std::uint32_t index = 0; index < record.attempt_count && index < kMaxAttemptsPerRequest;
       ++index) {
    AttemptRecord& entry = record.attempts[index];
    if (!is_terminal(entry.lifecycle)) {
      entry.lifecycle = AttemptLifecycle::Cancelled;
      entry.terminal_reason = ReasonCode::Cancelled;
      entry.updated_at_nanos = now_nanos;
    }
  }
  release_request_flows(record);
  record.accounting_closed = false;
  accounting_.requests_interrupted += 1;
  return Status::success();
}

Result<PublishOutcome> RequestLedger::publish_completion(const CompletionPublication& publication,
                                                         std::int64_t now_nanos) {
  if (publication.request.is_nil() || publication.attempt.is_nil()) {
    return Result<PublishOutcome>::failure(StatusCode::InvalidArgument,
                                           "completion publication requires request and attempt");
  }
  const auto iterator = requests_.find(publication.request);
  if (iterator == requests_.end()) {
    return Result<PublishOutcome>::failure(StatusCode::NotFound, "unknown request");
  }
  RequestRecord& record = iterator->second;
  PublishOutcome outcome;
  const AttemptRecord* found = record.find_attempt(publication.attempt);
  if (found == nullptr) {
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::StaleGeneration;
    outcome.reason = ReasonCode::RejectedUnknownAttempt;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (!publication.attempt_generation.is_unset() &&
      publication.attempt_generation != found->generation) {
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::StaleGeneration;
    outcome.reason = ReasonCode::RejectedStaleAttempt;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (record.lifecycle == RequestLifecycle::Cancelled) {
    // Closure invariant: cancellation permanently removes network authority.
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::Cancelled;
    outcome.reason = ReasonCode::RejectedRequestCancelled;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (record.lifecycle == RequestLifecycle::Completed) {
    outcome.disposition = PublishOutcome::Disposition::DuplicateSuppressed;
    outcome.reason = ReasonCode::DuplicateCompletionSuppressed;
    accounting_.completions_suppressed += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (is_terminal(record.lifecycle)) {
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::Conflict;
    outcome.reason = ReasonCode::RejectedRequestTerminal;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  const std::uint32_t attempt_index = static_cast<std::uint32_t>(found - record.attempts.data());
  AttemptRecord& entry = record.attempts[attempt_index];
  if (entry.lifecycle == AttemptLifecycle::Superseded) {
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::StaleGeneration;
    outcome.reason = ReasonCode::RejectedStaleAttempt;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (entry.lifecycle == AttemptLifecycle::Failed) {
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::Conflict;
    outcome.reason = ReasonCode::AttemptFailed;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (entry.lifecycle == AttemptLifecycle::Cancelled) {
    outcome.disposition = PublishOutcome::Disposition::Refused;
    outcome.refusal = StatusCode::Cancelled;
    outcome.reason = ReasonCode::RejectedRequestCancelled;
    accounting_.completions_refused += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  if (!publication.success) {
    if (entry.failure_published) {
      outcome.disposition = PublishOutcome::Disposition::DuplicateSuppressed;
      outcome.reason = ReasonCode::DuplicateCompletionSuppressed;
      accounting_.completions_suppressed += 1;
      return Result<PublishOutcome>::success(outcome);
    }
    entry.failure_published = true;
    entry.lifecycle = AttemptLifecycle::Failed;
    entry.stage = ServingStage::Failed;
    entry.terminal_reason =
        publication.reason == ReasonCode::Unknown ? ReasonCode::AttemptFailed
                                                  : publication.reason;
    entry.updated_at_nanos = now_nanos;
    entry.bytes_in = std::max(entry.bytes_in, publication.bytes_in);
    entry.bytes_out = std::max(entry.bytes_out, publication.bytes_out);
    record.lifecycle = RequestLifecycle::Failed;
    record.stage = ServingStage::Failed;
    record.terminal_reason = entry.terminal_reason;
    record.terminal_at_nanos = now_nanos;
    record.bytes_in = std::max(record.bytes_in, publication.bytes_in);
    record.bytes_out = std::max(record.bytes_out, publication.bytes_out);
    release_request_flows(record);
    terminal_order_.push_back(record.id);
    accounting_.attempts_failed += 1;
    accounting_.requests_failed += 1;
    outcome.disposition = PublishOutcome::Disposition::Committed;
    outcome.reason = ReasonCode::AttemptFailed;
    evict_if_needed();
    return Result<PublishOutcome>::success(outcome);
  }
  if (entry.success_published) {
    outcome.disposition = PublishOutcome::Disposition::DuplicateSuppressed;
    outcome.reason = ReasonCode::DuplicateCompletionSuppressed;
    accounting_.completions_suppressed += 1;
    return Result<PublishOutcome>::success(outcome);
  }
  entry.success_published = true;
  entry.lifecycle = AttemptLifecycle::Completed;
  entry.stage = ServingStage::Completed;
  entry.updated_at_nanos = now_nanos;
  // The publication carries authoritative totals; live flow reporting may
  // already have accounted part of them, so the committed values are the
  // maximum of the two views and are never summed.
  entry.bytes_in = std::max(entry.bytes_in, publication.bytes_in);
  entry.bytes_out = std::max(entry.bytes_out, publication.bytes_out);
  record.lifecycle = RequestLifecycle::Completed;
  record.stage = ServingStage::Completed;
  record.terminal_reason = ReasonCode::CommittedCompletion;
  record.terminal_at_nanos = now_nanos;
  record.bytes_in = std::max(record.bytes_in, publication.bytes_in);
  record.bytes_out = std::max(record.bytes_out, publication.bytes_out);
  outcome.accounted_bytes_in = publication.bytes_in;
  outcome.accounted_bytes_out = publication.bytes_out;
  outcome.released_flows = record.active_flows;
  release_request_flows(record);
  terminal_order_.push_back(record.id);
  accounting_.attempts_completed += 1;
  accounting_.requests_completed += 1;
  accounting_.completions_committed += 1;
  outcome.disposition = PublishOutcome::Disposition::Committed;
  outcome.reason = ReasonCode::CommittedCompletion;
  evict_if_needed();
  return Result<PublishOutcome>::success(outcome);
}

Result<FlowId> RequestLedger::open_flow(RequestId id, AttemptId attempt, TrafficClass traffic,
                                        std::uint64_t declared_bytes, std::int64_t now_nanos) {
  if (traffic == TrafficClass::Unknown || traffic >= TrafficClass::Count) {
    return Result<FlowId>::failure(StatusCode::InvalidArgument, "unknown traffic class for flow");
  }
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) {
    return Result<FlowId>::failure(StatusCode::NotFound, "unknown request");
  }
  RequestRecord& record = iterator->second;
  if (record.lifecycle == RequestLifecycle::Cancelled) {
    return Result<FlowId>::failure(StatusCode::Cancelled, "cancelled request cannot open a flow");
  }
  if (is_terminal(record.lifecycle)) {
    return Result<FlowId>::failure(StatusCode::Conflict, "terminal request cannot open a flow");
  }
  if (record.requires_revalidation) {
    return Result<FlowId>::failure(StatusCode::RevalidationRequired,
                                   "request requires revalidation");
  }
  const AttemptRecord* entry = record.find_attempt(attempt);
  if (entry == nullptr) return Result<FlowId>::failure(StatusCode::NotFound, "unknown attempt");
  if (!record.is_current_attempt(attempt)) {
    return Result<FlowId>::failure(StatusCode::StaleGeneration, "attempt is no longer current");
  }
  if (entry->lifecycle == AttemptLifecycle::Superseded) {
    return Result<FlowId>::failure(StatusCode::StaleGeneration, "attempt was superseded");
  }
  if (is_terminal(entry->lifecycle)) {
    return Result<FlowId>::failure(StatusCode::Conflict, "terminal attempt cannot open a flow");
  }
  if (record.active_flows >= limits_.max_active_flows_per_request) {
    return Result<FlowId>::failure(StatusCode::CapacityExhausted,
                                   "per-request active flow limit reached");
  }
  if (accounting_.active_flows_total >= limits_.max_active_flows_total) {
    return Result<FlowId>::failure(StatusCode::CapacityExhausted, "global active flow limit reached");
  }
  next_flow_sequence_ += 1;
  const std::uint64_t mixed = id.high() ^ (id.low() * 0x9E3779B97F4A7C15ULL) ^
                              (attempt.high() * 0xBF58476D1CE4E5B9ULL) ^ attempt.low() ^
                              (static_cast<std::uint64_t>(traffic) << 56U);
  const FlowId flow(next_flow_sequence_, mixed);
  FlowRecord flow_record;
  flow_record.id = flow;
  flow_record.request = id;
  flow_record.traffic = traffic;
  flow_record.declared_bytes = declared_bytes;
  flow_record.opened_at_nanos = now_nanos;
  flows_.emplace(flow, flow_record);
  flows_by_request_.emplace(id, flow);
  record.active_flows += 1;
  account_flow_open(traffic);
  return Result<FlowId>::success(flow);
}

Status RequestLedger::release_flow(FlowId flow, std::int64_t /*now_nanos*/) {
  const auto iterator = flows_.find(flow);
  if (iterator == flows_.end()) return Status(StatusCode::NotFound, "unknown flow");
  const FlowRecord record = iterator->second;
  account_flow_close(record.traffic);
  flows_.erase(iterator);
  const auto range = flows_by_request_.equal_range(record.request);
  for (auto entry = range.first; entry != range.second; ++entry) {
    if (entry->second == flow) {
      flows_by_request_.erase(entry);
      break;
    }
  }
  const auto request_iterator = requests_.find(record.request);
  if (request_iterator != requests_.end() && request_iterator->second.active_flows > 0) {
    request_iterator->second.active_flows -= 1;
  }
  return Status::success();
}

Status RequestLedger::record_flow_bytes(FlowId flow, std::uint64_t bytes, bool inbound) {
  const auto iterator = flows_.find(flow);
  if (iterator == flows_.end()) return Status(StatusCode::NotFound, "unknown flow");
  FlowRecord& record = iterator->second;
  std::uint64_t updated = 0;
  if (!checked_add(record.observed_bytes, bytes, updated)) {
    return Status(StatusCode::BoundsExceeded, "flow byte accounting overflow");
  }
  record.observed_bytes = updated;
  const auto request_iterator = requests_.find(record.request);
  if (request_iterator == requests_.end()) return Status::success();
  RequestRecord& request = request_iterator->second;
  std::uint64_t total = 0;
  if (inbound) {
    if (!checked_add(request.bytes_in, bytes, total)) {
      return Status(StatusCode::BoundsExceeded, "request inbound byte accounting overflow");
    }
    request.bytes_in = total;
  } else {
    if (!checked_add(request.bytes_out, bytes, total)) {
      return Status(StatusCode::BoundsExceeded, "request outbound byte accounting overflow");
    }
    request.bytes_out = total;
  }
  return Status::success();
}

Status RequestLedger::record_decision(RequestId id, AttemptId attempt, TrafficClass traffic,
                                      DecisionOutcome outcome, ReasonCode reason,
                                      std::uint64_t bytes) {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return Status(StatusCode::NotFound, "unknown request");
  RequestRecord& record = iterator->second;
  record.decisions += 1;
  AttemptRecord* entry = record.find_attempt(attempt);
  if (entry != nullptr) entry->decisions += 1;
  const std::size_t index = static_cast<std::size_t>(traffic);
  ClassAccounting* class_entry =
      index < kTrafficClassCount ? &accounting_.per_class[index] : nullptr;
  switch (outcome) {
    case DecisionOutcome::Allowed:
      if (class_entry != nullptr) {
        class_entry->allowed += 1;
        class_entry->bytes_allowed += bytes;
      }
      break;
    case DecisionOutcome::Degraded:
      if (class_entry != nullptr) {
        class_entry->degraded += 1;
        class_entry->bytes_allowed += bytes;
      }
      break;
    case DecisionOutcome::Deferred:
      record.defers += 1;
      if (entry != nullptr) entry->defers += 1;
      if (class_entry != nullptr) {
        class_entry->deferred += 1;
        class_entry->bytes_deferred += bytes;
      }
      break;
    case DecisionOutcome::Rejected:
      record.rejections += 1;
      if (entry != nullptr) entry->rejections += 1;
      if (class_entry != nullptr) {
        class_entry->rejected += 1;
        class_entry->bytes_rejected += bytes;
      }
      break;
    case DecisionOutcome::Unknown:
    case DecisionOutcome::Count:
      break;
  }
  if (reason == ReasonCode::AdmittedStarvationGuard && class_entry != nullptr) {
    class_entry->starvation_admissions += 1;
  }
  return Status::success();
}

bool RequestLedger::contains(RequestId id) const { return requests_.find(id) != requests_.end(); }

bool RequestLedger::is_cancelled(RequestId id) const {
  const auto iterator = requests_.find(id);
  return iterator != requests_.end() &&
         iterator->second.lifecycle == RequestLifecycle::Cancelled;
}

std::optional<RequestRecord> RequestLedger::snapshot_request(RequestId id) const {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return std::nullopt;
  return iterator->second;
}

std::optional<RequestAccounting> RequestLedger::snapshot_accounting(RequestId id) const {
  const auto iterator = requests_.find(id);
  if (iterator == requests_.end()) return std::nullopt;
  const RequestRecord& record = iterator->second;
  RequestAccounting out;
  out.request = record.id;
  out.bytes_in = record.bytes_in;
  out.bytes_out = record.bytes_out;
  out.decisions = record.decisions;
  out.defers = record.defers;
  out.rejections = record.rejections;
  out.active_flows = record.active_flows;
  out.closed = is_terminal(record.lifecycle) && record.active_flows == 0;
  return out;
}

Status RequestLedger::verify_closure() const {
  std::array<std::uint32_t, kTrafficClassCount> per_class{};
  std::uint32_t total = 0;
  for (const auto& entry : flows_) {
    const std::size_t index = static_cast<std::size_t>(entry.second.traffic);
    if (index < kTrafficClassCount) per_class[index] += 1;
    total += 1;
  }
  if (total != accounting_.active_flows_total) {
    return Status(StatusCode::Internal, "active flow counter diverged from live flow records");
  }
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    if (per_class[index] != accounting_.per_class[index].active_flows) {
      return Status(StatusCode::Internal,
                    "per-class active flow counter diverged from live flow records");
    }
  }
  for (const auto& entry : requests_) {
    const RequestRecord& record = entry.second;
    if (is_terminal(record.lifecycle) && record.active_flows != 0) {
      return Status(StatusCode::Internal, "terminal request retains active flows");
    }
    if (!is_terminal(record.lifecycle) && record.requires_revalidation &&
        record.active_flows != 0) {
      return Status(StatusCode::Internal, "interrupted request retains active flows");
    }
  }
  return Status::success();
}

Status RequestLedger::restore_record(const RequestRecord& record) {
  if (record.id.is_nil()) {
    return Status(StatusCode::InvalidArgument, "restored record has nil identity");
  }
  if (record.attempt_count > kMaxAttemptsPerRequest) {
    return Status(StatusCode::MalformedInput, "restored record exceeds attempt ceiling");
  }
  if (requests_.find(record.id) != requests_.end()) {
    return Status(StatusCode::AlreadyExists, "restored record duplicates a live request");
  }
  RequestRecord restored = record;
  for (std::uint32_t index = restored.attempt_count; index < kMaxAttemptsPerRequest; ++index) {
    restored.attempts[index] = AttemptRecord{};
  }
  if (is_terminal(restored.lifecycle)) {
    restored.accounting_closed = true;
    restored.active_flows = 0;
    terminal_order_.push_back(restored.id);
    if (restored.lifecycle == RequestLifecycle::Completed) accounting_.requests_completed += 1;
    if (restored.lifecycle == RequestLifecycle::Cancelled) accounting_.requests_cancelled += 1;
    if (restored.lifecycle == RequestLifecycle::Failed) accounting_.requests_failed += 1;
  } else {
    restored.lifecycle = RequestLifecycle::Interrupted;
    restored.requires_revalidation = true;
    restored.active_flows = 0;
    restored.accounting_closed = false;
    accounting_.requests_interrupted += 1;
  }
  accounting_.requests_registered += 1;
  requests_.emplace(restored.id, restored);
  refresh_request_counts();
  evict_if_needed();
  return Status::success();
}

std::vector<RequestRecord> RequestLedger::export_terminal_records(std::size_t max_records) const {
  std::vector<RequestRecord> out;
  out.reserve(std::min(max_records, terminal_order_.size()));
  for (auto iterator = terminal_order_.rbegin();
       iterator != terminal_order_.rend() && out.size() < max_records; ++iterator) {
    const auto found = requests_.find(*iterator);
    if (found != requests_.end()) out.push_back(found->second);
  }
  return out;
}

std::vector<RequestRecord> RequestLedger::export_live_records() const {
  std::vector<RequestRecord> out;
  for (const auto& entry : requests_) {
    if (!is_terminal(entry.second.lifecycle)) out.push_back(entry.second);
  }
  return out;
}

void RequestLedger::refresh_request_counts() {
  accounting_.retained_requests = terminal_order_.size();
  accounting_.live_requests =
      requests_.size() > terminal_order_.size() ? requests_.size() - terminal_order_.size() : 0;
}

void RequestLedger::evict_if_needed() {
  while (terminal_order_.size() > limits_.max_retained_terminal_requests) {
    const RequestId victim = terminal_order_.front();
    terminal_order_.pop_front();
    const auto iterator = requests_.find(victim);
    if (iterator == requests_.end()) continue;
    if (!is_terminal(iterator->second.lifecycle)) continue;
    requests_.erase(iterator);
    accounting_.requests_evicted += 1;
  }
  refresh_request_counts();
}

}  // namespace itf
