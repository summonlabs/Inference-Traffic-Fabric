// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "itf/coordinator.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0xC0C0C0C0C0C0C0C0ULL;
constexpr std::uint64_t kStateHash = 0xD0D0D0D0D0D0D0D0ULL;

ITF_TEST(concurrent_cancellation_always_wins) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  config.boot_seed = 13;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);

  constexpr int kRequests = 64;
  constexpr int kRounds = 16;
  std::vector<RequestId> requests;
  std::vector<AttemptId> attempts;
  for (int index = 0; index < kRequests; ++index) {
    const RequestId request = itf::test::make_request_id(61, static_cast<std::uint64_t>(index));
    const AttemptId attempt = itf::test::make_attempt_id(61, static_cast<std::uint64_t>(index));
    ITF_REQUIRE(coordinator.register_request(request).ok());
    ITF_REQUIRE(coordinator.register_attempt(request, attempt, kModelHash).ok());
    ITF_REQUIRE(coordinator.issue_route_decision(request, attempt, 0, false, false).ok());
    ITF_REQUIRE(coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200).ok());
    ITF_REQUIRE(coordinator.advance_stage(request, attempt, ServingStage::Admitted).ok());
    requests.push_back(request);
    attempts.push_back(attempt);
  }

  std::atomic<bool> start{false};
  std::atomic<int> successful_completions{0};
  std::atomic<int> refusals{0};
  std::atomic<int> admissions_after_cancel{0};

  const auto make_message = [&](int index) {
    TrafficRequest message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = requests[static_cast<std::size_t>(index)];
    subject.attempt = attempts[static_cast<std::size_t>(index)];
    subject.request_generation = RequestGeneration::from(1);
    message.subject = subject;
    message.declared_class = TrafficClass::RequestIngress;
    message.declared_stage = ServingStage::Admitted;
    message.payload_bytes = 256;
    message.binding.policy = coordinator.policy().generation();
    message.binding.coordinator_epoch = coordinator.epoch();
    message.binding.model_generation = ModelGeneration::from(1);
    message.binding.route_decision = RouteDecisionGeneration::from(1);
    message.binding.slo_contract = SloContractGeneration::from(1);
    message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    return message;
  };

  // Phase one: concurrent cancellation.
  std::vector<std::thread> cancellers;
  for (int canceller = 0; canceller < 4; ++canceller) {
    cancellers.emplace_back([&, canceller] {
      while (!start.load()) {
      }
      for (int index = canceller; index < kRequests; index += 4) {
        (void)coordinator.cancel_request(requests[static_cast<std::size_t>(index)],
                                         ReasonCode::Cancelled);
        std::this_thread::yield();
      }
    });
  }
  start.store(true);
  for (std::thread& canceller : cancellers) canceller.join();

  // Phase two: every thread tries to regain network authority for a request
  // that has already been cancelled. None may succeed.
  std::atomic<bool> second_phase{false};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      while (!second_phase.load()) {
      }
      for (int round = 0; round < kRounds; ++round) {
        for (int index = worker; index < kRequests; index += 4) {
          const Decision decision = coordinator.evaluate(make_message(index));
          if (decision.permits_traffic()) {
            admissions_after_cancel.fetch_add(1);
            const auto flow = coordinator.open_flow(decision, 256);
            if (flow.ok()) {
              (void)coordinator.record_flow_bytes(flow.value(), 256, true);
            }
          }
          CompletionPublication publication;
          publication.request = requests[static_cast<std::size_t>(index)];
          publication.attempt = attempts[static_cast<std::size_t>(index)];
          publication.attempt_generation = AttemptGeneration::from(1);
          publication.bytes_in = 256;
          const auto outcome = coordinator.publish_completion(publication);
          if (outcome.ok()) {
            if (outcome.value().committed()) successful_completions.fetch_add(1);
            if (outcome.value().refused()) refusals.fetch_add(1);
          }
        }
      }
    });
  }
  second_phase.store(true);
  for (std::thread& worker : workers) worker.join();
  ITF_CHECK_EQ(successful_completions.load(), 0);
  ITF_CHECK(refusals.load() > 0);

  // Closure invariant: no cancelled request ever regained network authority.
  ITF_CHECK_EQ(admissions_after_cancel.load(), 0);
  int cancelled = 0;
  int completed = 0;
  for (int index = 0; index < kRequests; ++index) {
    const auto record = coordinator.request_record(requests[static_cast<std::size_t>(index)]);
    ITF_REQUIRE(record.has_value());
    if (record->lifecycle == RequestLifecycle::Cancelled) {
      ++cancelled;
      ITF_CHECK_EQ(record->active_flows, 0U);
      // A cancelled request refuses every later success publication.
      CompletionPublication late;
      late.request = requests[static_cast<std::size_t>(index)];
      late.attempt = record->current_attempt;
      const auto outcome = coordinator.publish_completion(late);
      if (outcome.ok()) ITF_CHECK(outcome.value().refused());
    } else {
      ITF_CHECK_EQ(static_cast<int>(record->lifecycle),
                   static_cast<int>(RequestLifecycle::Completed));
      ++completed;
    }
    ITF_CHECK_EQ(record->active_flows, 0U);
  }
  std::printf("  cancellation race: cancelled=%d completed=%d\n", cancelled, completed);
  ITF_CHECK(cancelled > 0);
  const AccountingSnapshot snapshot = coordinator.accounting();
  ITF_CHECK_EQ(snapshot.active_flows_total, 0U);
  ITF_CHECK(snapshot.closed());
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(concurrent_policy_and_evidence_publication_is_safe) {
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  config.boot_seed = 17;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);

  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      while (!start.load()) {
      }
      for (int round = 0; round < 200; ++round) {
        TopologyEvidence topology;
        topology.provenance = EvidenceProvenance::Synthetic;
        topology.node_count = static_cast<std::uint32_t>(1 + (round % 4));
        topology.link_count = 1;
        topology.fabric_bytes_per_sec = 1000000000ULL * static_cast<std::uint64_t>(1 + round % 8);
        topology.max_age_nanos = 1000000000000ULL;
        (void)coordinator.publish_topology(topology);
        PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
        policy.set_congestion_queue_depth(static_cast<std::uint32_t>(1 + worker * 8 + round % 16));
        (void)coordinator.apply_policy(policy);
        (void)coordinator.status();
        (void)coordinator.accounting();
        (void)coordinator.verify_invariants();
      }
    });
  }
  start.store(true);
  for (std::thread& worker : workers) worker.join();
  ITF_CHECK(!coordinator.policy().generation().is_unset());
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(repeated_lifecycle_start_stop_is_clean) {
  for (int cycle = 0; cycle < 40; ++cycle) {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.boot_seed = static_cast<std::uint64_t>(cycle) + 1;
    VirtualClock clock;
    Coordinator coordinator(config, &clock);
    ITF_REQUIRE(coordinator.boot_fresh().ok());
    const RequestId request =
        itf::test::make_request_id(62, static_cast<std::uint64_t>(cycle));
    ITF_REQUIRE(coordinator.register_request(request).ok());
    ITF_REQUIRE(coordinator.cancel_request(request, ReasonCode::Cancelled).ok());
    ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
    ITF_CHECK(coordinator.accounting().closed());
  }
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_concurrency_authority", argc, argv);
}
