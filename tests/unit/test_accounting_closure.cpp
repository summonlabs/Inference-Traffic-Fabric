// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "itf/coordinator.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x0F0F0F0F0F0F0F0FULL;
constexpr std::uint64_t kStateHash = 0xF0F0F0F0F0F0F0F0ULL;

struct Fixture {
  VirtualClock clock;
  Coordinator coordinator;

  Fixture() : coordinator(make_config(), &clock) {
    (void)coordinator.boot_fresh();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Synthetic;
    topology.node_count = 2;
    topology.link_count = 1;
    topology.accelerator_count = 2;
    topology.fabric_bytes_per_sec = 100000000000ULL;
    topology.max_age_nanos = 1000000000ULL;
    topology.disaggregated = true;
    (void)coordinator.publish_topology(topology);
    (void)coordinator.set_model_generation(kModelHash, std::nullopt);
    (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
  }

  static CoordinatorConfig make_config() {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.boot_seed = 99;
    return config;
  }

  void prepare(const RequestId& request, const AttemptId& attempt, ServingStage stage) {
    (void)coordinator.register_request(request);
    (void)coordinator.register_attempt(request, attempt, kModelHash);
    (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
    (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    if (stage != ServingStage::Unknown) (void)coordinator.advance_stage(request, attempt, stage);
  }

  TrafficRequest message(const RequestId& request, const AttemptId& attempt, TrafficClass traffic,
                         ServingStage stage, std::uint64_t bytes) {
    TrafficRequest request_message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    request_message.subject = subject;
    request_message.declared_class = traffic;
    request_message.declared_stage = stage;
    request_message.direction = FlowDirection::Ingress;
    request_message.payload_bytes = bytes;
    request_message.binding.policy = coordinator.policy().generation();
    request_message.binding.coordinator_epoch = coordinator.epoch();
    request_message.binding.model_generation = ModelGeneration::from(1);
    auto route = coordinator.issue_route_decision(request, attempt, 0, false, false);
    if (route.ok()) request_message.binding.route_decision = route.value().generation;
    auto slo = coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    if (slo.ok()) request_message.binding.slo_contract = slo.value().generation;
    request_message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    return request_message;
  }
};

ITF_TEST(per_class_accounting_tracks_outcomes) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(21, 1);
  const AttemptId attempt = itf::test::make_attempt_id(21, 1);
  fixture.prepare(request, attempt, ServingStage::Admitted);
  (void)fixture.coordinator.evaluate(
      fixture.message(request, attempt, TrafficClass::RequestIngress, ServingStage::Admitted, 2048));

  const AccountingSnapshot snapshot = fixture.coordinator.accounting();
  const ClassAccounting& ingress =
      snapshot.per_class[static_cast<std::size_t>(TrafficClass::RequestIngress)];
  ITF_CHECK_EQ(ingress.allowed, 1ULL);
  ITF_CHECK_EQ(ingress.bytes_allowed, 2048ULL);
  ITF_CHECK_EQ(ingress.rejected, 0ULL);
  ITF_CHECK_EQ(snapshot.requests_registered, 1ULL);
  ITF_CHECK(snapshot.closed());
}

ITF_TEST(rejections_are_accounted_separately) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(22, 1);
  const AttemptId attempt = itf::test::make_attempt_id(22, 1);
  fixture.prepare(request, attempt, ServingStage::Unknown);
  auto message =
      fixture.message(request, attempt, TrafficClass::RequestIngress, ServingStage::Unknown, 1024);
  const Decision decision = fixture.coordinator.evaluate(message);
  ITF_CHECK_EQ(decision.reason, ReasonCode::RejectedUnknownStage);

  const AccountingSnapshot snapshot = fixture.coordinator.accounting();
  const ClassAccounting& ingress =
      snapshot.per_class[static_cast<std::size_t>(TrafficClass::RequestIngress)];
  ITF_CHECK_EQ(ingress.rejected, 1ULL);
  ITF_CHECK_EQ(ingress.bytes_rejected, 1024ULL);
  const auto request_accounting = fixture.coordinator.request_record(request);
  ITF_REQUIRE(request_accounting.has_value());
  ITF_CHECK_EQ(request_accounting->rejections, 1U);
}

ITF_TEST(deferrals_are_counted_and_pending_deferrals_close) {
  CoordinatorConfig config = Fixture::make_config();
  PolicyConfig policy = config.policy;
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  streaming.max_defer_count = 3;
  streaming.max_defer_horizon_nanos = 1000000ULL;
  (void)policy.set_rule(TrafficClass::DecodeStream, streaming);
  policy.set_congestion_queue_depth(1);
  config.policy = policy;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.queue_depth = 64;
  topology.congestion_signalled = true;
  topology.fabric_bytes_per_sec = 1000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);

  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
  const RequestId request = itf::test::make_request_id(23, 1);
  const AttemptId attempt = itf::test::make_attempt_id(23, 1);
  (void)coordinator.register_request(request);
  (void)coordinator.register_attempt(request, attempt, kModelHash);
  (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
  (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
  for (const ServingStage stage :
       {ServingStage::Admitted, ServingStage::PrefillQueued, ServingStage::PrefillRunning,
        ServingStage::HandoffPending, ServingStage::DecodeQueued, ServingStage::DecodeRunning,
        ServingStage::Streaming}) {
    (void)coordinator.advance_stage(request, attempt, stage);
  }
  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::DecodeStream;
  message.declared_stage = ServingStage::Streaming;
  message.payload_bytes = 512;
  message.binding.policy = coordinator.policy().generation();
  message.binding.coordinator_epoch = coordinator.epoch();
  message.binding.model_generation = ModelGeneration::from(1);
  message.binding.route_decision = RouteDecisionGeneration::from(1);
  message.binding.slo_contract = SloContractGeneration::from(1);
  message.deadline = Instant::from_now(clock.now_nanos(), 20000000);

  const Decision first = coordinator.evaluate(message);
  ITF_CHECK_EQ(first.outcome, DecisionOutcome::Deferred);
  ITF_CHECK_EQ(coordinator.accounting().pending_deferrals, 1U);

  bool admitted = false;
  for (int index = 0; index < 8 && !admitted; ++index) {
    const Decision decision = coordinator.evaluate(message);
    if (decision.permits_traffic()) {
      admitted = true;
      ITF_CHECK_EQ(decision.reason, ReasonCode::AdmittedStarvationGuard);
    }
    clock.advance(100000);
  }
  ITF_CHECK(admitted);
  ITF_CHECK_EQ(coordinator.accounting().pending_deferrals, 0U);
  ITF_CHECK_EQ(coordinator.accounting()
                   .per_class[static_cast<std::size_t>(TrafficClass::DecodeStream)]
                   .starvation_admissions,
               1ULL);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(active_flow_counts_close_after_completion) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(24, 1);
  const AttemptId attempt = itf::test::make_attempt_id(24, 1);
  fixture.prepare(request, attempt, ServingStage::Admitted);
  const TrafficRequest message =
      fixture.message(request, attempt, TrafficClass::RequestIngress, ServingStage::Admitted, 8192);
  const Decision decision = fixture.coordinator.evaluate(message);
  ITF_REQUIRE(decision.permits_traffic());
  const auto flow = fixture.coordinator.open_flow(decision, message.payload_bytes);
  ITF_REQUIRE(flow.ok());
  ITF_CHECK_EQ(fixture.coordinator.accounting().active_flows_total, 1U);
  ITF_CHECK_EQ(fixture.coordinator.accounting().peak_active_flows_total, 1U);
  (void)fixture.coordinator.record_flow_bytes(flow.value(), message.payload_bytes, true);

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.bytes_in = 8192;
  const auto outcome = fixture.coordinator.publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().committed());
  ITF_CHECK_EQ(outcome.value().released_flows, 1U);
  ITF_CHECK_EQ(fixture.coordinator.accounting().active_flows_total, 0U);
  ITF_CHECK(fixture.coordinator.accounting().closed());
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.verify_invariants());

  const auto request_accounting = fixture.coordinator.request_record(request);
  ITF_REQUIRE(request_accounting.has_value());
  ITF_CHECK_EQ(request_accounting->bytes_in, 8192ULL);
  ITF_CHECK_EQ(request_accounting->active_flows, 0U);
}

ITF_TEST(a_completed_request_closes_the_transfer_flow_it_owns) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(28, 1);
  const AttemptId attempt = itf::test::make_attempt_id(28, 1);
  fixture.prepare(request, attempt, ServingStage::HandoffPending);

  TransferRegistration registration;
  registration.id = itf::test::make_transfer_id(28, 1);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = 4U << 20;
  registration.declared_class = TrafficClass::KvTransfer;
  registration.direction = FlowDirection::Lateral;
  registration.request = request;
  registration.attempt = attempt;
  ITF_REQUIRE(fixture.coordinator.register_transfer(registration).ok());

  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Transfer;
  subject.transfer = registration.id;
  message.subject = subject;
  message.declared_class = TrafficClass::KvTransfer;
  message.direction = FlowDirection::Lateral;
  message.payload_bytes = registration.payload_bytes;
  message.binding.policy = fixture.coordinator.policy().generation();
  message.binding.coordinator_epoch = fixture.coordinator.epoch();
  message.binding.model_generation = fixture.coordinator.model_generation(kModelHash).value();
  message.binding.state_generation = fixture.coordinator.state_generation(kStateHash).value();
  message.binding.slo_contract = SloContractGeneration::from(1);
  const auto authorization = fixture.coordinator.authorize_transfer(message);
  ITF_REQUIRE(authorization.ok());
  if (!authorization.value().authorized) {
    std::printf("  transfer refused: %s\n",
                std::string(to_string(authorization.value().reason)).c_str());
  }
  ITF_REQUIRE(authorization.value().authorized);
  ITF_CHECK_EQ(fixture.coordinator.accounting().active_flows_total, 1U);

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.bytes_out = 1024;
  const auto outcome = fixture.coordinator.publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().committed());
  ITF_CHECK_EQ(fixture.coordinator.accounting().active_flows_total, 0U);
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.verify_invariants());
}

ITF_TEST(direct_api_misuse_is_reported_not_ignored) {
  Fixture fixture;
  Decision unstamped;
  unstamped.outcome = DecisionOutcome::Allowed;
  const auto missing_authority = fixture.coordinator.open_flow(unstamped, 16);
  ITF_CHECK_EQ(static_cast<int>(missing_authority.code()),
               static_cast<int>(StatusCode::StaleAuthority));

  const RequestId request = itf::test::make_request_id(27, 1);
  const AttemptId attempt = itf::test::make_attempt_id(27, 1);
  fixture.prepare(request, attempt, ServingStage::Unknown);
  auto message =
      fixture.message(request, attempt, TrafficClass::RequestIngress, ServingStage::Unknown, 16);
  const Decision rejected = fixture.coordinator.evaluate(message);
  ITF_CHECK_EQ(rejected.outcome, DecisionOutcome::Rejected);
  const auto flow = fixture.coordinator.open_flow(rejected, 16);
  ITF_CHECK_EQ(static_cast<int>(flow.code()), static_cast<int>(StatusCode::Refused));
  ITF_CHECK_EQ(static_cast<int>(fixture.coordinator.release_flow(FlowId(1, 1)).code()),
               static_cast<int>(StatusCode::NotFound));
  ITF_CHECK_EQ(static_cast<int>(fixture.coordinator.record_flow_bytes(FlowId(1, 1), 1, true).code()),
               static_cast<int>(StatusCode::NotFound));
  ITF_CHECK_EQ(static_cast<int>(fixture.coordinator.cancel_request(
                                    itf::test::make_request_id(25, 1), ReasonCode::Cancelled)
                                    .code()),
               static_cast<int>(StatusCode::NotFound));
}

ITF_TEST(per_request_accounting_snapshot_reports_closure) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(26, 1);
  const AttemptId attempt = itf::test::make_attempt_id(26, 1);
  fixture.prepare(request, attempt, ServingStage::Admitted);
  const auto before = fixture.coordinator.request_record(request);
  ITF_REQUIRE(before.has_value());
  ITF_CHECK(!before->accounting_closed);
  (void)fixture.coordinator.cancel_request(request, ReasonCode::Cancelled);
  const auto after = fixture.coordinator.request_record(request);
  ITF_REQUIRE(after.has_value());
  ITF_CHECK(after->accounting_closed);
  ITF_CHECK_EQ(after->active_flows, 0U);
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.verify_invariants());
}


/// A coordinator with evidence, a model and a state identity, for the cases
/// that need their own limits or policy.
void prepare_coordinator(Coordinator& coordinator) {
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.accelerator_count = 2;
  topology.fabric_bytes_per_sec = 100000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
}

TransferRegistration bulk_transfer(std::uint64_t seed, std::uint64_t index, std::uint64_t bytes) {
  TransferRegistration registration;
  registration.id = itf::test::make_transfer_id(seed, index);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = bytes;
  registration.declared_class = TrafficClass::ModelTransfer;
  registration.direction = FlowDirection::Lateral;
  return registration;
}

TrafficRequest transfer_message(const Coordinator& coordinator,
                                const TransferRegistration& registration) {
  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Transfer;
  subject.transfer = registration.id;
  message.subject = subject;
  message.declared_class = registration.declared_class;
  message.direction = FlowDirection::Lateral;
  message.payload_bytes = registration.payload_bytes;
  message.binding.policy = coordinator.policy().generation();
  message.binding.coordinator_epoch = coordinator.epoch();
  return message;
}

ITF_TEST(a_new_policy_document_fences_decisions_minted_under_the_previous_one) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(31, 1);
  const AttemptId attempt = itf::test::make_attempt_id(31, 1);
  fixture.prepare(request, attempt, ServingStage::Admitted);

  // An operator ships a policy document. Like every document built from the
  // canonical defaults it carries its own generation number.
  ITF_REQUIRE_STATUS_OK(
      fixture.coordinator.apply_policy(PolicyConfig::canonical_defaults(PolicyGeneration::from(1))));
  const PolicyGeneration first = fixture.coordinator.policy().generation();
  const TrafficRequest message =
      fixture.message(request, attempt, TrafficClass::RequestIngress, ServingStage::Admitted, 1024);
  const Decision decision = fixture.coordinator.evaluate(message);
  ITF_REQUIRE(decision.permits_traffic());
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.validate_decision(decision));

  // A second, different document built the same way: it switches the class
  // off. Everything minted or bound under the first document must be fenced.
  PolicyConfig replacement = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule ingress = replacement.rule(TrafficClass::RequestIngress);
  ingress.enabled = false;
  ITF_REQUIRE_STATUS_OK(replacement.set_rule(TrafficClass::RequestIngress, ingress));
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.apply_policy(replacement));

  ITF_CHECK(fixture.coordinator.policy().generation() > first);
  ITF_CHECK_EQ(static_cast<int>(fixture.coordinator.validate_decision(decision).code()),
               static_cast<int>(StatusCode::StaleGeneration));
  ITF_CHECK(!fixture.coordinator.open_flow(decision, 1024).ok());
  const Decision stale = fixture.coordinator.evaluate(message);
  ITF_CHECK_EQ(stale.reason, ReasonCode::RejectedStalePolicyGeneration);
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.verify_invariants());
}

ITF_TEST(a_deferred_transfer_can_be_authorized_once_the_pressure_is_gone) {
  CoordinatorConfig config = Fixture::make_config();
  config.policy.set_bulk_in_flight_cap_bytes(1ULL << 20);
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  prepare_coordinator(coordinator);

  const TransferRegistration first = bulk_transfer(29, 1, 1ULL << 20);
  const TransferRegistration second = bulk_transfer(29, 2, 1ULL << 20);
  ITF_REQUIRE(coordinator.register_transfer(first).ok());
  ITF_REQUIRE(coordinator.register_transfer(second).ok());

  const auto admitted = coordinator.authorize_transfer(transfer_message(coordinator, first));
  ITF_REQUIRE(admitted.ok());
  ITF_REQUIRE(admitted.value().authorized);

  // The bulk allowance is in use, so the second transfer is told to wait.
  const auto deferred = coordinator.authorize_transfer(transfer_message(coordinator, second));
  ITF_REQUIRE(deferred.ok());
  ITF_CHECK(!deferred.value().authorized);
  ITF_CHECK_EQ(deferred.value().reason, ReasonCode::DeferredBulkCapacity);

  ITF_REQUIRE_STATUS_OK(coordinator.complete_transfer(first.id, first.payload_bytes));

  // A deferral is an instruction to retry, not a refusal: with the allowance
  // free again the same transfer is authorized.
  const auto retried = coordinator.authorize_transfer(transfer_message(coordinator, second));
  ITF_REQUIRE(retried.ok());
  if (!retried.value().authorized) {
    std::printf("  retry refused: %s\n", std::string(to_string(retried.value().reason)).c_str());
  }
  ITF_CHECK(retried.value().authorized);
  ITF_CHECK(coordinator.complete_transfer(second.id, second.payload_bytes).ok());
  const AccountingSnapshot snapshot = coordinator.accounting();
  ITF_CHECK_EQ(snapshot.transfers_completed, 2ULL);
  ITF_CHECK_EQ(snapshot.transfers_refused, 0ULL);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(a_cancelled_request_takes_its_deferral_with_it) {
  CoordinatorConfig config = Fixture::make_config();
  PolicyConfig policy = config.policy;
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  (void)policy.set_rule(TrafficClass::DecodeStream, streaming);
  policy.set_congestion_queue_depth(1);
  config.policy = policy;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.queue_depth = 64;
  topology.congestion_signalled = true;
  topology.fabric_bytes_per_sec = 1000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);

  const RequestId request = itf::test::make_request_id(32, 1);
  const AttemptId attempt = itf::test::make_attempt_id(32, 1);
  (void)coordinator.register_request(request);
  (void)coordinator.register_attempt(request, attempt, kModelHash);
  (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
  (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
  for (const ServingStage stage :
       {ServingStage::Admitted, ServingStage::PrefillQueued, ServingStage::PrefillRunning,
        ServingStage::HandoffPending, ServingStage::DecodeQueued, ServingStage::DecodeRunning,
        ServingStage::Streaming}) {
    (void)coordinator.advance_stage(request, attempt, stage);
  }
  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::DecodeStream;
  message.declared_stage = ServingStage::Streaming;
  message.payload_bytes = 512;
  message.binding.policy = coordinator.policy().generation();
  message.binding.coordinator_epoch = coordinator.epoch();
  message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
  const Decision waiting = coordinator.evaluate(message);
  ITF_REQUIRE(waiting.outcome == DecisionOutcome::Deferred);
  ITF_CHECK_EQ(coordinator.accounting().pending_deferrals, 1U);

  // The request is cancelled while its stream is waiting. Nothing will ever
  // evaluate that stream again, so nothing is waiting any more.
  ITF_REQUIRE_STATUS_OK(coordinator.cancel_request(request, ReasonCode::Cancelled));
  ITF_CHECK_EQ(coordinator.accounting().pending_deferrals, 0U);

  // The congestion clears. Bulk movement yields only while a protected flow is
  // deferred, and none is.
  topology.queue_depth = 0;
  topology.congestion_signalled = false;
  ITF_REQUIRE(coordinator.publish_topology(topology).ok());
  const TransferRegistration bulk = bulk_transfer(32, 1, 4096);
  ITF_REQUIRE(coordinator.register_transfer(bulk).ok());
  const Decision movement = coordinator.evaluate(transfer_message(coordinator, bulk));
  if (!movement.permits_traffic()) {
    std::printf("  bulk movement: %s\n", std::string(to_string(movement.reason)).c_str());
  }
  ITF_CHECK(movement.permits_traffic());
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(finished_requests_release_their_route_and_slo_entries) {
  CoordinatorConfig config = Fixture::make_config();
  config.limits.max_slo_contracts = 3;
  config.limits.max_route_decisions = 3;
  config.limits.ledger.max_retained_terminal_requests = 2;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  prepare_coordinator(coordinator);

  // Twelve requests, one after another, each finished before the next begins.
  // The registries are sized for three requests in progress, which is never
  // exceeded.
  int served = 0;
  for (std::uint64_t index = 0; index < 12; ++index) {
    const RequestId request = itf::test::make_request_id(33, index);
    const AttemptId attempt = itf::test::make_attempt_id(33, index);
    ITF_REQUIRE(coordinator.register_request(request).ok());
    ITF_REQUIRE(coordinator.register_attempt(request, attempt, kModelHash).ok());
    const auto route = coordinator.issue_route_decision(request, attempt, 0, false, false);
    const auto slo = coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    if (!route.ok() || !slo.ok()) {
      std::printf("  request %llu: route=%s slo=%s\n", static_cast<unsigned long long>(index),
                  route.status().to_string().c_str(), slo.status().to_string().c_str());
      break;
    }
    ITF_REQUIRE_STATUS_OK(coordinator.advance_stage(request, attempt, ServingStage::Admitted));
    TrafficRequest message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    message.subject = subject;
    message.declared_class = TrafficClass::RequestIngress;
    message.declared_stage = ServingStage::Admitted;
    message.payload_bytes = 1024;
    message.binding.policy = coordinator.policy().generation();
    message.binding.coordinator_epoch = coordinator.epoch();
    message.binding.route_decision = route.value().generation;
    message.binding.slo_contract = slo.value().generation;
    message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    const Decision decision = coordinator.evaluate(message);
    if (!decision.permits_traffic()) break;
    CompletionPublication publication;
    publication.request = request;
    publication.attempt = attempt;
    publication.attempt_generation = AttemptGeneration::from(1);
    const auto outcome = coordinator.publish_completion(publication);
    ITF_REQUIRE(outcome.ok());
    ITF_REQUIRE(outcome.value().committed());
    ++served;
  }
  ITF_CHECK_EQ(served, 12);

  // The ceilings still bound the requests that are in progress.
  int admitted = 0;
  StatusCode route_refusal = StatusCode::Ok;
  StatusCode slo_refusal = StatusCode::Ok;
  for (std::uint64_t index = 0; index < 4; ++index) {
    const RequestId request = itf::test::make_request_id(34, index);
    const AttemptId attempt = itf::test::make_attempt_id(34, index);
    ITF_REQUIRE(coordinator.register_request(request).ok());
    ITF_REQUIRE(coordinator.register_attempt(request, attempt, kModelHash).ok());
    const auto route = coordinator.issue_route_decision(request, attempt, 0, false, false);
    const auto slo = coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    if (route.ok() && slo.ok()) {
      ++admitted;
    } else {
      route_refusal = route.code();
      slo_refusal = slo.code();
    }
  }
  ITF_CHECK_EQ(admitted, 3);
  ITF_CHECK_EQ(static_cast<int>(route_refusal), static_cast<int>(StatusCode::CapacityExhausted));
  ITF_CHECK_EQ(static_cast<int>(slo_refusal), static_cast<int>(StatusCode::CapacityExhausted));
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(finished_transfers_release_their_registry_entries) {
  CoordinatorConfig config = Fixture::make_config();
  config.limits.max_transfer_records = 3;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  prepare_coordinator(coordinator);

  // Nine transfers, one after another, each completed before the next is
  // registered. The registry is sized for three transfers in progress.
  int moved = 0;
  for (std::uint64_t index = 0; index < 9; ++index) {
    const TransferRegistration registration = bulk_transfer(35, index, 4096);
    const auto registered = coordinator.register_transfer(registration);
    if (!registered.ok()) {
      std::printf("  transfer %llu: %s\n", static_cast<unsigned long long>(index),
                  registered.status().to_string().c_str());
      break;
    }
    const auto authorization =
        coordinator.authorize_transfer(transfer_message(coordinator, registration));
    ITF_REQUIRE(authorization.ok());
    ITF_REQUIRE(authorization.value().authorized);
    ITF_REQUIRE_STATUS_OK(coordinator.complete_transfer(registration.id, 4096));
    ++moved;
  }
  ITF_CHECK_EQ(moved, 9);
  ITF_CHECK_EQ(coordinator.accounting().transfers_completed, 9ULL);

  // The ceiling still bounds the transfers that are in progress: a transfer
  // that has not finished is never dropped to make room.
  const TransferRegistration kept = bulk_transfer(36, 0, 4096);
  ITF_REQUIRE(coordinator.register_transfer(kept).ok());
  ITF_REQUIRE(coordinator.register_transfer(bulk_transfer(36, 1, 4096)).ok());
  ITF_REQUIRE(coordinator.register_transfer(bulk_transfer(36, 2, 4096)).ok());
  ITF_CHECK_EQ(static_cast<int>(coordinator.register_transfer(bulk_transfer(36, 3, 4096)).code()),
               static_cast<int>(StatusCode::CapacityExhausted));
  const auto authorization = coordinator.authorize_transfer(transfer_message(coordinator, kept));
  ITF_REQUIRE(authorization.ok());
  ITF_CHECK(authorization.value().authorized);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(a_transfer_cannot_be_registered_for_a_finished_request) {
  Fixture fixture;
  const RequestId request = itf::test::make_request_id(37, 1);
  const AttemptId attempt = itf::test::make_attempt_id(37, 1);
  fixture.prepare(request, attempt, ServingStage::HandoffPending);
  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  const auto outcome = fixture.coordinator.publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_REQUIRE(outcome.value().committed());

  // The request is finished and holds no authority. A transfer on its behalf
  // has nothing to inherit.
  TransferRegistration registration;
  registration.id = itf::test::make_transfer_id(37, 1);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = 4096;
  registration.declared_class = TrafficClass::KvTransfer;
  registration.direction = FlowDirection::Lateral;
  registration.request = request;
  registration.attempt = attempt;
  const auto registered = fixture.coordinator.register_transfer(registration);
  ITF_CHECK(!registered.ok());
  if (registered.ok()) {
    // What the registration leads to: a flow the coordinator's own invariant
    // check reports as corruption.
    (void)fixture.coordinator.authorize_transfer(
        transfer_message(fixture.coordinator, registration));
  }
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.verify_invariants());
  ITF_CHECK_EQ(fixture.coordinator.accounting().active_flows_total, 0U);
}


ITF_TEST(a_completed_transfer_stays_completed) {
  Fixture fixture;
  const TransferRegistration registration = bulk_transfer(38, 1, 4096);
  ITF_REQUIRE(fixture.coordinator.register_transfer(registration).ok());
  const TrafficRequest message = transfer_message(fixture.coordinator, registration);
  const auto authorization = fixture.coordinator.authorize_transfer(message);
  ITF_REQUIRE(authorization.ok());
  ITF_REQUIRE(authorization.value().authorized);
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.complete_transfer(registration.id, 4096));

  // An authorization request that arrives again after the transfer completed is
  // answered with a refusal...
  const auto repeated = fixture.coordinator.authorize_transfer(message);
  ITF_REQUIRE(repeated.ok());
  ITF_CHECK(!repeated.value().authorized);
  ITF_CHECK_EQ(repeated.value().reason, ReasonCode::RejectedTransferAlreadyTerminal);
  // ...and does not rewrite how the transfer ended: it is still the completed
  // transfer that cannot be abandoned.
  ITF_CHECK_EQ(static_cast<int>(
                   fixture.coordinator.abandon_transfer(registration.id, ReasonCode::Cancelled)
                       .code()),
               static_cast<int>(StatusCode::Conflict));
  ITF_CHECK_EQ(fixture.coordinator.accounting().transfers_completed, 1ULL);
  ITF_REQUIRE_STATUS_OK(fixture.coordinator.verify_invariants());
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_accounting_closure", argc, argv);
}
