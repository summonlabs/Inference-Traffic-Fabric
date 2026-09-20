// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "itf/coordinator.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x5EED5EED5EED5EEDULL;
constexpr std::uint64_t kStateHash = 0xFACE0FF1CE0FF1CEULL;

struct Harness {
  VirtualClock clock;
  Coordinator coordinator;

  Harness() : coordinator(make_config(), &clock) {
    (void)coordinator.boot_fresh();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Synthetic;
    topology.node_count = 2;
    topology.link_count = 1;
    topology.accelerator_count = 2;
    topology.fabric_bytes_per_sec = 100000000000ULL;
    topology.max_age_nanos = 3600000000000ULL;
    topology.disaggregated = true;
    (void)coordinator.publish_topology(topology);
    (void)coordinator.set_model_generation(kModelHash, std::nullopt);
    (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
  }

  static CoordinatorConfig make_config() {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.boot_seed = 1;
    return config;
  }

  RequestId request = itf::test::make_request_id(51, 1);
  AttemptId attempt = itf::test::make_attempt_id(51, 1);
  std::uint64_t counter = 0;
  std::vector<RequestId> cancelled;
};

/// Runs a seeded randomized operation sequence and asserts the invariants after
/// every step. On failure the seed printed by the harness reproduces the run.
void run_sequence(std::uint64_t seed, int steps) {
  std::mt19937_64 random(seed);
  Harness harness;
  std::vector<RequestId> live;
  std::vector<RequestId> cancelled;

  for (int step = 0; step < steps; ++step) {
    const std::uint64_t choice = random() % 100;
    if (choice < 18 || live.empty()) {
      const RequestId request = itf::test::make_request_id(seed, static_cast<std::uint64_t>(step));
      const AttemptId attempt = itf::test::make_attempt_id(seed, static_cast<std::uint64_t>(step));
      if (harness.coordinator.register_request(request).ok()) {
        live.push_back(request);
        (void)harness.coordinator.register_attempt(request, attempt, kModelHash);
        (void)harness.coordinator.issue_route_decision(request, attempt, 0, false, false);
        (void)harness.coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
      }
    } else {
      const RequestId request = live[random() % live.size()];
      const auto record = harness.coordinator.request_record(request);
      if (!record.has_value()) continue;
      const AttemptId attempt = record->current_attempt;
      switch (choice % 8) {
        case 0:
          (void)harness.coordinator.advance_stage(request, attempt, ServingStage::Admitted);
          break;
        case 1:
          (void)harness.coordinator.advance_stage(request, attempt, ServingStage::PrefillQueued);
          break;
        case 2: {
          TrafficRequest message;
          TrafficSubject subject;
          subject.kind = SubjectKind::Serving;
          subject.request = request;
          subject.attempt = attempt;
          subject.request_generation = record->generation;
          message.subject = subject;
          message.declared_class = TrafficClass::RequestIngress;
          message.declared_stage = ServingStage::Admitted;
          message.payload_bytes = 1024;
          message.binding.policy = harness.coordinator.policy().generation();
          message.binding.coordinator_epoch = harness.coordinator.epoch();
          const Decision decision = harness.coordinator.evaluate(message);
          if (decision.permits_traffic()) {
            const auto flow = harness.coordinator.open_flow(decision, message.payload_bytes);
            if (flow.ok()) {
              (void)harness.coordinator.record_flow_bytes(flow.value(), 512, true);
              if ((random() % 3) == 0) (void)harness.coordinator.release_flow(flow.value());
            }
          }
          break;
        }
        case 3:
          if (harness.coordinator.cancel_request(request, ReasonCode::Cancelled).ok()) {
            cancelled.push_back(request);
            live.erase(std::remove(live.begin(), live.end(), request), live.end());
          }
          break;
        case 4: {
          const AttemptId replacement =
              itf::test::make_attempt_id(seed, 1000 + static_cast<std::uint64_t>(step));
          (void)harness.coordinator.replace_attempt(request, attempt, replacement, kModelHash);
          break;
        }
        case 5: {
          CompletionPublication publication;
          publication.request = request;
          publication.attempt = attempt;
          publication.attempt_generation = record->current_attempt_generation;
          publication.bytes_in = 1024;
          const auto outcome = harness.coordinator.publish_completion(publication);
          if (outcome.ok() && outcome.value().committed()) {
            live.erase(std::remove(live.begin(), live.end(), request), live.end());
          }
          break;
        }
        case 6:
          (void)harness.coordinator.fail_attempt(request, attempt, ReasonCode::AttemptFailed);
          live.erase(std::remove(live.begin(), live.end(), request), live.end());
          break;
        case 7:
          (void)harness.coordinator.advance_stage(request, attempt, ServingStage::Routing);
          break;
        default:
          break;
      }
    }

    const Status invariants = harness.coordinator.verify_invariants();
    if (!invariants.ok()) {
      std::printf("  invariant failure at step %d (seed %llu): %s\n", step,
                  static_cast<unsigned long long>(seed), invariants.to_string().c_str());
      ITF_CHECK(false);
      return;
    }
    const AccountingSnapshot snapshot = harness.coordinator.accounting();
    if (snapshot.pending_deferrals > snapshot.requests_registered) {
      ITF_CHECK(false);
      return;
    }
  }

  // Closure: every cancelled request stays cancelled and holds no authority.
  for (const RequestId& request : cancelled) {
    const auto record = harness.coordinator.request_record(request);
    if (!record.has_value()) continue;
    ITF_CHECK_EQ(static_cast<int>(record->lifecycle), static_cast<int>(RequestLifecycle::Cancelled));
    ITF_CHECK_EQ(record->active_flows, 0U);
    CompletionPublication publication;
    publication.request = request;
    publication.attempt = record->current_attempt;
    const auto outcome = harness.coordinator.publish_completion(publication);
    if (outcome.ok()) {
      ITF_CHECK(outcome.value().refused());
    }
  }
  ITF_REQUIRE_STATUS_OK(harness.coordinator.verify_invariants());
  ITF_CHECK(harness.coordinator.accounting().closed());
}

ITF_TEST(seeded_state_machine_sequences_hold_invariants) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    run_sequence(itf::test::seed() + seed * 7919ULL, 400);
  }
}

ITF_TEST(a_single_long_sequence_holds_invariants) {
  run_sequence(itf::test::seed() * 31ULL + 5ULL, 4000);
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_property_state_machine", argc, argv);
}
