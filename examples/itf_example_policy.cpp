// Inference Traffic Fabric - example: deterministic policy decisions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>

#include "itf/coordinator.hpp"
#include "itf/serialize.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0xABCDEF0123456789ULL;
constexpr std::uint64_t kStateHash = 0x0F1E2D3C4B5A6978ULL;

void print_decision(const char* label, const Decision& decision) {
  std::printf("%-28s %s\n", label, decision.to_string().c_str());
}

}  // namespace

int main() {
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();

  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.accelerator_count = 2;
  topology.fabric_bytes_per_sec = 100ULL * 1000ULL * 1000ULL * 1000ULL;
  topology.max_age_nanos = 5000000000ULL;
  topology.disaggregated = true;
  const auto topology_generation = coordinator.publish_topology(topology);
  std::printf("topology generation=%llu\n",
              static_cast<unsigned long long>(topology_generation.value().value()));

  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);

  const RequestId request(0x0011223344556677ULL, 0x8899AABBCCDDEEFFULL);
  const AttemptId attempt(0x0102030405060708ULL, 0x090A0B0C0D0E0F10ULL);
  (void)coordinator.register_request(request);
  (void)coordinator.register_attempt(request, attempt, kModelHash);
  (void)coordinator.issue_route_decision(request, attempt, 1, false, true);
  (void)coordinator.register_slo_contract(request, 25000000ULL, 25000000ULL, 200);

  const auto make_request = [&](TrafficClass traffic, ServingStage stage, std::uint64_t bytes) {
    TrafficRequest message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    message.subject = subject;
    message.declared_class = traffic;
    message.declared_stage = stage;
    message.direction = FlowDirection::Lateral;
    message.payload_bytes = bytes;
    message.model_hash = kModelHash;
    message.binding.policy = coordinator.policy().generation();
    message.binding.coordinator_epoch = coordinator.epoch();
    message.binding.model_generation = coordinator.model_generation(kModelHash).value();
    message.binding.state_generation = coordinator.state_generation(kStateHash).value();
    message.deadline = Instant::from_now(clock.now_nanos(), 25000000);
    return message;
  };

  print_decision("before any stage:", coordinator.evaluate(
                                        make_request(TrafficClass::RequestIngress,
                                                     ServingStage::Unknown, 4096)));

  (void)coordinator.advance_stage(request, attempt, ServingStage::Admitted);
  auto ingress = make_request(TrafficClass::RequestIngress, ServingStage::Admitted, 4096);
  ingress.binding.route_decision = coordinator.issue_route_decision(request, attempt, 1, false, true)
                                       .value()
                                       .generation;
  ingress.binding.slo_contract =
      coordinator.register_slo_contract(request, 25000000ULL, 25000000ULL, 200).value().generation;
  print_decision("ingress admitted:", coordinator.evaluate(ingress));

  auto wrong_stage = ingress;
  wrong_stage.declared_stage = ServingStage::PrefillRunning;
  print_decision("stage claim overridden:", coordinator.evaluate(wrong_stage));

  auto wrong_class = ingress;
  wrong_class.declared_class = TrafficClass::DecodeStream;
  print_decision("class re-derived:", coordinator.evaluate(wrong_class));

  auto stale_policy = ingress;
  stale_policy.binding.policy = PolicyGeneration::from(42);
  print_decision("stale policy binding:", coordinator.evaluate(stale_policy));

  auto stale_model = ingress;
  stale_model.declared_class = TrafficClass::PrefillInput;
  stale_model.binding.model_generation = ModelGeneration::from(9);
  print_decision("stale model generation:", coordinator.evaluate(stale_model));

  auto expired = ingress;
  expired.deadline = Instant::at(clock.now_nanos() - 1);
  print_decision("deadline expired:", coordinator.evaluate(expired));

  std::printf("\nInvariants: %s\n", coordinator.verify_invariants().to_string().c_str());
  std::printf("Accounting: %s\n", coordinator.accounting().to_string().c_str());
  return 0;
}
