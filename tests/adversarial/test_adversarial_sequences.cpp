// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deliberate break-it surface: hostile randomized operations against the
// coordinator, with the invariants asserted after every single step.

#include <random>
#include <string>
#include <vector>

#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x7E57AB1E7E57AB1EULL;
constexpr std::uint64_t kStateHash = 0x0DDBA110DDBA1100ULL;

struct Fixture {
  VirtualClock clock;
  Coordinator coordinator;

  Fixture(std::uint32_t defer_records, std::uint32_t flows)
      : coordinator(make_config(defer_records, flows), &clock) {
    (void)coordinator.boot_fresh();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Synthetic;
    topology.node_count = 2;
    topology.link_count = 1;
    topology.queue_depth = 64;
    topology.fabric_bytes_per_sec = 1000000000ULL;
    topology.max_age_nanos = 1000000000ULL;
    (void)coordinator.publish_topology(topology);
  }

  static CoordinatorConfig make_config(std::uint32_t defer_records, std::uint32_t flows) {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.limits.max_defer_records = defer_records;
    config.limits.ledger.max_active_flows_total = flows;
    config.boot_seed = 0xBADF00DULL;
    return config;
  }

  [[nodiscard]] TrafficRequest make_message(const RequestId& request, const AttemptId& attempt,
                                            TrafficClass traffic, ServingStage stage,
                                            std::uint64_t bytes) {
    TrafficRequest message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    message.subject = subject;
    message.declared_class = traffic;
    message.declared_stage = stage;
    message.payload_bytes = bytes;
    message.binding.policy = coordinator.policy().generation();
    message.binding.coordinator_epoch = coordinator.epoch();
    message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    return message;
  }
};

ITF_TEST(randomized_hostile_operations_hold_every_invariant) {
  std::mt19937_64 random(itf::test::seed() ^ 0x5A5A5A5AULL);
  Fixture fixture(16, 32);
  Coordinator& coordinator = fixture.coordinator;

  std::vector<RequestId> requests;
  std::vector<AttemptId> attempts;
  int operations = 0;

  for (int step = 0; step < 6000; ++step) {
    const std::uint64_t choice = random() % 14;
    if (choice == 0 || requests.empty()) {
      const RequestId request =
          itf::test::make_request_id(0x51EEDULL, static_cast<std::uint64_t>(step));
      const AttemptId attempt =
          itf::test::make_attempt_id(0x51EEDULL, static_cast<std::uint64_t>(step));
      (void)coordinator.register_request(request);
      (void)coordinator.register_attempt(request, attempt, kModelHash);
      (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
      (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
      requests.push_back(request);
      attempts.push_back(attempt);
      ++operations;
      continue;
    }

    const std::size_t index = static_cast<std::size_t>(random() % requests.size());
    const RequestId request = requests[index];
    const AttemptId attempt = attempts[index];
    switch (choice) {
      case 1:
        (void)coordinator.set_model_generation(kModelHash, std::nullopt);
        (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
        break;
      case 2: {
        // Hostile: a nil or foreign generation binding.
        auto message = fixture.make_message(request, attempt, TrafficClass::Control,
                                            ServingStage::Admitted, random() % 4096);
        if ((random() % 2) == 0) {
          message.binding.policy = PolicyGeneration::from(random() % 64);
        } else {
          message.binding.coordinator_epoch = CoordinatorEpoch::from(random() % 64);
        }
        (void)coordinator.evaluate(message);
        break;
      }
      case 3: {
        // Hostile: an absurd declared payload size.
        auto message = fixture.make_message(request, attempt, TrafficClass::Control,
                                            ServingStage::Admitted, UINT64_MAX - (random() % 8));
        (void)coordinator.evaluate(message);
        break;
      }
      case 4: {
        const Decision decision = coordinator.evaluate(
            fixture.make_message(request, attempt, TrafficClass::Control, ServingStage::Admitted,
                                 random() % 1024));
        if (decision.permits_traffic()) {
          auto flow = coordinator.open_flow(decision, decision.treatment.max_burst_bytes);
          if (flow.ok()) {
            (void)coordinator.record_flow_bytes(flow.value(), random() % 4096, (random() % 2) == 0);
            if ((random() % 3) == 0) (void)coordinator.release_flow(flow.value());
          }
        }
        break;
      }
      case 5: {
        // Hostile: replay a decision that was just invalidated.
        const Decision decision = coordinator.evaluate(
            fixture.make_message(request, attempt, TrafficClass::Control, ServingStage::Admitted, 64));
        (void)coordinator.cancel_request(request, ReasonCode::Cancelled);
        (void)coordinator.open_flow(decision, 64);
        break;
      }
      case 6:
        (void)coordinator.fail_attempt(request, attempt, ReasonCode::AttemptFailed);
        break;
      case 7:
        (void)coordinator.replace_attempt(
            request, attempt, itf::test::make_attempt_id(0xFA11ULL, static_cast<std::uint64_t>(step)),
            kModelHash);
        break;
      case 8: {
        TransferRegistration registration;
        registration.id =
            itf::test::make_transfer_id(0x7A11ULL, static_cast<std::uint64_t>(step));
        registration.state_hash = kStateHash;
        registration.model_hash = kModelHash;
        registration.payload_bytes = random() % (1U << 24);
        registration.declared_class = static_cast<TrafficClass>(
            4 + (random() % 4));  // KV, STATE, MODEL or ADAPTER
        registration.direction = FlowDirection::Lateral;
        registration.request = request;
        (void)coordinator.register_transfer(registration);
        break;
      }
      case 9: {
        CompletionPublication publication;
        publication.request = request;
        publication.attempt = attempt;
        publication.attempt_generation = AttemptGeneration::from(1 + (random() % 4));
        publication.bytes_in = random() % 8192;
        publication.bytes_out = random() % 8192;
        (void)coordinator.publish_completion(publication);
        break;
      }
      case 10: {
        const auto record = coordinator.request_record(request);
        if (record.has_value()) {
          CompletionPublication publication;
          publication.request = request;
          publication.attempt = record->current_attempt;
          publication.attempt_generation = record->current_attempt_generation;
          (void)coordinator.publish_completion(publication);
        }
        break;
      }
      case 11:
        (void)coordinator.cancel_request(request, ReasonCode::Cancelled);
        break;
      case 12:
        (void)coordinator.advance_stage(request, attempt,
                                        static_cast<ServingStage>(1 + (random() % 12)));
        break;
      default:
        (void)coordinator.accounting();
        (void)coordinator.status();
        (void)coordinator.explain_request(request);
        break;
    }
    ++operations;

    const Status invariants = coordinator.verify_invariants();
    if (!invariants.ok()) {
      std::printf("  invariant failure after operation %d (seed %llu): %s\n", step,
                  static_cast<unsigned long long>(itf::test::seed()),
                  invariants.to_string().c_str());
      ITF_CHECK(false);
      return;
    }
  }

  const AccountingSnapshot snapshot = coordinator.accounting();
  std::printf("  hostile run: operations=%d active_flows=%u pending_deferrals=%u\n", operations,
              snapshot.active_flows_total, snapshot.pending_deferrals);
  ITF_CHECK(snapshot.pending_deferrals <= 16U);
  ITF_CHECK(snapshot.active_flows_total <= 32U);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(every_ceiling_refuses_deterministically) {
  Fixture fixture(4, 3);
  Coordinator& coordinator = fixture.coordinator;
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.advance_state_generation(kStateHash, std::nullopt);

  int refused_flows = 0;
  for (int index = 0; index < 16; ++index) {
    const RequestId request = itf::test::make_request_id(0xCE1111ULL, static_cast<std::uint64_t>(index));
    const AttemptId attempt = itf::test::make_attempt_id(0xCE1111ULL, static_cast<std::uint64_t>(index));
    ITF_REQUIRE(coordinator.register_request(request).ok());
    ITF_REQUIRE(coordinator.register_attempt(request, attempt, kModelHash).ok());
    ITF_REQUIRE(coordinator.issue_route_decision(request, attempt, 0, false, false).ok());
    ITF_REQUIRE(coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200).ok());
    ITF_REQUIRE(coordinator.advance_stage(request, attempt, ServingStage::Admitted).ok());
    const Decision decision = coordinator.evaluate(
        fixture.make_message(request, attempt, TrafficClass::RequestIngress, ServingStage::Admitted, 64));
    ITF_REQUIRE(decision.permits_traffic());
    const auto flow = coordinator.open_flow(decision, 64);
    if (!flow.ok()) {
      ++refused_flows;
      ITF_CHECK_EQ(static_cast<int>(flow.code()), static_cast<int>(StatusCode::CapacityExhausted));
    }
  }
  ITF_CHECK_EQ(refused_flows, 13);
  ITF_CHECK_EQ(coordinator.accounting().active_flows_total, 3U);
  ITF_REQUIRE_STATUS_OK(coordinator.verify_invariants());
}

ITF_TEST(every_truncation_length_is_refused) {
  const std::string directory = itf::test::make_temp_dir("adversarial-truncate");
  const std::string path = directory + "/state.bin";
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  (void)coordinator.register_request(itf::test::make_request_id(0x7A11ULL, 1));
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());
  SnapshotStore store(path);
  ITF_REQUIRE_STATUS_OK(store.save(state.value()));
  const std::uint64_t full = itf::test::file_size(path);
  ITF_REQUIRE(full > SnapshotStore::kHeaderBytes + 4);

  int accepted = 0;
  for (std::uint64_t length = 0; length < full; ++length) {
    SnapshotStore scratch(path);
    ITF_REQUIRE_STATUS_OK(scratch.save(state.value()));
    itf::test::truncate_file(path, length);
    const auto loaded = scratch.load();
    if (loaded.ok()) ++accepted;
  }
  ITF_CHECK_EQ(accepted, 0);
  itf::test::remove_dir(directory);
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_adversarial_sequences", argc, argv);
}
