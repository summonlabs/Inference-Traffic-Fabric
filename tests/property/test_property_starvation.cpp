// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <random>
#include <string>
#include <vector>

#include "itf/coordinator.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x5741A7E5741A7E57ULL;
constexpr std::uint64_t kStateHash = 0x0B1E0B1E0B1E0B1EULL;

/// Under sustained bulk pressure a starvation-protected flow must still be
/// admitted within its configured deferral budget.
void run_pressure(std::uint64_t seed, std::uint32_t max_defer_count, int rounds) {
  std::mt19937_64 random(seed);
  CoordinatorConfig config;
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  streaming.max_defer_count = max_defer_count;
  streaming.max_defer_horizon_nanos = 1000000000ULL;
  (void)policy.set_rule(TrafficClass::DecodeStream, streaming);
  ClassPolicyRule egress = policy.rule(TrafficClass::ResponseEgress);
  egress.reserved_weight = 0;
  egress.max_defer_count = max_defer_count;
  egress.max_defer_horizon_nanos = 1000000000ULL;
  (void)policy.set_rule(TrafficClass::ResponseEgress, egress);
  policy.set_congestion_queue_depth(1);
  config.policy = policy;
  config.boot_seed = seed;

  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.queue_depth = 128;
  topology.congestion_signalled = true;
  topology.fabric_bytes_per_sec = 1000000000ULL;
  topology.max_age_nanos = 1000000000000ULL;
  (void)coordinator.publish_topology(topology);
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);

  const RequestId request = itf::test::make_request_id(seed, 1);
  const AttemptId attempt = itf::test::make_attempt_id(seed, 1);
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

  TrafficRequest streaming_message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = attempt;
  subject.request_generation = RequestGeneration::from(1);
  streaming_message.subject = subject;
  streaming_message.declared_class = TrafficClass::DecodeStream;
  streaming_message.declared_stage = ServingStage::Streaming;
  streaming_message.payload_bytes = 8192;
  streaming_message.binding.policy = coordinator.policy().generation();
  streaming_message.binding.coordinator_epoch = coordinator.epoch();
  streaming_message.binding.route_decision = RouteDecisionGeneration::from(1);
  streaming_message.binding.slo_contract = SloContractGeneration::from(1);
  streaming_message.binding.model_generation = ModelGeneration::from(1);

  TransferRegistration registration;
  registration.id = itf::test::make_transfer_id(seed, 1);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = 32U << 20;
  registration.declared_class = TrafficClass::ModelTransfer;
  registration.direction = FlowDirection::Lateral;
  ITF_REQUIRE(coordinator.register_transfer(registration).ok());

  TrafficRequest bulk_message;
  TrafficSubject bulk_subject;
  bulk_subject.kind = SubjectKind::Transfer;
  bulk_subject.transfer = registration.id;
  bulk_message.subject = bulk_subject;
  bulk_message.declared_class = TrafficClass::ModelTransfer;
  bulk_message.direction = FlowDirection::Lateral;
  bulk_message.payload_bytes = registration.payload_bytes;
  bulk_message.binding.policy = coordinator.policy().generation();
  bulk_message.binding.coordinator_epoch = coordinator.epoch();
  bulk_message.binding.model_generation = ModelGeneration::from(1);

  std::uint32_t defers = 0;
  bool admitted = false;
  for (int round = 0; round < rounds && !admitted; ++round) {
    const Decision bulk = coordinator.evaluate(bulk_message);
    // Bulk movement never competes with latency-critical traffic.
    ITF_CHECK(bulk.outcome == DecisionOutcome::Deferred ||
              bulk.outcome == DecisionOutcome::Rejected);

    const Decision decision = coordinator.evaluate(streaming_message);
    if (decision.permits_traffic()) {
      admitted = true;
      ITF_CHECK(decision.reason == ReasonCode::AdmittedStarvationGuard ||
                decision.reason == ReasonCode::Admitted);
    } else {
      ITF_CHECK_EQ(decision.outcome, DecisionOutcome::Deferred);
      ++defers;
    }
    clock.advance(static_cast<std::int64_t>(random() % 100000) + 1000);
  }
  if (!admitted) {
    std::printf("  starvation guard never fired (seed %llu, defers %u)\n",
                static_cast<unsigned long long>(seed), defers);
  }
  ITF_CHECK(admitted);
  ITF_CHECK(defers + 1 <= max_defer_count + 1);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(streaming_is_never_starved_by_bulk_movement) {
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    run_pressure(itf::test::seed() + seed * 104729ULL, 4, 64);
  }
}

ITF_TEST(the_guard_scales_with_the_configured_budget) {
  run_pressure(itf::test::seed() * 7ULL + 3ULL, 12, 64);
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_property_starvation", argc, argv);
}
