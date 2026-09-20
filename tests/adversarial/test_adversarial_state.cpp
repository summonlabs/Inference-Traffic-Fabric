// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <random>
#include <string>
#include <vector>

#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0xDEAD10CCDEAD10CCULL;
constexpr std::uint64_t kStateHash = 0xBADC0DE0BADC0DE0ULL;

struct Harness {
  VirtualClock clock;
  Coordinator coordinator;
  RequestId request = itf::test::make_request_id(71, 1);
  AttemptId attempt = itf::test::make_attempt_id(71, 1);

  Harness() : coordinator(make_config(), &clock) {
    (void)coordinator.boot_fresh();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Synthetic;
    topology.node_count = 2;
    topology.link_count = 1;
    topology.fabric_bytes_per_sec = 1000000000ULL;
    topology.max_age_nanos = 1000000000ULL;
    (void)coordinator.publish_topology(topology);
    (void)coordinator.set_model_generation(kModelHash, std::nullopt);
    (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
    (void)coordinator.register_request(request);
    (void)coordinator.register_attempt(request, attempt, kModelHash);
    (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
    (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    (void)coordinator.advance_stage(request, attempt, ServingStage::Admitted);
  }

  static CoordinatorConfig make_config() {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.boot_seed = 21;
    return config;
  }

  TrafficRequest message() const {
    TrafficRequest message_out;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    message_out.subject = subject;
    message_out.declared_class = TrafficClass::RequestIngress;
    message_out.declared_stage = ServingStage::Admitted;
    message_out.payload_bytes = 1024;
    message_out.binding.policy = coordinator.policy().generation();
    message_out.binding.coordinator_epoch = coordinator.epoch();
    message_out.binding.model_generation = ModelGeneration::from(1);
    message_out.binding.route_decision = RouteDecisionGeneration::from(1);
    message_out.binding.slo_contract = SloContractGeneration::from(1);
    message_out.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    return message_out;
  }
};

ITF_TEST(nil_and_unknown_identities_are_refused) {
  Harness harness;
  ITF_CHECK_EQ(static_cast<int>(harness.coordinator.register_request(RequestId()).code()),
               static_cast<int>(StatusCode::InvalidArgument));
  ITF_CHECK_EQ(static_cast<int>(harness.coordinator.register_attempt(RequestId(), AttemptId(), 0).code()),
               static_cast<int>(StatusCode::InvalidArgument));
  ITF_CHECK_EQ(
      static_cast<int>(harness.coordinator.cancel_request(RequestId(), ReasonCode::Cancelled).code()),
      static_cast<int>(StatusCode::NotFound));
  ITF_CHECK_EQ(static_cast<int>(harness.coordinator.register_slo_contract(
                                    RequestId(), 1000, 1000, 1)
                                    .code()),
               static_cast<int>(StatusCode::InvalidArgument));
}

ITF_TEST(contradictory_metadata_is_refused) {
  TrafficRequest contradictory;
  contradictory.declared_class = TrafficClass::ModelTransfer;
  contradictory.deadline = Instant::at(1000);
  contradictory.deadline_after_nanos = 5000;
  ITF_CHECK(!contradictory.structurally_valid());

  Harness harness;
  const Decision decision = harness.coordinator.evaluate(contradictory);
  ITF_CHECK_EQ(decision.reason, ReasonCode::RejectedSubjectKindMismatch);
}

ITF_TEST(transfer_registration_bounds_are_enforced) {
  Harness harness;
  TransferRegistration registration;
  registration.id = itf::test::make_transfer_id(72, 1);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.declared_class = TrafficClass::KvTransfer;
  registration.payload_bytes = (1ULL << 40) + 1;
  ITF_CHECK_EQ(
      static_cast<int>(harness.coordinator.register_transfer(registration).code()),
      static_cast<int>(StatusCode::BoundsExceeded));

  registration.payload_bytes = 1024;
  registration.declared_class = TrafficClass::DecodeStream;
  ITF_CHECK_EQ(
      static_cast<int>(harness.coordinator.register_transfer(registration).code()),
      static_cast<int>(StatusCode::InvalidArgument));

  registration.declared_class = TrafficClass::KvTransfer;
  registration.model_hash = 0;
  ITF_CHECK_EQ(
      static_cast<int>(harness.coordinator.register_transfer(registration).code()),
      static_cast<int>(StatusCode::NotFound));

  registration.model_hash = kModelHash;
  ITF_CHECK(harness.coordinator.register_transfer(registration).ok());
  ITF_CHECK_EQ(
      static_cast<int>(harness.coordinator.register_transfer(registration).code()),
      static_cast<int>(StatusCode::AlreadyExists));
}

ITF_TEST(decision_replay_cannot_open_a_second_flow_after_completion) {
  Harness harness;
  const TrafficRequest message = harness.message();
  const Decision decision = harness.coordinator.evaluate(message);
  ITF_REQUIRE(decision.permits_traffic());
  const auto first = harness.coordinator.open_flow(decision, message.payload_bytes);
  ITF_REQUIRE(first.ok());

  CompletionPublication publication;
  publication.request = harness.request;
  publication.attempt = harness.attempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.bytes_in = message.payload_bytes;
  const auto outcome = harness.coordinator.publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().committed());

  const auto replayed = harness.coordinator.open_flow(decision, message.payload_bytes);
  ITF_CHECK(!replayed.ok());
  ITF_CHECK_EQ(harness.coordinator.accounting().active_flows_total, 0U);
  ITF_REQUIRE_STATUS_OK(harness.coordinator.verify_invariants());
}

ITF_TEST(stale_decisions_are_refused_after_cancellation) {
  Harness harness;
  const TrafficRequest message = harness.message();
  const Decision decision = harness.coordinator.evaluate(message);
  ITF_REQUIRE(decision.permits_traffic());
  ITF_REQUIRE(harness.coordinator.cancel_request(harness.request, ReasonCode::Cancelled).ok());
  const auto flow = harness.coordinator.open_flow(decision, message.payload_bytes);
  ITF_CHECK(!flow.ok());
  ITF_CHECK_EQ(static_cast<int>(flow.code()), static_cast<int>(StatusCode::Cancelled));
  ITF_REQUIRE_STATUS_OK(harness.coordinator.verify_invariants());
}

ITF_TEST(deferral_records_are_bounded) {
  CoordinatorConfig config = Harness::make_config();
  PolicyConfig policy = config.policy;
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  streaming.max_defer_count = 1000;
  streaming.max_defer_horizon_nanos = 1000000000ULL;
  (void)policy.set_rule(TrafficClass::DecodeStream, streaming);
  policy.set_congestion_queue_depth(1);
  config.policy = policy;
  config.limits.max_defer_records = 8;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.queue_depth = 4096;
  topology.congestion_signalled = true;
  topology.fabric_bytes_per_sec = 1000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);

  for (int index = 0; index < 200; ++index) {
    const RequestId request = itf::test::make_request_id(73, static_cast<std::uint64_t>(index));
    const AttemptId attempt = itf::test::make_attempt_id(73, static_cast<std::uint64_t>(index));
    (void)coordinator.register_request(request);
    (void)coordinator.register_attempt(request, attempt, 0);
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
    (void)coordinator.evaluate(message);
  }
  ITF_CHECK(coordinator.accounting().pending_deferrals <= 8U);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(global_flow_capacity_is_enforced) {
  CoordinatorConfig config = Harness::make_config();
  config.limits.ledger.max_active_flows_total = 3;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 1;
  topology.link_count = 0;
  topology.fabric_bytes_per_sec = 1000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);

  int opened = 0;
  int refused = 0;
  for (int index = 0; index < 8; ++index) {
    const RequestId request = itf::test::make_request_id(74, static_cast<std::uint64_t>(index));
    const AttemptId attempt = itf::test::make_attempt_id(74, static_cast<std::uint64_t>(index));
    (void)coordinator.register_request(request);
    (void)coordinator.register_attempt(request, attempt, 0);
    (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
    (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    (void)coordinator.advance_stage(request, attempt, ServingStage::Admitted);
    TrafficRequest message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    message.subject = subject;
    message.declared_class = TrafficClass::RequestIngress;
    message.declared_stage = ServingStage::Admitted;
    message.payload_bytes = 128;
    message.binding.policy = coordinator.policy().generation();
    message.binding.coordinator_epoch = coordinator.epoch();
    message.binding.route_decision = RouteDecisionGeneration::from(1);
    message.binding.slo_contract = SloContractGeneration::from(1);
    const Decision decision = coordinator.evaluate(message);
    ITF_REQUIRE(decision.permits_traffic());
    const auto flow = coordinator.open_flow(decision, message.payload_bytes);
    if (flow.ok()) {
      ++opened;
    } else {
      ++refused;
      ITF_CHECK_EQ(static_cast<int>(flow.code()), static_cast<int>(StatusCode::CapacityExhausted));
    }
  }
  ITF_CHECK_EQ(opened, 3);
  ITF_CHECK_EQ(refused, 5);
  ITF_CHECK_EQ(coordinator.accounting().active_flows_total, 3U);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(corrupt_durable_documents_are_refused) {
  const std::string directory = itf::test::make_temp_dir("adversarial-state");
  const std::string path = directory + "/coordinator.bin";
  VirtualClock clock;
  CoordinatorConfig config = Harness::make_config();
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.register_request(itf::test::make_request_id(75, 1));
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());

  SnapshotStore store(path);
  ITF_REQUIRE_STATUS_OK(store.save(state.value()));

  std::mt19937_64 random(0x1234ULL);
  int accepted = 0;
  for (int round = 0; round < 300; ++round) {
    SnapshotStore scratch(path);
    ITF_REQUIRE_STATUS_OK(scratch.save(state.value()));
    const std::uint64_t size = itf::test::file_size(path);
    ITF_REQUIRE(size > 0);
    const std::uint64_t offset = random() % size;
    itf::test::flip_byte(path, offset);
    const auto loaded = scratch.load();
    if (loaded.ok()) ++accepted;
  }
  // A flip inside an opaque hash field can still decode; a flip anywhere in a
  // structural field must not.
  ITF_CHECK(accepted < 30);
  itf::test::remove_dir(directory);
}

ITF_TEST(oversized_snapshot_payloads_are_refused_before_allocation) {
  const std::string directory = itf::test::make_temp_dir("adversarial-oversize");
  const std::string path = directory + "/coordinator.bin";
  SnapshotStore store(path, 1024);
  std::vector<std::uint8_t> payload(4096, 0x41);
  ITF_CHECK_EQ(static_cast<int>(store.save_raw(ByteSpan(payload.data(), payload.size())).code()),
               static_cast<int>(StatusCode::BoundsExceeded));
  std::vector<std::uint8_t> small(16, 0x41);
  ITF_REQUIRE_STATUS_OK(store.save_raw(ByteSpan(small.data(), small.size())));
  const auto loaded = store.load();
  ITF_CHECK(!loaded.ok());
  itf::test::remove_dir(directory);
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_adversarial_state", argc, argv); }
