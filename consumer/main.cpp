// Inference Traffic Fabric - downstream consumer using find_package.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <cstring>
#include <string>

#include "itf/accel.hpp"
#include "itf/client.hpp"
#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "itf/serialize.hpp"
#include "itf/version.hpp"

int main() {
  using namespace itf;
  std::printf("Inference Traffic Fabric %.*s downstream consumer\n",
              static_cast<int>(kVersionString.size()), kVersionString.data());

  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();

  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Measured;
  topology.node_count = 1;
  topology.link_count = 0;
  topology.accelerator_count = 1;
  topology.fabric_bytes_per_sec = 25000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);

  constexpr std::uint64_t kModelHash = 0xABCDEF0011223344ULL;
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  const RequestId request(0x1111222233334444ULL, 0x5555666677778888ULL);
  const AttemptId attempt(0x9999AAAABBBBCCCCULL, 0xDDDDEEEEFFFF0000ULL);
  (void)coordinator.register_request(request);
  (void)coordinator.register_attempt(request, attempt, kModelHash);
  (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
  (void)coordinator.register_slo_contract(request, 30000000ULL, 30000000ULL, 200);
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
  message.payload_bytes = 4096;
  message.binding.policy = coordinator.policy().generation();
  message.binding.coordinator_epoch = coordinator.epoch();
  message.binding.route_decision = coordinator.issue_route_decision(request, attempt, 0, false, false)
                                       .value()
                                       .generation;
  message.binding.slo_contract =
      coordinator.register_slo_contract(request, 30000000ULL, 30000000ULL, 200).value().generation;
  message.deadline = Instant::from_now(clock.now_nanos(), 30000000);

  const Decision decision = coordinator.evaluate(message);
  std::printf("decision: %s\n", decision.to_string().c_str());
  const auto flow = coordinator.open_flow(decision, message.payload_bytes);
  if (!flow.ok()) {
    std::printf("flow refused: %s\n", flow.status().to_string().c_str());
    return 1;
  }
  (void)coordinator.record_flow_bytes(flow.value(), message.payload_bytes, true);

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.bytes_in = message.payload_bytes;
  const auto outcome = coordinator.publish_completion(publication);
  std::printf("completion: disposition=%u reason=%s\n",
              static_cast<unsigned>(outcome.value().disposition),
              std::string(to_string(outcome.value().reason)).c_str());
  const Status invariants = coordinator.verify_invariants();
  std::printf("invariants: %s\n", invariants.to_string().c_str());

  SnapshotStore store("consumer-snapshot.bin");
  auto state = coordinator.export_state();
  const Status saved = store.save(state.value());
  std::printf("snapshot save: %s\n", saved.to_string().c_str());
  auto loaded = store.load();
  std::printf("snapshot load: %s\n",
              loaded.ok() ? loaded.value().to_string().c_str()
                          : loaded.status().to_string().c_str());
  (void)std::remove("consumer-snapshot.bin");

  const auto probe = accel::probe(false);
  std::printf("accelerator label: %s\n", std::string(to_string(probe.value().label)).c_str());
  return invariants.ok() && saved.ok() && loaded.ok() ? 0 : 1;
}
