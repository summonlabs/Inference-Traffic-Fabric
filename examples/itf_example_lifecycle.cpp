// Inference Traffic Fabric - example: lifecycle, cancellation and completion.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>

#include "itf/coordinator.hpp"
#include "itf/serialize.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x1111222233334444ULL;
constexpr std::uint64_t kStateHash = 0x5555666677778888ULL;

}  // namespace

int main() {
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  config.limits.ledger.max_retained_terminal_requests = 4;
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
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);

  const RequestId request(0xDEADBEEFCAFEBABEULL, 0x0123456789ABCDEFULL);
  const AttemptId first_attempt(0xA000000000000001ULL, 0x0000000000000001ULL);
  const AttemptId second_attempt(0xA000000000000002ULL, 0x0000000000000002ULL);

  (void)coordinator.register_request(request);
  auto attempt = coordinator.register_attempt(request, first_attempt, kModelHash);
  (void)coordinator.issue_route_decision(request, first_attempt, 0, false, true);
  (void)coordinator.register_slo_contract(request, 40000000ULL, 40000000ULL, 250);
  (void)coordinator.advance_stage(request, first_attempt, ServingStage::Admitted);
  (void)coordinator.advance_stage(request, first_attempt, ServingStage::PrefillQueued);

  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = first_attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::PrefillInput;
  message.declared_stage = ServingStage::PrefillQueued;
  message.direction = FlowDirection::Ingress;
  message.payload_bytes = 1U << 21;
  message.binding.policy = coordinator.policy().generation();
  message.binding.coordinator_epoch = coordinator.epoch();
  message.binding.model_generation = coordinator.model_generation(kModelHash).value();
  message.binding.route_decision =
      coordinator.issue_route_decision(request, first_attempt, 0, false, true).value().generation;
  message.binding.slo_contract =
      coordinator.register_slo_contract(request, 40000000ULL, 40000000ULL, 250).value().generation;
  message.deadline = Instant::from_now(clock.now_nanos(), 40000000);

  const Decision admitted = coordinator.evaluate(message);
  std::printf("admitted decision: %s\n", admitted.to_string().c_str());
  auto flow = coordinator.open_flow(admitted, message.payload_bytes);
  std::printf("flow opened: %s\n", flow.ok() ? "yes" : "no");
  (void)coordinator.record_flow_bytes(flow.value(), message.payload_bytes, true);

  // The prefill worker dies. The attempt is failed and a replacement attempt is
  // bound with a new identity, which fences every decision from the old one.
  auto failed = coordinator.fail_attempt(request, first_attempt, ReasonCode::AttemptFailed);
  std::printf("attempt failure recorded: %s\n", failed.ok() ? "yes" : "no");

  CompletionPublication late;
  late.request = request;
  late.attempt = first_attempt;
  late.attempt_generation = attempt.value();
  late.bytes_in = message.payload_bytes;
  const auto late_outcome = coordinator.publish_completion(late);
  std::printf("late completion for the failed attempt: disposition=%u reason=%s\n",
              static_cast<unsigned>(late_outcome.value().disposition),
              std::string(to_string(late_outcome.value().reason)).c_str());

  // Retry on a fresh attempt with a fresh request generation.
  auto replacement = coordinator.replace_attempt(request, first_attempt, second_attempt, kModelHash);
  std::printf("replacement attempt generation=%llu request generation=%llu\n",
              static_cast<unsigned long long>(replacement.value().value()),
              static_cast<unsigned long long>(
                  coordinator.request_record(request).value().generation.value()));
  std::printf("traffic for the retired attempt: %s\n",
              coordinator.evaluate(message).to_string().c_str());

  auto record = coordinator.request_record(request).value();
  TrafficRequest retry = message;
  retry.subject->attempt = second_attempt;
  retry.subject->request_generation = record.generation;
  retry.declared_stage = ServingStage::Unknown;
  const auto retry_route =
      coordinator.issue_route_decision(request, second_attempt, 0, false, true);
  const auto retry_slo = coordinator.register_slo_contract(request, 40000000ULL, 40000000ULL, 250);
  retry.declared_class = TrafficClass::Control;
  retry.binding.route_decision = retry_route.value().generation;
  retry.binding.slo_contract = retry_slo.value().generation;
  retry.deadline = Instant::from_now(clock.now_nanos(), 40000000);
  const Decision control = coordinator.evaluate(retry);
  std::printf("control traffic on the replacement attempt: %s\n", control.to_string().c_str());

  (void)coordinator.advance_stage(request, second_attempt, ServingStage::Admitted);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::PrefillQueued);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::PrefillRunning);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::HandoffPending);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::DecodeQueued);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::DecodeRunning);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::Streaming);
  (void)coordinator.advance_stage(request, second_attempt, ServingStage::Completing);

  CompletionPublication success;
  success.request = request;
  success.attempt = second_attempt;
  success.attempt_generation = replacement.value();
  success.bytes_in = message.payload_bytes;
  success.bytes_out = 1U << 16;
  const auto outcome = coordinator.publish_completion(success);
  std::printf("successful completion: disposition=%u reason=%s\n",
              static_cast<unsigned>(outcome.value().disposition),
              std::string(to_string(outcome.value().reason)).c_str());
  const auto duplicate = coordinator.publish_completion(success);
  std::printf("duplicate completion: disposition=%u reason=%s\n",
              static_cast<unsigned>(duplicate.value().disposition),
              std::string(to_string(duplicate.value().reason)).c_str());
  std::printf("accounting: %s\n", coordinator.accounting().to_string().c_str());
  std::printf("invariants: %s\n", coordinator.verify_invariants().to_string().c_str());
  std::printf("request explanation:\n%s", coordinator.explain_request(request).c_str());
  return 0;
}
