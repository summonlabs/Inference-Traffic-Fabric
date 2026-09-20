// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/coordinator.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "itf/persistence.hpp"
#include "itf/serialize.hpp"

namespace itf {
namespace {

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
  value ^= value >> 30U;
  value *= 0xBF58476D1CE4E5B9ULL;
  value ^= value >> 27U;
  value *= 0x94D049BB133111EBULL;
  value ^= value >> 31U;
  return value;
}

template <class Tag>
[[nodiscard]] Id128<Tag> make_random_id(std::mt19937_64& random) noexcept {
  std::uint64_t high = random();
  std::uint64_t low = random();
  if (high == 0 && low == 0) low = 1;
  return Id128<Tag>(high, low);
}

[[nodiscard]] bool is_transfer_class(TrafficClass value) noexcept {
  return value == TrafficClass::KvTransfer || value == TrafficClass::StateTransfer ||
         value == TrafficClass::ModelTransfer || value == TrafficClass::AdapterTransfer;
}

[[nodiscard]] IntegrityClass max_integrity(IntegrityClass a, IntegrityClass b) noexcept {
  return static_cast<std::uint16_t>(a) >= static_cast<std::uint16_t>(b) ? a : b;
}

}  // namespace

std::string_view to_string(TransferState value) noexcept {
  switch (value) {
    case TransferState::Unknown: return "UNKNOWN";
    case TransferState::Registered: return "REGISTERED";
    case TransferState::Authorized: return "AUTHORIZED";
    case TransferState::InProgress: return "IN_PROGRESS";
    case TransferState::Completed: return "COMPLETED";
    case TransferState::Refused: return "REFUSED";
    case TransferState::Abandoned: return "ABANDONED";
    case TransferState::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string CoordinatorStatus::to_string() const {
  std::string out;
  out.reserve(256);
  out.append("epoch=");
  out.append(std::to_string(epoch.value()));
  out.append(" incarnation=");
  out.append(incarnation.to_string());
  out.append(" policy=");
  out.append(std::to_string(policy_generation.value()));
  out.append(" topology=");
  out.append(std::to_string(topology_generation.value()));
  out.append(" topology_state=");
  out.append(itf::to_string(topology_state));
  out.append(" topology_provenance=");
  out.append(itf::to_string(topology_provenance));
  out.append(" live_requests=");
  out.append(std::to_string(live_requests));
  out.append(" transfers=");
  out.append(std::to_string(transfers));
  out.append(" active_flows=");
  out.append(std::to_string(active_flows));
  out.append(" authority_sequence=");
  out.append(std::to_string(authority_sequence));
  return out;
}

Coordinator::Coordinator(CoordinatorConfig config, Clock* clock)
    : config_(std::move(config)),
      clock_(clock != nullptr ? clock : &steady_clock_singleton()),
      policy_(config_.policy),
      ledger_(config_.limits.ledger),
      random_(config_.boot_seed != 0 ? config_.boot_seed : std::random_device{}()) {
  if (policy_.generation().is_unset()) policy_.set_generation(PolicyGeneration::from(1));
}

AuthorityStamp Coordinator::mint_stamp_locked() {
  authority_sequence_ = authority_sequence_.is_unset() ? AuthoritySequence::from(1)
                                                       : authority_sequence_.next();
  AuthorityStamp stamp;
  stamp.epoch = epoch_;
  stamp.incarnation = incarnation_;
  stamp.policy = policy_.generation();
  stamp.sequence = authority_sequence_;
  return stamp;
}

Result<CoordinatorEpoch> Coordinator::boot_fresh() {
  std::lock_guard<std::mutex> guard(mutex_);
  epoch_ = CoordinatorEpoch::from(1);
  clear_state_locked();
  boot_count_ += 1;
  reseed_boot_identity_locked();
  authority_sequence_ = AuthoritySequence::from(1);
  policy_ = config_.policy;
  if (policy_.generation().is_unset()) policy_.set_generation(PolicyGeneration::from(1));
  return Result<CoordinatorEpoch>::success(epoch_);
}

void Coordinator::clear_state_locked() {
  topology_present_ = false;
  models_.clear();
  states_.clear();
  slo_contracts_.clear();
  route_decisions_.clear();
  transfers_.clear();
  flows_.clear();
  flows_by_request_.clear();
  flows_by_transfer_.clear();
  transfers_by_request_.clear();
  defers_.clear();
  completions_.clear();
  completion_order_.clear();
  ledger_.clear();
  for (ClassAccounting& entry : transfer_class_accounting_) entry = ClassAccounting{};
  bulk_in_flight_bytes_ = 0;
  streaming_deferrals_ = 0;
  transfers_started_ = 0;
  transfers_completed_ = 0;
  transfers_refused_ = 0;
  next_flow_sequence_ = 0;
}

Result<CoordinatorEpoch> Coordinator::boot_from(const PersistedCoordinatorState& restored) {
  std::lock_guard<std::mutex> guard(mutex_);
  clear_state_locked();
  // Recovery is all-or-nothing: the document is validated and applied before
  // any new authority is minted, and a refused document leaves nothing behind.
  const Status restored_status = restore_state(restored);
  if (!restored_status.ok()) {
    clear_state_locked();
    return Result<CoordinatorEpoch>::failure(restored_status);
  }
  boot_count_ = (restored.boot_count > boot_count_ ? restored.boot_count : boot_count_) + 1;
  epoch_ = restored.epoch.is_unset() ? CoordinatorEpoch::from(1) : restored.epoch.next();
  reseed_boot_identity_locked();
  authority_sequence_ = AuthoritySequence::from(1);
  return Result<CoordinatorEpoch>::success(epoch_);
}

void Coordinator::reseed_boot_identity_locked() {
  // A configured boot seed keeps runs reproducible, but two boots of the same
  // process or host must never share an incarnation: the boot counter is mixed
  // into the seed so that consecutive boots always differ.
  if (config_.boot_seed != 0) {
    random_.seed(config_.boot_seed + boot_count_ * 0x9E3779B97F4A7C15ULL);
  }
  incarnation_ = make_random_id<BootIdTag>(random_);
}

Status Coordinator::validate_restored_state_locked(
    const PersistedCoordinatorState& state) const {
  if (state.format_version != kSnapshotFormatVersion) {
    return Status(StatusCode::UnsupportedVersion, "restored snapshot format is not supported");
  }
  if (state.policy.validate() != PolicyVerdict::Valid) {
    return Status(StatusCode::PolicyInvalid, "restored policy document is not valid");
  }
  if (state.policy.generation().is_unset()) {
    return Status(StatusCode::MalformedInput, "restored policy document has no generation");
  }
  if (state.models.size() > config_.limits.max_model_records) {
    return Status(StatusCode::BoundsExceeded, "restored model registry exceeds its limit");
  }
  for (const ModelState& entry : state.models) {
    if (!entry.is_valid()) {
      return Status(StatusCode::MalformedInput, "restored model record is invalid");
    }
  }
  if (state.states.size() > config_.limits.max_state_records) {
    return Status(StatusCode::BoundsExceeded, "restored state registry exceeds its limit");
  }
  for (const StateGenerationRecord& entry : state.states) {
    if (!entry.is_valid()) {
      return Status(StatusCode::MalformedInput, "restored state record is invalid");
    }
  }
  if (state.topology_present && !state.topology.is_authoritative()) {
    return Status(StatusCode::MalformedInput, "restored topology evidence is invalid");
  }
  std::vector<RequestId> seen;
  seen.reserve(state.terminal_requests.size() + state.interrupted_requests.size());
  const auto inspect = [&seen](const std::vector<RequestRecord>& records) -> Status {
    for (const RequestRecord& record : records) {
      if (record.id.is_nil()) {
        return Status(StatusCode::MalformedInput, "restored request record has no identity");
      }
      if (record.attempt_count > kMaxAttemptsPerRequest) {
        return Status(StatusCode::MalformedInput,
                      "restored request record exceeds the attempt ceiling");
      }
      if (std::find(seen.begin(), seen.end(), record.id) != seen.end()) {
        return Status(StatusCode::AlreadyExists, "restored request history repeats an identity");
      }
      seen.push_back(record.id);
    }
    return Status::success();
  };
  const Status terminal_status = inspect(state.terminal_requests);
  if (!terminal_status.ok()) return terminal_status;
  return inspect(state.interrupted_requests);
}

Status Coordinator::restore_state(const PersistedCoordinatorState& state) {
  const Status validated = validate_restored_state_locked(state);
  if (!validated.ok()) return validated;
  // Policy is durable configuration, but it is not current authority: the
  // generation advances so that decisions bound to the previous document are
  // fenced deterministically.
  policy_ = state.policy;
  policy_.set_generation(state.policy.generation().is_unset()
                             ? PolicyGeneration::from(1)
                             : state.policy.generation().next());

  for (const ModelState& entry : state.models) {
    ModelState restored = entry;
    restored.requires_revalidation = true;
    models_.emplace(restored.model_hash, restored);
  }
  for (const StateGenerationRecord& entry : state.states) {
    StateGenerationRecord restored = entry;
    restored.requires_revalidation = true;
    states_.emplace(restored.state_hash, restored);
  }
  if (state.topology_present) {
    topology_ = state.topology;
    topology_present_ = true;
  }
  for (const RequestRecord& record : state.terminal_requests) {
    const Status status = ledger_.restore_record(record);
    if (!status.ok()) return status;
  }
  for (const RequestRecord& record : state.interrupted_requests) {
    const Status status = ledger_.restore_record(record);
    if (!status.ok()) return status;
  }
  return Status::success();
}

AuthorityStamp Coordinator::authority() const {
  std::lock_guard<std::mutex> guard(mutex_);
  AuthorityStamp stamp;
  stamp.epoch = epoch_;
  stamp.incarnation = incarnation_;
  stamp.policy = policy_.generation();
  stamp.sequence = authority_sequence_;
  return stamp;
}

CoordinatorEpoch Coordinator::epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return epoch_;
}

BootId Coordinator::incarnation() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return incarnation_;
}

CoordinatorStatus Coordinator::status() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const AccountingSnapshot snapshot = ledger_.accounting();
  CoordinatorStatus status;
  status.epoch = epoch_;
  status.incarnation = incarnation_;
  status.policy_generation = policy_.generation();
  status.topology_generation = topology_.generation;
  status.topology_state = topology_state_locked(clock_->now_nanos());
  status.topology_provenance =
      topology_present_ ? topology_.provenance : EvidenceProvenance::Unknown;
  status.transfers = transfers_.size();
  status.live_requests = snapshot.live_requests;
  status.active_flows = accounting_unlocked_active_flows(snapshot);
  status.authority_sequence = authority_sequence_.value();
  return status;
}

PolicyConfig Coordinator::policy() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return policy_;
}

Status Coordinator::apply_policy(PolicyConfig policy) {
  const PolicyVerdict verdict = policy.validate();
  if (verdict != PolicyVerdict::Valid) {
    return Status(StatusCode::PolicyInvalid, std::string(to_string(verdict)));
  }
  std::lock_guard<std::mutex> guard(mutex_);
  policy_ = std::move(policy);
  policy_.set_generation(policy_.generation().is_unset() ? PolicyGeneration::from(1)
                                                         : policy_.generation().next());
  return Status::success();
}

Result<TopologyGeneration> Coordinator::publish_topology(TopologyEvidence evidence) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (evidence.provenance == EvidenceProvenance::Unknown) {
    return Result<TopologyGeneration>::failure(StatusCode::InvalidArgument,
                                               "topology evidence requires a provenance label");
  }
  if (evidence.node_count == 0) {
    return Result<TopologyGeneration>::failure(StatusCode::InvalidArgument,
                                               "topology evidence requires at least one node");
  }
  evidence.generation = topology_present_ ? topology_.generation.next()
                                          : TopologyGeneration::from(1);
  evidence.observed_boot = incarnation_;
  evidence.observed_at_nanos = clock_->now_nanos();
  if (evidence.max_age_nanos == 0) evidence.max_age_nanos = config_.default_evidence_max_age_nanos;
  if (!evidence.is_authoritative()) {
    return Result<TopologyGeneration>::failure(StatusCode::InvalidArgument,
                                               "topology evidence is internally inconsistent");
  }
  topology_ = evidence;
  topology_present_ = true;
  return Result<TopologyGeneration>::success(evidence.generation);
}

EvidenceState Coordinator::topology_state_locked(std::int64_t now) const {
  if (!topology_present_) return EvidenceState::Unknown;
  if (topology_.observed_boot != incarnation_) return EvidenceState::RevalidationRequired;
  if (topology_.max_age_nanos > 0 && now > topology_.observed_at_nanos &&
      static_cast<std::uint64_t>(now - topology_.observed_at_nanos) > topology_.max_age_nanos) {
    return EvidenceState::Stale;
  }
  return EvidenceState::Current;
}

EvidenceState Coordinator::topology_state() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return topology_state_locked(clock_->now_nanos());
}

std::optional<TopologyEvidence> Coordinator::topology() const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!topology_present_) return std::nullopt;
  TopologyEvidence evidence = topology_;
  if (evidence.observed_boot != incarnation_) evidence.provenance = topology_.provenance;
  return evidence;
}

Result<SloContract> Coordinator::register_slo_contract(RequestId request,
                                                       std::uint64_t tail_latency_budget_nanos,
                                                       std::uint64_t registration_deadline_nanos,
                                                       std::uint32_t min_stream_share_per_mille) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (request.is_nil()) {
    return Result<SloContract>::failure(StatusCode::InvalidArgument, "nil request id");
  }
  if (!ledger_.contains(request)) {
    return Result<SloContract>::failure(StatusCode::NotFound,
                                        "SLO contract requires a registered request");
  }
  if (tail_latency_budget_nanos == 0) {
    return Result<SloContract>::failure(StatusCode::InvalidArgument,
                                        "SLO contract requires a non-zero tail budget");
  }
  auto existing = slo_contracts_.find(request);
  if (existing == slo_contracts_.end() && slo_contracts_.size() >= config_.limits.max_slo_contracts) {
    return Result<SloContract>::failure(StatusCode::CapacityExhausted,
                                        "SLO contract registry is full");
  }
  SloContract contract;
  contract.generation = existing == slo_contracts_.end() ? SloContractGeneration::from(1)
                                                         : existing->second.generation.next();
  contract.request = request;
  contract.tail_latency_budget_nanos = tail_latency_budget_nanos;
  contract.registration_deadline_nanos = registration_deadline_nanos;
  contract.min_stream_share_per_mille =
      min_stream_share_per_mille > 1000U ? 1000U : min_stream_share_per_mille;
  if (!contract.is_valid()) {
    return Result<SloContract>::failure(StatusCode::InvalidArgument, "SLO contract is invalid");
  }
  slo_contracts_[request] = contract;
  return Result<SloContract>::success(contract);
}

Result<RouteDecision> Coordinator::issue_route_decision(RequestId request, AttemptId attempt,
                                                        std::uint32_t target_node, bool lateral,
                                                        bool disaggregated_handoff) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto record = ledger_.snapshot_request(request);
  if (!record.has_value()) {
    return Result<RouteDecision>::failure(StatusCode::NotFound, "unknown request");
  }
  if (record->find_attempt(attempt) == nullptr) {
    return Result<RouteDecision>::failure(StatusCode::NotFound, "unknown attempt");
  }
  auto existing = route_decisions_.find(attempt);
  if (existing == route_decisions_.end() &&
      route_decisions_.size() >= config_.limits.max_route_decisions) {
    return Result<RouteDecision>::failure(StatusCode::CapacityExhausted,
                                          "route decision registry is full");
  }
  RouteDecision decision;
  decision.generation = existing == route_decisions_.end() ? RouteDecisionGeneration::from(1)
                                                           : existing->second.generation.next();
  decision.request = request;
  decision.attempt = attempt;
  decision.target_node = target_node;
  decision.lateral = lateral;
  decision.disaggregated_handoff = disaggregated_handoff;
  if (!decision.is_valid()) {
    return Result<RouteDecision>::failure(StatusCode::InvalidArgument, "route decision is invalid");
  }
  route_decisions_[attempt] = decision;
  return Result<RouteDecision>::success(decision);
}

Result<ModelGeneration> Coordinator::set_model_generation(
    std::uint64_t model_hash, std::optional<ModelGeneration> expected_current) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (model_hash == 0) {
    return Result<ModelGeneration>::failure(StatusCode::InvalidArgument, "nil model hash");
  }
  auto existing = models_.find(model_hash);
  if (existing == models_.end()) {
    if (expected_current.has_value()) {
      return Result<ModelGeneration>::failure(StatusCode::Conflict,
                                              "model generation expectation does not exist");
    }
    if (models_.size() >= config_.limits.max_model_records) {
      return Result<ModelGeneration>::failure(StatusCode::CapacityExhausted,
                                              "model registry is full");
    }
  } else if (existing->second.requires_revalidation) {
    return Result<ModelGeneration>::failure(
        StatusCode::RevalidationRequired,
        "model generation survived a restart and must be revalidated first");
  } else if (expected_current.has_value() && existing->second.generation != *expected_current) {
    return Result<ModelGeneration>::failure(StatusCode::Conflict,
                                            "model generation expectation did not match");
  }
  ModelState state;
  state.model_hash = model_hash;
  state.generation = existing == models_.end() ? ModelGeneration::from(1)
                                               : existing->second.generation.next();
  state.requires_revalidation = false;
  models_[model_hash] = state;
  return Result<ModelGeneration>::success(state.generation);
}

Result<ModelGeneration> Coordinator::revalidate_model_generation(std::uint64_t model_hash) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto existing = models_.find(model_hash);
  if (existing == models_.end()) {
    return Result<ModelGeneration>::failure(StatusCode::NotFound, "unknown model");
  }
  existing->second.requires_revalidation = false;
  return Result<ModelGeneration>::success(existing->second.generation);
}

Result<StateGeneration> Coordinator::advance_state_generation(
    std::uint64_t state_hash, std::optional<StateGeneration> expected_current) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (state_hash == 0) {
    return Result<StateGeneration>::failure(StatusCode::InvalidArgument, "nil state hash");
  }
  auto existing = states_.find(state_hash);
  if (existing == states_.end()) {
    if (expected_current.has_value()) {
      return Result<StateGeneration>::failure(StatusCode::Conflict,
                                              "state generation expectation does not exist");
    }
    if (states_.size() >= config_.limits.max_state_records) {
      return Result<StateGeneration>::failure(StatusCode::CapacityExhausted,
                                              "state registry is full");
    }
  } else if (existing->second.requires_revalidation) {
    return Result<StateGeneration>::failure(
        StatusCode::RevalidationRequired,
        "state generation survived a restart and must be revalidated first");
  } else if (expected_current.has_value() && existing->second.generation != *expected_current) {
    return Result<StateGeneration>::failure(StatusCode::Conflict,
                                            "state generation expectation did not match");
  }
  StateGenerationRecord record;
  record.state_hash = state_hash;
  record.generation = existing == states_.end() ? StateGeneration::from(1)
                                                : existing->second.generation.next();
  record.requires_revalidation = false;
  states_[state_hash] = record;
  return Result<StateGeneration>::success(record.generation);
}

Result<StateGeneration> Coordinator::revalidate_state_generation(std::uint64_t state_hash) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto existing = states_.find(state_hash);
  if (existing == states_.end()) {
    return Result<StateGeneration>::failure(StatusCode::NotFound, "unknown state identity");
  }
  existing->second.requires_revalidation = false;
  return Result<StateGeneration>::success(existing->second.generation);
}

Result<ModelGeneration> Coordinator::require_model_generation_locked(
    std::uint64_t model_hash) const {
  const auto iterator = models_.find(model_hash);
  if (iterator == models_.end()) {
    return Result<ModelGeneration>::failure(StatusCode::NotFound, "unknown model generation");
  }
  if (iterator->second.requires_revalidation) {
    return Result<ModelGeneration>::failure(StatusCode::RevalidationRequired,
                                            "model generation requires revalidation");
  }
  return Result<ModelGeneration>::success(iterator->second.generation);
}

Result<StateGeneration> Coordinator::require_state_generation_locked(
    std::uint64_t state_hash) const {
  const auto iterator = states_.find(state_hash);
  if (iterator == states_.end()) {
    return Result<StateGeneration>::failure(StatusCode::NotFound, "unknown state generation");
  }
  if (iterator->second.requires_revalidation) {
    return Result<StateGeneration>::failure(StatusCode::RevalidationRequired,
                                            "state generation requires revalidation");
  }
  return Result<StateGeneration>::success(iterator->second.generation);
}

Result<ModelGeneration> Coordinator::model_generation(std::uint64_t model_hash) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return require_model_generation_locked(model_hash);
}

Result<StateGeneration> Coordinator::state_generation(std::uint64_t state_hash) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return require_state_generation_locked(state_hash);
}

Result<RequestGeneration> Coordinator::register_request(RequestId request) {
  std::lock_guard<std::mutex> guard(mutex_);
  return ledger_.register_request(request, clock_->now_nanos());
}

Result<AttemptGeneration> Coordinator::register_attempt(RequestId request, AttemptId attempt,
                                                        std::uint64_t model_hash) {
  std::lock_guard<std::mutex> guard(mutex_);
  ModelGeneration model_generation;
  if (model_hash != 0) {
    auto resolved = require_model_generation_locked(model_hash);
    if (!resolved.ok()) return Result<AttemptGeneration>::failure(resolved.status());
    model_generation = resolved.value();
  }
  return ledger_.register_attempt(request, attempt, model_hash, model_generation,
                                  RouteDecisionGeneration(), clock_->now_nanos());
}

Result<AttemptGeneration> Coordinator::revalidate_request(RequestId request, AttemptId attempt) {
  std::lock_guard<std::mutex> guard(mutex_);
  return ledger_.revalidate_request(request, attempt, clock_->now_nanos());
}

Status Coordinator::advance_stage(RequestId request, AttemptId attempt, ServingStage target) {
  std::lock_guard<std::mutex> guard(mutex_);
  return ledger_.advance_stage(request, attempt, target, clock_->now_nanos());
}

Status Coordinator::cancel_request(RequestId request, ReasonCode reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  const AuthoritySequence sequence = authority_sequence_.is_unset()
                                         ? AuthoritySequence::from(1)
                                         : authority_sequence_.next();
  authority_sequence_ = sequence;
  const Status status = ledger_.cancel_request(request, reason, sequence, clock_->now_nanos());
  if (status.ok()) release_request_flows_locked(request);
  return status;
}

Result<AttemptGeneration> Coordinator::fail_attempt(RequestId request, AttemptId attempt,
                                                    ReasonCode reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto outcome = ledger_.fail_attempt(request, attempt, reason, clock_->now_nanos());
  if (outcome.ok()) release_request_flows_locked(request);
  return outcome;
}

Result<AttemptGeneration> Coordinator::replace_attempt(RequestId request, AttemptId superseded,
                                                       AttemptId replacement,
                                                       std::uint64_t model_hash) {
  std::lock_guard<std::mutex> guard(mutex_);
  ModelGeneration model_generation;
  if (model_hash != 0) {
    auto resolved = require_model_generation_locked(model_hash);
    if (!resolved.ok()) return Result<AttemptGeneration>::failure(resolved.status());
    model_generation = resolved.value();
  }
  auto outcome =
      ledger_.supersede_attempt(request, superseded, replacement, clock_->now_nanos());
  if (!outcome.ok()) return outcome;
  release_request_flows_locked(request);
  (void)ledger_.set_attempt_model(request, replacement, model_hash, model_generation);
  route_decisions_.erase(superseded);
  return outcome;
}

std::uint64_t Coordinator::defer_key(const TrafficSubject& subject,
                                     TrafficClass traffic) const noexcept {
  std::uint64_t key = mix64(static_cast<std::uint64_t>(traffic) * 0x9E3779B97F4A7C15ULL + 0x165667B19E3779F9ULL);
  if (subject.kind == SubjectKind::Serving) {
    key ^= mix64(subject.request.high() ^ 0xD6E8FEB86659FD93ULL);
    key ^= mix64(subject.request.low() + 0x9E3779B97F4A7C15ULL);
    key ^= mix64(subject.attempt.high() + 0xBF58476D1CE4E5B9ULL);
    key ^= mix64(subject.attempt.low() ^ 0x94D049BB133111EBULL);
  } else {
    key ^= mix64(subject.transfer.high() + 0x2545F4914F6CDD1DULL);
    key ^= mix64(subject.transfer.low() ^ 0x9E3779B97F4A7C15ULL);
  }
  return key;
}

bool Coordinator::defer_matches(const DeferRecord& record,
                                const TrafficSubject& subject) const noexcept {
  if (record.kind != subject.kind) return false;
  if (subject.kind == SubjectKind::Serving) {
    return record.request == subject.request && record.attempt == subject.attempt;
  }
  return record.transfer == subject.transfer;
}

void Coordinator::evict_defer_records_locked() {
  if (defers_.empty()) return;
  // Deferral state is a scheduling hint with a fixed ceiling. When the ceiling
  // is reached one entry is retired deterministically; no input can grow it.
  auto victim = defers_.begin();
  if (victim->second.starvation_class && streaming_deferrals_ > 0) streaming_deferrals_ -= 1;
  defers_.erase(victim);
}

void Coordinator::note_defer_locked(const TrafficSubject& subject, TrafficClass traffic,
                                    std::int64_t now, std::uint32_t& count_out) {
  const bool starvation_class = policy_.rule(traffic).starvation_protected;
  const std::uint64_t key = defer_key(subject, traffic);
  auto iterator = defers_.find(key);
  if (iterator != defers_.end() && !defer_matches(iterator->second, subject)) {
    if (iterator->second.starvation_class && streaming_deferrals_ > 0) streaming_deferrals_ -= 1;
    defers_.erase(iterator);
    iterator = defers_.end();
  }
  if (iterator == defers_.end()) {
    if (defers_.size() >= config_.limits.max_defer_records) evict_defer_records_locked();
    DeferRecord record;
    record.kind = subject.kind;
    record.request = subject.request;
    record.attempt = subject.attempt;
    record.transfer = subject.transfer;
    record.traffic = traffic;
    record.count = 1;
    record.first_defer_nanos = now;
    record.last_defer_nanos = now;
    record.starvation_class = starvation_class;
    defers_.emplace(key, record);
    if (starvation_class) streaming_deferrals_ += 1;
    count_out = 1;
    return;
  }
  iterator->second.count += 1;
  iterator->second.last_defer_nanos = now;
  count_out = iterator->second.count;
}

void Coordinator::clear_defer_locked(const TrafficSubject& subject, TrafficClass traffic) {
  const std::uint64_t key = defer_key(subject, traffic);
  auto iterator = defers_.find(key);
  if (iterator == defers_.end()) return;
  if (!defer_matches(iterator->second, subject)) return;
  if (iterator->second.starvation_class && streaming_deferrals_ > 0) streaming_deferrals_ -= 1;
  defers_.erase(iterator);
}

bool Coordinator::streaming_pressure_locked() const { return streaming_deferrals_ > 0; }

void Coordinator::account_decision_locked(TrafficClass traffic, DecisionOutcome outcome,
                                          std::uint64_t bytes) {
  const std::size_t index = static_cast<std::size_t>(traffic);
  if (index >= kTrafficClassCount) return;
  ClassAccounting& entry = transfer_class_accounting_[index];
  switch (outcome) {
    case DecisionOutcome::Allowed:
      entry.allowed += 1;
      entry.bytes_allowed += bytes;
      break;
    case DecisionOutcome::Degraded:
      entry.degraded += 1;
      entry.bytes_allowed += bytes;
      break;
    case DecisionOutcome::Deferred:
      entry.deferred += 1;
      entry.bytes_deferred += bytes;
      break;
    case DecisionOutcome::Rejected:
      entry.rejected += 1;
      entry.bytes_rejected += bytes;
      break;
    case DecisionOutcome::Unknown:
    case DecisionOutcome::Count:
      break;
  }
}

Decision Coordinator::reject_locked(const TrafficRequest& request, const TrafficSubject& subject,
                                    ReasonCode reason) {
  Decision decision;
  decision.outcome = DecisionOutcome::Rejected;
  decision.reason = reason;
  decision.subject = subject;
  decision.binding = request.binding;
  decision.authority = mint_stamp_locked();
  return decision;
}

Decision Coordinator::evaluate_locked(const TrafficRequest& request) {
  const std::int64_t now = clock_->now_nanos();
  const TrafficSubject subject = request.subject.value_or(TrafficSubject{});
  ExplanationLog log;
  log.add(ExplanationRule::AuthorityBinding, epoch_.value(),
          static_cast<std::int64_t>(policy_.generation().value()));

  auto build = [&](DecisionOutcome outcome, ReasonCode reason, TrafficClass traffic) -> Decision {
    Decision decision;
    decision.outcome = outcome;
    decision.reason = reason;
    decision.subject = subject;
    decision.binding = request.binding;
    decision.authority = mint_stamp_locked();
    std::uint32_t limit = policy_.max_explanation_steps();
    if (limit > kMaxExplanationSteps) limit = static_cast<std::uint32_t>(kMaxExplanationSteps);
    std::uint32_t count = log.count() < limit ? log.count() : limit;
    decision.explanation_count = count;
    for (std::uint32_t index = 0; index < count; ++index) {
      decision.explanation[index] = log[index];
    }
    if (subject.kind == SubjectKind::Serving && ledger_.contains(subject.request)) {
      (void)ledger_.record_decision(subject.request, subject.attempt, traffic, outcome, reason,
                                    request.payload_bytes);
    } else {
      account_decision_locked(traffic, outcome, request.payload_bytes);
    }
    return decision;
  };

  if (!request.structurally_valid() || !request.subject.has_value()) {
    log.add(ExplanationRule::RequestLookup, 0, 0);
    return build(DecisionOutcome::Rejected, ReasonCode::RejectedSubjectKindMismatch,
                 request.declared_class);
  }
  if (request.binding.coordinator_epoch.has_value() &&
      *request.binding.coordinator_epoch != epoch_) {
    log.add(ExplanationRule::AuthorityBinding,
            static_cast<std::int64_t>(request.binding.coordinator_epoch->value()),
            static_cast<std::int64_t>(epoch_.value()));
    return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleEpoch, request.declared_class);
  }
  if (request.binding.policy.has_value() && *request.binding.policy != policy_.generation()) {
    log.add(ExplanationRule::AuthorityBinding,
            static_cast<std::int64_t>(request.binding.policy->value()),
            static_cast<std::int64_t>(policy_.generation().value()));
    return build(DecisionOutcome::Rejected, ReasonCode::RejectedStalePolicyGeneration,
                 request.declared_class);
  }

  bool serving = false;
  bool has_transfer = false;
  RequestRecord record;
  TransferRecord transfer;
  ServingStage stage = ServingStage::Unknown;
  const AttemptRecord* attempt = nullptr;
  ReasonCode degrade_reason = ReasonCode::Unknown;
  bool degraded = false;

  if (subject.kind == SubjectKind::Serving) {
    serving = true;
    auto snapshot = ledger_.snapshot_request(subject.request);
    if (!snapshot.has_value()) {
      log.add(ExplanationRule::RequestLookup, 0, 0);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedUnknownRequest,
                   request.declared_class);
    }
    record = std::move(snapshot).value();
    log.add(ExplanationRule::RequestLookup,
            static_cast<std::int64_t>(record.generation.value()),
            static_cast<std::int64_t>(record.lifecycle));
    if (record.lifecycle == RequestLifecycle::Cancelled) {
      log.add(ExplanationRule::TerminalState, static_cast<std::int64_t>(record.lifecycle), 1);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedRequestCancelled,
                   request.declared_class);
    }
    if (record.requires_revalidation) {
      log.add(ExplanationRule::RequestLookup, 0, 1);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedRevalidationRequired,
                   request.declared_class);
    }
    if (is_terminal(record.lifecycle)) {
      log.add(ExplanationRule::TerminalState, static_cast<std::int64_t>(record.lifecycle), 0);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedRequestTerminal,
                   request.declared_class);
    }
    attempt = record.find_attempt(subject.attempt);
    if (attempt == nullptr) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedUnknownAttempt,
                   request.declared_class);
    }
    if (!record.is_current_attempt(subject.attempt)) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleAttempt,
                   request.declared_class);
    }
    if (request.binding.request_generation.has_value() &&
        *request.binding.request_generation != record.generation) {
      log.add(ExplanationRule::RequestLookup,
              static_cast<std::int64_t>(request.binding.request_generation->value()),
              static_cast<std::int64_t>(record.generation.value()));
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleRequestGeneration,
                   request.declared_class);
    }
    stage = attempt->stage;
    log.add(ExplanationRule::StageResolution, static_cast<std::int64_t>(stage),
            static_cast<std::int64_t>(request.declared_stage));
  } else if (subject.kind == SubjectKind::Transfer) {
    auto found = transfers_.find(subject.transfer);
    if (found == transfers_.end()) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedUnknownTransfer,
                   request.declared_class);
    }
    transfer = found->second;
    has_transfer = true;
    log.add(ExplanationRule::TransferBinding, static_cast<std::int64_t>(transfer.state), 0);
    if (transfer.state == TransferState::Completed || transfer.state == TransferState::Refused ||
        transfer.state == TransferState::Abandoned) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedTransferAlreadyTerminal,
                   transfer.declared_class);
    }
    if (!transfer.request.is_nil() && ledger_.is_cancelled(transfer.request)) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedRequestCancelled,
                   transfer.declared_class);
    }
    if (transfer.model_hash != 0) {
      auto model = require_model_generation_locked(transfer.model_hash);
      if (!model.ok()) {
        return build(DecisionOutcome::Rejected,
                     model.code() == StatusCode::RevalidationRequired
                         ? ReasonCode::RejectedRevalidationRequired
                         : ReasonCode::RejectedStaleModelGeneration,
                     transfer.declared_class);
      }
      if (transfer.model_generation != model.value()) {
        log.add(ExplanationRule::ModelGeneration,
                static_cast<std::int64_t>(transfer.model_generation.value()),
                static_cast<std::int64_t>(model.value().value()));
        return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleModelGeneration,
                     transfer.declared_class);
      }
    }
    auto state = require_state_generation_locked(transfer.state_hash);
    if (!state.ok()) {
      return build(DecisionOutcome::Rejected,
                   state.code() == StatusCode::RevalidationRequired
                       ? ReasonCode::RejectedRevalidationRequired
                       : ReasonCode::RejectedStaleStateGeneration,
                   transfer.declared_class);
    }
    if (transfer.state_generation != state.value()) {
      log.add(ExplanationRule::StateGeneration,
              static_cast<std::int64_t>(transfer.state_generation.value()),
              static_cast<std::int64_t>(state.value().value()));
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleStateGeneration,
                   transfer.declared_class);
    }
    if (request.binding.model_generation.has_value() &&
        *request.binding.model_generation != transfer.model_generation) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleModelGeneration,
                   transfer.declared_class);
    }
    if (request.binding.state_generation.has_value() &&
        *request.binding.state_generation != transfer.state_generation) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleStateGeneration,
                   transfer.declared_class);
    }
  } else {
    return build(DecisionOutcome::Rejected, ReasonCode::RejectedSubjectKindMismatch,
                 request.declared_class);
  }

  TrafficClass effective = request.declared_class;
  if (serving) {
    if (!is_serving_active(stage)) {
      if (stage != ServingStage::Unknown) {
        log.add(ExplanationRule::TerminalState, static_cast<std::int64_t>(stage), 0);
        return build(DecisionOutcome::Rejected, ReasonCode::RejectedRequestTerminal,
                     request.declared_class);
      }
      if (!policy_.allow_unknown_stage()) {
        log.add(ExplanationRule::StageResolution, 0, 0);
        return build(DecisionOutcome::Rejected, ReasonCode::RejectedUnknownStage,
                     request.declared_class);
      }
      log.add(ExplanationRule::StageResolution, 0, 2);
      effective = TrafficClass::Unknown;
      degraded = true;
      degrade_reason = ReasonCode::AdmittedConservative;
    } else {
      if (request.declared_stage != ServingStage::Unknown && request.declared_stage != stage) {
        degraded = true;
        degrade_reason = ReasonCode::DegradedStageClaimOverridden;
      }
      if (effective == TrafficClass::Unknown) {
        effective = canonical_class_for_stage(stage);
        log.add(ExplanationRule::ClassClassification, static_cast<std::int64_t>(effective), 0);
      } else if (!is_class_legal_for_stage(effective, stage)) {
        const TrafficClass derived = canonical_class_for_stage(stage);
        log.add(ExplanationRule::ClassClassification, static_cast<std::int64_t>(effective),
                static_cast<std::int64_t>(derived));
        effective = derived;
        degraded = true;
        degrade_reason = ReasonCode::DegradedClassRederived;
      }
      if (effective == TrafficClass::Unknown) {
        if (!policy_.allow_unknown_traffic_class()) {
          return build(DecisionOutcome::Rejected, ReasonCode::RejectedUnknownClass,
                       request.declared_class);
        }
        degraded = true;
        degrade_reason = ReasonCode::AdmittedConservative;
      }
    }
  } else {
    if (effective == TrafficClass::Unknown) effective = transfer.declared_class;
    if (!is_transfer_class(effective)) {
      if (!policy_.allow_unknown_traffic_class()) {
        return build(DecisionOutcome::Rejected, ReasonCode::RejectedClassNotLegalForStage,
                     effective);
      }
      degraded = true;
      degrade_reason = ReasonCode::AdmittedConservative;
    } else if (effective != transfer.declared_class) {
      log.add(ExplanationRule::ClassClassification, static_cast<std::int64_t>(effective),
              static_cast<std::int64_t>(transfer.declared_class));
      effective = transfer.declared_class;
      degraded = true;
      degrade_reason = ReasonCode::DegradedClassRederived;
    }
  }

  const ClassPolicyRule& rule = policy_.rule(effective);
  if (!rule.enabled) {
    log.add(ExplanationRule::PolicyRule, static_cast<std::int64_t>(effective), 0);
    return build(DecisionOutcome::Rejected, ReasonCode::RejectedClassDisabled, effective);
  }
  log.add(ExplanationRule::PolicyRule, static_cast<std::int64_t>(rule.priority),
          static_cast<std::int64_t>(rule.reserved_weight));

  const EvidenceState evidence = topology_state_locked(now);
  if (rule.requires_current_topology) {
    if (evidence == EvidenceState::Unknown) {
      log.add(ExplanationRule::TopologyEvidence, 0, 0);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedEvidenceUnknown, effective);
    }
    if (evidence != EvidenceState::Current) {
      bool grace = false;
      if (policy_.stale_evidence_grace_nanos() > 0 && topology_present_ &&
          now > topology_.observed_at_nanos) {
        grace = static_cast<std::uint64_t>(now - topology_.observed_at_nanos) <=
                policy_.stale_evidence_grace_nanos();
      }
      if (!grace) {
        log.add(ExplanationRule::TopologyEvidence, static_cast<std::int64_t>(evidence), 0);
        return build(DecisionOutcome::Rejected,
                     evidence == EvidenceState::RevalidationRequired
                         ? ReasonCode::RejectedRevalidationRequired
                         : ReasonCode::RejectedEvidenceStale,
                     effective);
      }
      degraded = true;
      degrade_reason = ReasonCode::DegradedStaleTopology;
      log.add(ExplanationRule::TopologyEvidence, static_cast<std::int64_t>(evidence), 1);
    }
    if (request.binding.topology.has_value() &&
        *request.binding.topology != topology_.generation) {
      log.add(ExplanationRule::TopologyEvidence,
              static_cast<std::int64_t>(request.binding.topology->value()),
              static_cast<std::int64_t>(topology_.generation.value()));
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleTopologyGeneration,
                   effective);
    }
  }

  std::optional<SloContract> slo;
  if (serving) {
    auto found = slo_contracts_.find(subject.request);
    if (found != slo_contracts_.end()) slo = found->second;
  } else if (has_transfer && !transfer.request.is_nil()) {
    // A transfer inherits the SLO contract of the request that owns it; a
    // transfer with no owning request simply has no SLO obligation to meet.
    auto found = slo_contracts_.find(transfer.request);
    if (found != slo_contracts_.end()) slo = found->second;
  }
  if (rule.requires_slo_contract) {
    if (!slo.has_value()) {
      log.add(ExplanationRule::SloContract, 0, 0);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleSloContract, effective);
    }
    if (request.binding.slo_contract.has_value() &&
        *request.binding.slo_contract != slo->generation) {
      log.add(ExplanationRule::SloContract,
              static_cast<std::int64_t>(request.binding.slo_contract->value()),
              static_cast<std::int64_t>(slo->generation.value()));
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleSloContract, effective);
    }
  }

  const AttemptId route_attempt = serving ? subject.attempt : transfer.attempt;
  if (rule.requires_current_route && !route_attempt.is_nil()) {
    auto route = route_decisions_.find(route_attempt);
    if (route == route_decisions_.end()) {
      log.add(ExplanationRule::RouteDecision, 0, 0);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleRouteDecision, effective);
    }
    if (request.binding.route_decision.has_value() &&
        *request.binding.route_decision != route->second.generation) {
      log.add(ExplanationRule::RouteDecision,
              static_cast<std::int64_t>(request.binding.route_decision->value()),
              static_cast<std::int64_t>(route->second.generation.value()));
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleRouteDecision, effective);
    }
  }
  (void)route_attempt;

  if (rule.requires_model_generation && serving && attempt != nullptr) {
    if (attempt->model_hash == 0) {
      log.add(ExplanationRule::ModelGeneration, 0, 0);
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleModelGeneration, effective);
    }
    auto model = require_model_generation_locked(attempt->model_hash);
    if (!model.ok()) {
      return build(DecisionOutcome::Rejected,
                   model.code() == StatusCode::RevalidationRequired
                       ? ReasonCode::RejectedRevalidationRequired
                       : ReasonCode::RejectedStaleModelGeneration,
                   effective);
    }
    if (attempt->model_generation != model.value()) {
      log.add(ExplanationRule::ModelGeneration,
              static_cast<std::int64_t>(attempt->model_generation.value()),
              static_cast<std::int64_t>(model.value().value()));
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleModelGeneration, effective);
    }
    if (request.binding.model_generation.has_value() &&
        *request.binding.model_generation != attempt->model_generation) {
      return build(DecisionOutcome::Rejected, ReasonCode::RejectedStaleModelGeneration, effective);
    }
  }

  Instant deadline = request.deadline;
  if (!deadline.is_set() && request.deadline_after_nanos > 0) {
    const std::uint64_t bounded = request.deadline_after_nanos > 3600000000000ULL
                                      ? 3600000000000ULL
                                      : request.deadline_after_nanos;
    deadline = Instant::from_now(now, static_cast<std::int64_t>(bounded));
  }
  if (!deadline.is_set() && slo.has_value() && slo->tail_latency_budget_nanos > 0 &&
      record.registered_at_nanos > 0) {
    deadline = Instant::at(record.registered_at_nanos +
                           static_cast<std::int64_t>(slo->tail_latency_budget_nanos));
  }
  if (deadline.is_set() && deadline.expired_at(now)) {
    log.add(ExplanationRule::Deadline, deadline.nanos(), now);
    return build(DecisionOutcome::Rejected, ReasonCode::RejectedDeadlineExpired, effective);
  }
  if (rule.requires_deadline && !deadline.is_set()) {
    log.add(ExplanationRule::Deadline, 0, 1);
    degraded = true;
    degrade_reason = ReasonCode::DegradedMissingSloContract;
  }

  ReasonCode defer_reason = ReasonCode::Unknown;
  if (rule.yields_to_latency) {
    if (evidence == EvidenceState::Current && topology_present_ &&
        topology_.congestion_signalled && policy_.congestion_yields_bulk()) {
      defer_reason = ReasonCode::DeferredCongestion;
    } else if (streaming_pressure_locked()) {
      defer_reason = ReasonCode::DeferredBulkYield;
    } else if (policy_.bulk_in_flight_cap_bytes() > 0 &&
               request.payload_bytes > 0 &&
               bulk_in_flight_bytes_ + request.payload_bytes >
                   policy_.bulk_in_flight_cap_bytes()) {
      defer_reason = ReasonCode::DeferredBulkCapacity;
    }
  } else if (evidence == EvidenceState::Current && topology_present_ &&
             topology_.congestion_signalled &&
             topology_.queue_depth >= policy_.congestion_queue_depth() &&
             rule.reserved_weight == 0) {
    defer_reason = ReasonCode::DeferredCongestion;
  }

  bool admitted_by_guard = false;
  if (defer_reason != ReasonCode::Unknown) {
    std::uint32_t count = 0;
    note_defer_locked(subject, effective, now, count);
    log.add(ExplanationRule::DeferState, static_cast<std::int64_t>(count),
            static_cast<std::int64_t>(defer_reason));
    bool exhausted = false;
    if (rule.starvation_protected) {
      const auto iterator = defers_.find(defer_key(subject, effective));
      const std::int64_t first = iterator != defers_.end() ? iterator->second.first_defer_nanos : now;
      const bool count_exhausted = rule.max_defer_count > 0 && count >= rule.max_defer_count;
      const bool time_exhausted = rule.max_defer_horizon_nanos > 0 && now > first &&
                                  static_cast<std::uint64_t>(now - first) >=
                                      rule.max_defer_horizon_nanos;
      exhausted = count_exhausted || time_exhausted;
      if (exhausted) {
        log.add(ExplanationRule::StarvationGuard, static_cast<std::int64_t>(count), now - first);
        clear_defer_locked(subject, effective);
        admitted_by_guard = true;
      }
    }
    if (!admitted_by_guard) {
      Decision decision =
          build(DecisionOutcome::Deferred, defer_reason, effective);
      decision.defer_hint_nanos = policy_.defer_hint_nanos();
      decision.treatment.traffic_class = effective;
      decision.treatment.priority = rule.priority;
      decision.treatment.pacing = rule.pacing;
      decision.treatment.integrity = rule.integrity;
      decision.treatment.isolation = rule.isolation;
      decision.treatment.hold_until =
          Instant::from_now(now, static_cast<std::int64_t>(policy_.defer_hint_nanos()));
      decision.treatment.defer_count = count;
      return decision;
    }
  } else {
    clear_defer_locked(subject, effective);
  }

  TrafficTreatment treatment;
  treatment.traffic_class = effective;
  treatment.priority = rule.priority;
  treatment.pacing = rule.pacing;
  treatment.integrity = rule.integrity;
  treatment.isolation = rule.isolation;
  treatment.reserved_weight = rule.reserved_weight;
  treatment.max_burst_bytes = rule.max_burst_bytes;
  treatment.rate_limit_bytes_per_sec = rule.rate_limit_bytes_per_sec;
  if (has_transfer) treatment.integrity = max_integrity(rule.integrity, transfer.required_integrity);
  if (degraded && degrade_reason == ReasonCode::DegradedMissingSloContract) {
    treatment.pacing = PacingClass::BestEffort;
    treatment.reserved_weight = 0;
  }
  if (request.priority_hint == PriorityHint::Background &&
      static_cast<std::uint16_t>(treatment.priority) <
          static_cast<std::uint16_t>(PriorityClass::Background)) {
    treatment.priority = PriorityClass::Background;
    if (!degraded) {
      degraded = true;
      degrade_reason = ReasonCode::DegradedPriorityLowered;
    }
    log.add(ExplanationRule::PolicyRule, static_cast<std::int64_t>(PriorityHint::Background), 1);
  }
  if (degraded && degrade_reason == ReasonCode::AdmittedConservative) {
    treatment.pacing = PacingClass::BestEffort;
    treatment.reserved_weight = 0;
    treatment.isolation = IsolationDomain::Background;
  }

  const DecisionOutcome outcome = admitted_by_guard
                                      ? DecisionOutcome::Allowed
                                      : (degraded ? DecisionOutcome::Degraded
                                                  : DecisionOutcome::Allowed);
  const ReasonCode reason = admitted_by_guard
                                ? ReasonCode::AdmittedStarvationGuard
                                : (degraded ? degrade_reason : ReasonCode::Admitted);
  Decision decision = build(outcome, reason, effective);
  decision.treatment = treatment;
  return decision;
}

Decision Coordinator::evaluate(const TrafficRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  return evaluate_locked(request);
}

Status Coordinator::validate_decision_locked(const Decision& decision) const {
  const AuthorityVerdict verdict =
      validate_authority(decision.authority, epoch_, incarnation_, policy_.generation());
  switch (verdict) {
    case AuthorityVerdict::Current:
      break;
    case AuthorityVerdict::StaleEpoch:
      return Status(StatusCode::StaleEpoch, "decision was issued by a previous coordinator epoch");
    case AuthorityVerdict::StalePolicy:
      return Status(StatusCode::StaleGeneration, "decision was issued under a previous policy");
    case AuthorityVerdict::StaleIncarnation:
      return Status(StatusCode::StaleEpoch, "decision was issued by a previous incarnation");
    case AuthorityVerdict::Absent:
      return Status(StatusCode::StaleAuthority, "decision carries no authority stamp");
    case AuthorityVerdict::Unknown:
    case AuthorityVerdict::Count:
      return Status(StatusCode::StaleAuthority, "decision authority could not be established");
  }
  if (!decision.permits_traffic()) {
    return Status(StatusCode::Refused, "decision does not permit traffic");
  }
  if (decision.subject.kind == SubjectKind::Serving) {
    auto record = ledger_.snapshot_request(decision.subject.request);
    if (!record.has_value()) return Status(StatusCode::NotFound, "request no longer exists");
    if (record->lifecycle == RequestLifecycle::Cancelled) {
      return Status(StatusCode::Cancelled, "request was cancelled after the decision was issued");
    }
    if (record->requires_revalidation) {
      return Status(StatusCode::RevalidationRequired, "request requires revalidation");
    }
    if (is_terminal(record->lifecycle)) {
      return Status(StatusCode::Conflict, "request is terminal");
    }
    if (!record->is_current_attempt(decision.subject.attempt)) {
      return Status(StatusCode::StaleGeneration, "attempt was superseded after the decision");
    }
    if (!record->authority_floor.is_unset() &&
        decision.authority.sequence <= record->authority_floor) {
      return Status(StatusCode::StaleAuthority,
                    "decision predates the request's current authority floor");
    }
  } else if (decision.subject.kind == SubjectKind::Transfer) {
    auto transfer = transfers_.find(decision.subject.transfer);
    if (transfer == transfers_.end()) return Status(StatusCode::NotFound, "transfer no longer exists");
    if (transfer->second.state == TransferState::Completed ||
        transfer->second.state == TransferState::Refused ||
        transfer->second.state == TransferState::Abandoned) {
      return Status(StatusCode::Conflict, "transfer is terminal");
    }
    if (transfer->second.model_hash != 0) {
      auto model = require_model_generation_locked(transfer->second.model_hash);
      if (!model.ok()) return model.status();
      if (model.value() != transfer->second.model_generation) {
        return Status(StatusCode::StaleGeneration, "model generation advanced");
      }
    }
    auto state = require_state_generation_locked(transfer->second.state_hash);
    if (!state.ok()) return state.status();
    if (state.value() != transfer->second.state_generation) {
      return Status(StatusCode::StaleGeneration, "state generation advanced");
    }
  }
  return Status::success();
}

Status Coordinator::validate_decision(const Decision& decision) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return validate_decision_locked(decision);
}

Result<FlowId> Coordinator::open_flow_locked(const Decision& decision, std::uint64_t declared_bytes,
                                             std::int64_t now) {
  const Status verdict = validate_decision_locked(decision);
  if (!verdict.ok()) return Result<FlowId>::failure(verdict);
  if (declared_bytes > config_.limits.max_single_payload_bytes) {
    return Result<FlowId>::failure(StatusCode::BoundsExceeded, "declared flow size is too large");
  }
  const TrafficClass traffic = decision.treatment.traffic_class;
  const bool bulk = policy_.rule(traffic).yields_to_latency;
  if (bulk && policy_.bulk_in_flight_cap_bytes() > 0 &&
      bulk_in_flight_bytes_ + declared_bytes > policy_.bulk_in_flight_cap_bytes()) {
    return Result<FlowId>::failure(StatusCode::CapacityExhausted,
                                   "bulk in-flight capacity is exhausted");
  }
  FlowRecord flow;
  flow.traffic = traffic;
  flow.declared_bytes = declared_bytes;
  flow.sequence = decision.authority.sequence;
  flow.bulk = bulk;
  if (decision.subject.kind == SubjectKind::Serving) {
    flow.request = decision.subject.request;
    flow.attempt = decision.subject.attempt;
    auto opened = ledger_.open_flow(flow.request, flow.attempt, traffic, declared_bytes, now);
    if (!opened.ok()) return Result<FlowId>::failure(opened.status());
    flow.id = opened.value();
  } else {
    auto transfer = transfers_.find(decision.subject.transfer);
    if (transfer == transfers_.end()) {
      return Result<FlowId>::failure(StatusCode::NotFound, "transfer no longer exists");
    }
    if (!transfer->second.flow.is_nil()) {
      return Result<FlowId>::success(transfer->second.flow);
    }
    next_flow_sequence_ += 1;
    flow.transfer = decision.subject.transfer;
    flow.id = FlowId(next_flow_sequence_,
                     mix64(decision.subject.transfer.low() ^
                           (decision.subject.transfer.high() + 0xA24BAED4963EE407ULL)));
    transfer->second.flow = flow.id;
  }
  flows_.emplace(flow.id, flow);
  if (!flow.request.is_nil()) flows_by_request_.emplace(flow.request, flow.id);
  if (!flow.transfer.is_nil()) flows_by_transfer_.emplace(flow.transfer, flow.id);
  if (bulk) bulk_in_flight_bytes_ += declared_bytes;
  return Result<FlowId>::success(flow.id);
}

Result<FlowId> Coordinator::open_flow(const Decision& decision, std::uint64_t declared_bytes) {
  std::lock_guard<std::mutex> guard(mutex_);
  return open_flow_locked(decision, declared_bytes, clock_->now_nanos());
}

void Coordinator::release_flow_locked(const FlowRecord& flow) {
  if (flow.bulk) {
    if (bulk_in_flight_bytes_ >= flow.declared_bytes) {
      bulk_in_flight_bytes_ -= flow.declared_bytes;
    } else {
      bulk_in_flight_bytes_ = 0;
    }
  }
  flows_.erase(flow.id);
  if (!flow.request.is_nil()) {
    const auto range = flows_by_request_.equal_range(flow.request);
    for (auto entry = range.first; entry != range.second; ++entry) {
      if (entry->second == flow.id) {
        flows_by_request_.erase(entry);
        break;
      }
    }
    (void)ledger_.release_flow(flow.id, clock_->now_nanos());
  }
  if (!flow.transfer.is_nil()) {
    const auto range = flows_by_transfer_.equal_range(flow.transfer);
    for (auto entry = range.first; entry != range.second; ++entry) {
      if (entry->second == flow.id) {
        flows_by_transfer_.erase(entry);
        break;
      }
    }
    auto transfer = transfers_.find(flow.transfer);
    if (transfer != transfers_.end() && transfer->second.flow == flow.id) {
      transfer->second.flow = FlowId();
    }
  }
}

void Coordinator::release_request_flows_locked(RequestId request) {
  if (request.is_nil()) return;
  std::vector<FlowId> victims;
  const auto range = flows_by_request_.equal_range(request);
  for (auto entry = range.first; entry != range.second; ++entry) victims.push_back(entry->second);
  // Transfers owned by the request are terminated with it: a request that can
  // no longer carry authority cannot keep a transfer flow open either.
  const auto transfers = transfers_by_request_.equal_range(request);
  for (auto entry = transfers.first; entry != transfers.second; ++entry) {
    auto transfer = transfers_.find(entry->second);
    if (transfer == transfers_.end()) continue;
    if (!transfer->second.flow.is_nil()) victims.push_back(transfer->second.flow);
    if (transfer->second.state != TransferState::Completed &&
        transfer->second.state != TransferState::Refused &&
        transfer->second.state != TransferState::Abandoned) {
      transfer->second.state = TransferState::Abandoned;
      transfer->second.terminal_reason = ReasonCode::RejectedRequestTerminal;
    }
  }
  for (const FlowId& id : victims) {
    const auto found = flows_.find(id);
    if (found == flows_.end()) continue;
    const FlowRecord copy = found->second;
    release_flow_locked(copy);
  }
}

void Coordinator::release_transfer_flows_locked(StateTransferId transfer) {
  if (transfer.is_nil()) return;
  std::vector<FlowId> victims;
  const auto range = flows_by_transfer_.equal_range(transfer);
  for (auto entry = range.first; entry != range.second; ++entry) victims.push_back(entry->second);
  for (const FlowId& id : victims) {
    const auto found = flows_.find(id);
    if (found == flows_.end()) continue;
    const FlowRecord copy = found->second;
    release_flow_locked(copy);
  }
}

Status Coordinator::release_flow(FlowId flow) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = flows_.find(flow);
  if (found == flows_.end()) return Status(StatusCode::NotFound, "unknown flow");
  const FlowRecord copy = found->second;
  release_flow_locked(copy);
  return Status::success();
}

Status Coordinator::record_flow_bytes(FlowId flow, std::uint64_t bytes, bool inbound) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = flows_.find(flow);
  if (found == flows_.end()) return Status(StatusCode::NotFound, "unknown flow");
  std::uint64_t updated = 0;
  if (!checked_add(found->second.observed_bytes, bytes, updated)) {
    return Status(StatusCode::BoundsExceeded, "flow accounting overflow");
  }
  found->second.observed_bytes = updated;
  if (!found->second.request.is_nil()) return ledger_.record_flow_bytes(flow, bytes, inbound);
  return Status::success();
}

Result<StateTransferId> Coordinator::register_transfer(const TransferRegistration& registration) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (registration.id.is_nil()) {
    return Result<StateTransferId>::failure(StatusCode::InvalidArgument, "nil transfer id");
  }
  if (!is_transfer_class(registration.declared_class)) {
    return Result<StateTransferId>::failure(StatusCode::InvalidArgument,
                                            "transfer requires a transfer traffic class");
  }
  if (registration.payload_bytes > config_.limits.max_single_payload_bytes) {
    return Result<StateTransferId>::failure(StatusCode::BoundsExceeded,
                                            "transfer payload exceeds the single-transfer limit");
  }
  if (transfers_.find(registration.id) != transfers_.end()) {
    return Result<StateTransferId>::failure(StatusCode::AlreadyExists, "transfer already registered");
  }
  if (transfers_.size() >= config_.limits.max_transfer_records) {
    return Result<StateTransferId>::failure(StatusCode::CapacityExhausted,
                                            "transfer registry is full");
  }
  auto model = require_model_generation_locked(registration.model_hash);
  if (!model.ok()) return Result<StateTransferId>::failure(model.status());
  auto state = require_state_generation_locked(registration.state_hash);
  if (!state.ok()) return Result<StateTransferId>::failure(state.status());
  if (!registration.request.is_nil() && ledger_.is_cancelled(registration.request)) {
    return Result<StateTransferId>::failure(StatusCode::Cancelled,
                                            "owner request is cancelled");
  }
  TransferRecord record;
  record.id = registration.id;
  record.state_hash = registration.state_hash;
  record.model_hash = registration.model_hash;
  record.payload_bytes = registration.payload_bytes;
  record.model_generation = model.value();
  record.state_generation = state.value();
  record.request = registration.request;
  record.attempt = registration.attempt;
  record.declared_class = registration.declared_class;
  record.direction = registration.direction;
  record.required_integrity = registration.required_integrity;
  record.state = TransferState::Registered;
  record.deadline = registration.deadline;
  record.registered_at = Instant::at(clock_->now_nanos());
  transfers_.emplace(record.id, record);
  if (!record.request.is_nil()) transfers_by_request_.emplace(record.request, record.id);
  return Result<StateTransferId>::success(record.id);
}

Result<TransferAuthorization> Coordinator::authorize_transfer(const TrafficRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!request.subject.has_value() || request.subject->kind != SubjectKind::Transfer) {
    return Result<TransferAuthorization>::failure(StatusCode::InvalidArgument,
                                                  "transfer authorization requires a transfer subject");
  }
  const TrafficSubject subject = *request.subject;
  const std::int64_t now = clock_->now_nanos();
  const Decision decision = evaluate_locked(request);

  TransferAuthorization authorization;
  authorization.transfer = subject.transfer;
  authorization.authority = decision.authority;
  authorization.reason = decision.reason;
  authorization.authorized = decision.permits_traffic();
  authorization.traffic_class = decision.treatment.traffic_class;
  authorization.integrity = decision.treatment.integrity;
  authorization.pacing = decision.treatment.pacing;
  authorization.priority = decision.treatment.priority;
  if (!authorization.authorized) {
    auto transfer = transfers_.find(subject.transfer);
    if (transfer != transfers_.end()) {
      transfer->second.state = TransferState::Refused;
      transfer->second.terminal_reason = decision.reason;
      release_transfer_flows_locked(subject.transfer);
    }
    transfers_refused_ += 1;
    return Result<TransferAuthorization>::success(authorization);
  }
  auto transfer = transfers_.find(subject.transfer);
  if (transfer == transfers_.end()) {
    return Result<TransferAuthorization>::failure(StatusCode::NotFound, "transfer no longer exists");
  }
  auto flow = open_flow_locked(decision, transfer->second.payload_bytes, now);
  if (!flow.ok()) {
    transfer->second.state = TransferState::Refused;
    transfer->second.terminal_reason = ReasonCode::RejectedFlowLimit;
    transfers_refused_ += 1;
    authorization.authorized = false;
    authorization.reason = ReasonCode::RejectedFlowLimit;
    return Result<TransferAuthorization>::success(authorization);
  }
  authorization.flow = flow.value();
  transfer->second.state = TransferState::Authorized;
  transfer->second.flow = flow.value();
  transfers_started_ += 1;
  return Result<TransferAuthorization>::success(authorization);
}

Status Coordinator::start_transfer(StateTransferId transfer_id,
                                   const TransferAuthorization& authorization) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto transfer = transfers_.find(transfer_id);
  if (transfer == transfers_.end()) return Status(StatusCode::NotFound, "unknown transfer");
  if (authorization.transfer != transfer_id) {
    return Status(StatusCode::InvalidArgument, "authorization belongs to a different transfer");
  }
  if (!authorization.authorized) {
    return Status(StatusCode::NotAuthorized, "transfer was not authorized");
  }
  if (transfer->second.state == TransferState::InProgress) return Status::success();
  if (transfer->second.state != TransferState::Authorized) {
    return Status(StatusCode::Conflict, "transfer is not in an authorized state");
  }
  transfer->second.state = TransferState::InProgress;
  return Status::success();
}

Status Coordinator::complete_transfer(StateTransferId transfer_id, std::uint64_t verified_bytes) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto transfer = transfers_.find(transfer_id);
  if (transfer == transfers_.end()) return Status(StatusCode::NotFound, "unknown transfer");
  TransferRecord& record = transfer->second;
  if (record.state != TransferState::Authorized && record.state != TransferState::InProgress) {
    return Status(StatusCode::Conflict, "transfer is not in an authorized state");
  }
  if (record.required_integrity == IntegrityClass::ChecksumAndVerify &&
      verified_bytes != record.payload_bytes) {
    record.state = TransferState::Refused;
    record.terminal_reason = ReasonCode::RejectedIntegrityUnavailable;
    release_transfer_flows_locked(transfer_id);
    transfers_refused_ += 1;
    return Status(StatusCode::IntegrityMismatch,
                  "verified byte count does not match the authorized payload");
  }
  if (verified_bytes > record.payload_bytes) {
    record.state = TransferState::Refused;
    record.terminal_reason = ReasonCode::RejectedIntegrityUnavailable;
    release_transfer_flows_locked(transfer_id);
    transfers_refused_ += 1;
    return Status(StatusCode::IntegrityMismatch,
                  "verified byte count exceeds the authorized payload");
  }
  record.verified_bytes = verified_bytes;
  record.state = TransferState::Completed;
  record.terminal_reason = ReasonCode::CommittedCompletion;
  release_transfer_flows_locked(transfer_id);
  transfers_completed_ += 1;
  return Status::success();
}

Status Coordinator::abandon_transfer(StateTransferId transfer_id, ReasonCode reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto transfer = transfers_.find(transfer_id);
  if (transfer == transfers_.end()) return Status(StatusCode::NotFound, "unknown transfer");
  if (transfer->second.state == TransferState::Completed) {
    return Status(StatusCode::Conflict, "transfer already completed");
  }
  transfer->second.state = TransferState::Abandoned;
  transfer->second.terminal_reason = reason;
  release_transfer_flows_locked(transfer_id);
  transfers_refused_ += 1;
  return Status::success();
}

Result<PublishOutcome> Coordinator::publish_completion(const CompletionPublication& publication) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::int64_t now = clock_->now_nanos();
  auto outcome = ledger_.publish_completion(publication, now);
  if (!outcome.ok()) return outcome;
  PublishOutcome value = outcome.value();
  if (value.disposition == PublishOutcome::Disposition::Committed) {
    release_request_flows_locked(publication.request);
    completions_[publication.request] = value;
    completion_order_.push_back(publication.request);
    while (completion_order_.size() > config_.max_retained_completion_records) {
      const RequestId victim = completion_order_.front();
      completion_order_.pop_front();
      completions_.erase(victim);
    }
  } else {
    const auto existing = completions_.find(publication.request);
    if (existing != completions_.end() &&
        value.disposition == PublishOutcome::Disposition::DuplicateSuppressed) {
      value.accounted_bytes_in = existing->second.accounted_bytes_in;
      value.accounted_bytes_out = existing->second.accounted_bytes_out;
    }
  }
  return Result<PublishOutcome>::success(value);
}

std::uint32_t Coordinator::accounting_unlocked_active_flows(
    const AccountingSnapshot& ledger_snapshot) const noexcept {
  std::uint32_t total = ledger_snapshot.active_flows_total;
  for (const auto& entry : flows_) {
    if (entry.second.request.is_nil()) total += 1;
  }
  return total;
}

AccountingSnapshot Coordinator::accounting() const {
  std::lock_guard<std::mutex> guard(mutex_);
  AccountingSnapshot snapshot = ledger_.accounting();
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    const ClassAccounting& extra = transfer_class_accounting_[index];
    ClassAccounting& base = snapshot.per_class[index];
    base.allowed += extra.allowed;
    base.degraded += extra.degraded;
    base.deferred += extra.deferred;
    base.rejected += extra.rejected;
    base.bytes_allowed += extra.bytes_allowed;
    base.bytes_deferred += extra.bytes_deferred;
    base.bytes_rejected += extra.bytes_rejected;
    base.starvation_admissions += extra.starvation_admissions;
  }
  for (const auto& entry : flows_) {
    const FlowRecord& flow = entry.second;
    if (!flow.request.is_nil()) continue;  // already counted by the ledger
    snapshot.active_flows_total += 1;
    const std::size_t index = static_cast<std::size_t>(flow.traffic);
    if (index < kTrafficClassCount) snapshot.per_class[index].active_flows += 1;
  }
  if (snapshot.active_flows_total > snapshot.peak_active_flows_total) {
    snapshot.peak_active_flows_total = snapshot.active_flows_total;
  }
  snapshot.transfers_started = transfers_started_;
  snapshot.transfers_completed = transfers_completed_;
  snapshot.transfers_refused = transfers_refused_;
  snapshot.pending_deferrals = static_cast<std::uint32_t>(defers_.size());
  return snapshot;
}

Status Coordinator::verify_invariants() const {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status closure = ledger_.verify_closure();
  if (!closure.ok()) return closure;
  const AccountingSnapshot snapshot = ledger_.accounting();
  std::uint32_t serving_flows = 0;
  std::uint64_t bulk_bytes = 0;
  std::uint32_t starvation_defers = 0;
  for (const auto& entry : flows_) {
    const FlowRecord& flow = entry.second;
    if (flow.request.is_nil() && flow.transfer.is_nil()) {
      return Status(StatusCode::Internal, "flow record has no subject");
    }
    if (!flow.request.is_nil()) {
      ++serving_flows;
      const auto record = ledger_.snapshot_request(flow.request);
      if (!record.has_value()) {
        return Status(StatusCode::Internal, "flow references a request that no longer exists");
      }
      if (record->lifecycle == RequestLifecycle::Cancelled) {
        return Status(StatusCode::Internal, "cancelled request retains an open flow");
      }
    } else {
      const auto transfer = transfers_.find(flow.transfer);
      if (transfer == transfers_.end()) {
        return Status(StatusCode::Internal, "flow references a transfer that no longer exists");
      }
      if (transfer->second.state == TransferState::Completed ||
          transfer->second.state == TransferState::Refused ||
          transfer->second.state == TransferState::Abandoned) {
        return Status(StatusCode::Internal, "terminal transfer retains an open flow");
      }
      if (!transfer->second.request.is_nil()) {
        const auto owner = ledger_.snapshot_request(transfer->second.request);
        if (owner.has_value() && is_terminal(owner->lifecycle)) {
          return Status(StatusCode::Internal,
                        "transfer of a terminal request retains an open flow");
        }
      }
    }
    if (flow.bulk) bulk_bytes += flow.declared_bytes;
  }
  if (serving_flows != snapshot.active_flows_total) {
    return Status(StatusCode::Internal,
                  "coordinator and ledger disagree about active serving flows");
  }
  if (bulk_bytes != bulk_in_flight_bytes_) {
    return Status(StatusCode::Internal, "bulk in-flight byte accounting diverged");
  }
  for (const auto& entry : defers_) {
    if (entry.second.starvation_class) ++starvation_defers;
  }
  if (starvation_defers != streaming_deferrals_) {
    return Status(StatusCode::Internal, "streaming deferral counter diverged");
  }
  if (defers_.size() > config_.limits.max_defer_records) {
    return Status(StatusCode::Internal, "deferral records exceed their configured ceiling");
  }
  if (flows_.size() > config_.limits.ledger.max_active_flows_total) {
    return Status(StatusCode::Internal, "flow records exceed their configured ceiling");
  }
  return Status::success();
}

std::string Coordinator::explain_request(RequestId request) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto record = ledger_.snapshot_request(request);
  if (!record.has_value()) return "request not found";
  std::string out;
  out.reserve(512);
  out.append("request ");
  out.append(request.to_string());
  out.append(" generation=");
  out.append(std::to_string(record->generation.value()));
  out.append(" lifecycle=");
  out.append(itf::to_string(record->lifecycle));
  out.append(" stage=");
  out.append(itf::to_string(record->stage));
  out.append(" requires_revalidation=");
  out.append(record->requires_revalidation ? "true" : "false");
  out.append(" terminal_reason=");
  out.append(itf::to_string(record->terminal_reason));
  out.append(" bytes_in=");
  out.append(std::to_string(record->bytes_in));
  out.append(" bytes_out=");
  out.append(std::to_string(record->bytes_out));
  out.append(" decisions=");
  out.append(std::to_string(record->decisions));
  out.append(" defers=");
  out.append(std::to_string(record->defers));
  out.append(" rejections=");
  out.append(std::to_string(record->rejections));
  out.append(" active_flows=");
  out.append(std::to_string(record->active_flows));
  out.append(" authority_floor=");
  out.append(std::to_string(record->authority_floor.value()));
  out.append("\n");
  for (std::uint32_t index = 0; index < record->attempt_count && index < kMaxAttemptsPerRequest;
       ++index) {
    const AttemptRecord& attempt = record->attempts[index];
    out.append("  attempt ");
    out.append(attempt.id.to_string());
    out.append(" generation=");
    out.append(std::to_string(attempt.generation.value()));
    out.append(" stage=");
    out.append(itf::to_string(attempt.stage));
    out.append(" lifecycle=");
    out.append(itf::to_string(attempt.lifecycle));
    out.append(" model_generation=");
    out.append(std::to_string(attempt.model_generation.value()));
    out.append(" route_generation=");
    out.append(std::to_string(attempt.route_generation.value()));
    out.append(" success=");
    out.append(attempt.success_published ? "true" : "false");
    out.append(" failure=");
    out.append(attempt.failure_published ? "true" : "false");
    out.append("\n");
  }
  return out;
}

std::optional<RequestRecord> Coordinator::request_record(RequestId request) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return ledger_.snapshot_request(request);
}

Result<PersistedCoordinatorState> Coordinator::export_state() const {
  std::lock_guard<std::mutex> guard(mutex_);
  PersistedCoordinatorState state;
  state.format_version = kSnapshotFormatVersion;
  state.epoch = epoch_;
  state.previous_incarnation = incarnation_;
  state.boot_count = boot_count_;
  state.policy = policy_;
  state.topology_present = topology_present_;
  state.topology = topology_;
  state.models.reserve(models_.size());
  for (const auto& entry : models_) state.models.push_back(entry.second);
  std::sort(state.models.begin(), state.models.end(),
            [](const ModelState& a, const ModelState& b) { return a.model_hash < b.model_hash; });
  state.states.reserve(states_.size());
  for (const auto& entry : states_) state.states.push_back(entry.second);
  std::sort(state.states.begin(), state.states.end(),
            [](const StateGenerationRecord& a, const StateGenerationRecord& b) {
              return a.state_hash < b.state_hash;
            });
  state.terminal_requests = ledger_.export_terminal_records(
      config_.limits.ledger.max_retained_terminal_requests);
  state.interrupted_requests = ledger_.export_live_records();
  return Result<PersistedCoordinatorState>::success(std::move(state));
}

}  // namespace itf
