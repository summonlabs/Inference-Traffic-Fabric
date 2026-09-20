// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <string>

#include "itf/coordinator.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0xC0FFEEC0FFEEC0FFULL;

struct Harness {
  VirtualClock clock;
  Coordinator coordinator;

  explicit Harness(std::uint32_t retained) : coordinator(make_config(retained), &clock) {
    (void)coordinator.boot_fresh();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Synthetic;
    topology.node_count = 2;
    topology.link_count = 1;
    topology.fabric_bytes_per_sec = 100000000000ULL;
    topology.max_age_nanos = 3600000000000ULL;
    (void)coordinator.publish_topology(topology);
    (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  }

  static CoordinatorConfig make_config(std::uint32_t retained) {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.limits.ledger.max_retained_terminal_requests = retained;
    config.limits.ledger.max_live_requests = 200000;
    config.boot_seed = 5;
    return config;
  }
};

/// Runs a full request lifecycle and returns the elapsed nanoseconds.
std::int64_t run_batch(Harness& harness, std::uint64_t seed, std::uint64_t count,
                       std::uint64_t offset) {
  const auto started = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index < count; ++index) {
    const RequestId request = itf::test::make_request_id(seed, offset + index);
    const AttemptId attempt = itf::test::make_attempt_id(seed, offset + index);
    (void)harness.coordinator.register_request(request);
    (void)harness.coordinator.register_attempt(request, attempt, kModelHash);
    (void)harness.coordinator.issue_route_decision(request, attempt, 0, false, false);
    (void)harness.coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    (void)harness.coordinator.advance_stage(request, attempt, ServingStage::Admitted);

    TrafficRequest message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    message.subject = subject;
    message.declared_class = TrafficClass::RequestIngress;
    message.declared_stage = ServingStage::Admitted;
    message.payload_bytes = 2048;
    message.binding.policy = harness.coordinator.policy().generation();
    message.binding.coordinator_epoch = harness.coordinator.epoch();
    message.binding.model_generation = ModelGeneration::from(1);
    message.binding.route_decision = RouteDecisionGeneration::from(1);
    message.binding.slo_contract = SloContractGeneration::from(1);
    message.deadline = Instant::from_now(harness.clock.now_nanos(), 20000000);
    const Decision decision = harness.coordinator.evaluate(message);
    if (decision.permits_traffic()) {
      const auto flow = harness.coordinator.open_flow(decision, message.payload_bytes);
      if (flow.ok()) {
        (void)harness.coordinator.record_flow_bytes(flow.value(), message.payload_bytes, true);
      }
    }
    CompletionPublication publication;
    publication.request = request;
    publication.attempt = attempt;
    publication.attempt_generation = AttemptGeneration::from(1);
    publication.bytes_in = message.payload_bytes;
    (void)harness.coordinator.publish_completion(publication);
    harness.clock.advance(1000);
  }
  const auto finished = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
}

ITF_TEST(keyed_lookups_do_not_degrade_quadratically) {
  Harness harness(4096);
  constexpr std::uint64_t kSmall = 2000;
  constexpr std::uint64_t kLarge = 20000;

  // Warm the allocator and the hash tables so the small batch is not paying
  // one-off setup costs.
  (void)run_batch(harness, 41, 500, 0);
  const std::int64_t small = run_batch(harness, 41, kSmall, 1000);
  const std::int64_t large = run_batch(harness, 42, kLarge, 1000);

  const double small_per_op = static_cast<double>(small) / static_cast<double>(kSmall);
  const double large_per_op = static_cast<double>(large) / static_cast<double>(kLarge);
  const double ratio = large_per_op / (small_per_op > 0 ? small_per_op : 1.0);
  std::printf("  scale: small_per_op=%.0fns large_per_op=%.0fns ratio=%.2f\n", small_per_op,
              large_per_op, ratio);
  // A quadratic implementation would show a ratio near ten; the ceiling is
  // deliberately generous so that a loaded machine does not produce noise.
  ITF_CHECK(ratio < 6.0);

  const AccountingSnapshot snapshot = harness.coordinator.accounting();
  ITF_CHECK_EQ(snapshot.requests_registered, 500 + kSmall + kLarge + 0ULL);
  ITF_CHECK(snapshot.closed());
  ITF_REQUIRE_STATUS_OK(harness.coordinator.verify_invariants());
}

ITF_TEST(terminal_history_stays_bounded_at_scale) {
  Harness harness(1024);
  (void)run_batch(harness, 43, 8000, 0);
  const AccountingSnapshot snapshot = harness.coordinator.accounting();
  ITF_CHECK_EQ(snapshot.retained_requests, static_cast<std::size_t>(1024));
  ITF_CHECK(snapshot.requests_evicted >= 6000ULL);
  ITF_CHECK_EQ(snapshot.live_requests, static_cast<std::size_t>(0));
  ITF_REQUIRE_STATUS_OK(harness.coordinator.verify_invariants());
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_scale_indexes", argc, argv); }
