// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x0102030405060708ULL;
constexpr std::uint64_t kStateHash = 0x0807060504030201ULL;

struct Snapshot {
  std::string directory;
  std::string path;
  CoordinatorConfig config;

  Snapshot() {
    directory = itf::test::make_temp_dir("persistence-recovery");
    path = directory + "/coordinator.bin";
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.boot_seed = 31;
  }

  ~Snapshot() { itf::test::remove_dir(directory); }
};

ITF_TEST(restart_advances_the_epoch_and_requires_revalidation) {
  Snapshot snapshot;
  VirtualClock clock;

  RequestId completed_request = itf::test::make_request_id(91, 1);
  AttemptId completed_attempt = itf::test::make_attempt_id(91, 1);
  RequestId live_request = itf::test::make_request_id(91, 2);
  AttemptId live_attempt = itf::test::make_attempt_id(91, 2);
  CoordinatorEpoch first_epoch;

  {
    Coordinator coordinator(snapshot.config, &clock);
    ITF_REQUIRE(coordinator.boot_fresh().ok());
    first_epoch = coordinator.epoch();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Measured;
    topology.node_count = 1;
    topology.link_count = 0;
    topology.fabric_bytes_per_sec = 1000000000ULL;
    topology.max_age_nanos = 1000000000000ULL;
    ITF_REQUIRE(coordinator.publish_topology(topology).ok());
    ITF_REQUIRE(coordinator.set_model_generation(kModelHash, std::nullopt).ok());
    ITF_REQUIRE(coordinator.advance_state_generation(kStateHash, std::nullopt).ok());
    ITF_CHECK_EQ(coordinator.topology_state(), EvidenceState::Current);

    ITF_REQUIRE(coordinator.register_request(completed_request).ok());
    const auto completed_registration =
        coordinator.register_attempt(completed_request, completed_attempt, kModelHash);
    ITF_REQUIRE(completed_registration.ok());
    CompletionPublication publication;
    publication.request = completed_request;
    publication.attempt = completed_attempt;
    publication.attempt_generation = completed_registration.value();
    publication.bytes_in = 4096;
    const auto outcome = coordinator.publish_completion(publication);
    ITF_REQUIRE(outcome.ok());
    ITF_CHECK(outcome.value().committed());

    ITF_REQUIRE(coordinator.register_request(live_request).ok());
    ITF_REQUIRE(coordinator.register_attempt(live_request, live_attempt, kModelHash).ok());

    auto state = coordinator.export_state();
    ITF_REQUIRE(state.ok());
    SnapshotStore store(snapshot.path);
    ITF_REQUIRE_STATUS_OK(store.save(state.value()));
  }

  Coordinator restored(snapshot.config, &clock);
  SnapshotStore store(snapshot.path);
  const auto loaded = store.load();
  ITF_REQUIRE(loaded.ok());
  const auto booted = restored.boot_from(loaded.value());
  ITF_REQUIRE(booted.ok());
  ITF_CHECK(restored.epoch() > first_epoch);
  ITF_CHECK_EQ(restored.epoch().value(), first_epoch.value() + 1);

  // Dynamic evidence from the previous incarnation is never resurrected as
  // current: it returns as revalidation-required.
  ITF_CHECK_EQ(restored.topology_state(), EvidenceState::RevalidationRequired);
  const auto model = restored.model_generation(kModelHash);
  ITF_CHECK(!model.ok());
  ITF_CHECK_EQ(static_cast<int>(model.code()), static_cast<int>(StatusCode::RevalidationRequired));
  const auto state = restored.state_generation(kStateHash);
  ITF_CHECK(!state.ok());
  ITF_CHECK_EQ(static_cast<int>(state.code()), static_cast<int>(StatusCode::RevalidationRequired));

  // Terminal history is retained for idempotency and accounting.
  const auto completed = restored.request_record(completed_request);
  ITF_REQUIRE(completed.has_value());
  ITF_CHECK_EQ(static_cast<int>(completed->lifecycle),
               static_cast<int>(RequestLifecycle::Completed));
  ITF_CHECK_EQ(completed->bytes_in, 4096ULL);

  // A live request becomes interrupted and requires fresh authority.
  const auto live = restored.request_record(live_request);
  ITF_REQUIRE(live.has_value());
  ITF_CHECK_EQ(static_cast<int>(live->lifecycle), static_cast<int>(RequestLifecycle::Interrupted));
  ITF_CHECK(live->requires_revalidation);

  // Revalidating a model generation restores it with the same generation
  // number, which is what makes transfer fencing meaningful.
  const auto revalidated = restored.revalidate_model_generation(kModelHash);
  ITF_REQUIRE(revalidated.ok());
  ITF_CHECK_EQ(revalidated.value().value(), 1ULL);
  const auto revalidated_state = restored.revalidate_state_generation(kStateHash);
  ITF_REQUIRE(revalidated_state.ok());
  ITF_REQUIRE_STATUS_OK(restored.verify_invariants());
}

ITF_TEST(a_fresh_boot_ignores_previous_authority) {
  Snapshot snapshot;
  VirtualClock clock;
  Coordinator coordinator(snapshot.config, &clock);
  ITF_REQUIRE(coordinator.boot_fresh().ok());
  const AuthorityStamp first = coordinator.authority();
  const Decision before = coordinator.evaluate(TrafficRequest{});
  ITF_CHECK(before.authority.epoch == first.epoch);

  ITF_REQUIRE(coordinator.boot_fresh().ok());
  const AuthorityStamp second = coordinator.authority();
  ITF_CHECK_EQ(second.epoch.value(), 1ULL);
  ITF_CHECK(second.incarnation != first.incarnation);
  ITF_CHECK(second.sequence.value() >= 1ULL);
}

ITF_TEST(models_and_states_restored_from_a_snapshot_require_revalidation) {
  Snapshot snapshot;
  VirtualClock clock;
  Coordinator coordinator(snapshot.config, &clock);
  ITF_REQUIRE(coordinator.boot_fresh().ok());
  ITF_REQUIRE(coordinator.set_model_generation(kModelHash, std::nullopt).ok());
  ITF_REQUIRE(coordinator.advance_state_generation(kStateHash, std::nullopt).ok());
  ITF_REQUIRE(coordinator.advance_state_generation(kStateHash, std::nullopt).ok());
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());
  ITF_CHECK_EQ(state.value().models.size(), static_cast<std::size_t>(1));
  ITF_CHECK_EQ(state.value().states.size(), static_cast<std::size_t>(1));
  ITF_CHECK_EQ(state.value().states.front().generation.value(), 2ULL);

  Coordinator restored(snapshot.config, &clock);
  ITF_REQUIRE(restored.boot_from(state.value()).ok());
  ITF_CHECK_EQ(static_cast<int>(restored.state_generation(kStateHash).code()),
               static_cast<int>(StatusCode::RevalidationRequired));
  ITF_REQUIRE(restored.revalidate_state_generation(kStateHash).ok());
  const auto generation = restored.state_generation(kStateHash);
  ITF_REQUIRE(generation.ok());
  ITF_CHECK_EQ(generation.value().value(), 2ULL);
}

ITF_TEST(a_refused_document_never_partially_applies) {
  Snapshot snapshot;
  VirtualClock clock;
  Coordinator coordinator(snapshot.config, &clock);
  ITF_REQUIRE(coordinator.boot_fresh().ok());
  ITF_REQUIRE(coordinator.set_model_generation(kModelHash, std::nullopt).ok());
  auto state = coordinator.export_state();
  ITF_REQUIRE(state.ok());

  PersistedCoordinatorState broken = state.value();
  broken.models.clear();
  broken.policy.set_generation(PolicyGeneration());
  Coordinator fresh(snapshot.config, &clock);
  // A policy document without a generation is refused during restore.
  ITF_CHECK(!fresh.boot_from(broken).ok());
  ITF_CHECK(fresh.epoch().is_unset());
  ITF_CHECK_EQ(static_cast<int>(fresh.model_generation(kModelHash).code()),
               static_cast<int>(StatusCode::NotFound));
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_persistence_recovery", argc, argv);
}
