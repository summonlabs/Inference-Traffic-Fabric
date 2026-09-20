// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "itf/coordinator.hpp"
#include "itf/persistence.hpp"
#include "support/cluster.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x0A0B0C0D0E0F1011ULL;
constexpr std::uint64_t kStateHash = 0x1112131415161718ULL;

ITF_TEST(abrupt_coordinator_death_does_not_resurrect_authority) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  const RequestId live_request = itf::test::make_request_id(111, 1);
  const AttemptId live_attempt = itf::test::make_attempt_id(111, 1);
  const RequestId finished_request = itf::test::make_request_id(111, 2);
  const AttemptId finished_attempt = itf::test::make_attempt_id(111, 2);
  CoordinatorEpoch first_epoch;

  {
    auto driver = cluster.connect("driver");
    ITF_REQUIRE(driver.ok());
    ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));
    first_epoch = driver.value().hello().epoch;

    ITF_REQUIRE(driver.value().register_request(live_request, 0).ok());
    ITF_REQUIRE(driver.value().register_attempt(live_request, live_attempt, kModelHash).ok());

    ITF_REQUIRE(driver.value().register_request(finished_request, 0).ok());
    const auto registration =
        driver.value().register_attempt(finished_request, finished_attempt, kModelHash);
    ITF_REQUIRE(registration.ok());
    CompletionPublication publication;
    publication.request = finished_request;
    publication.attempt = finished_attempt;
    publication.attempt_generation = registration.value().generation;
    publication.bytes_in = 2048;
    const auto outcome = driver.value().publish_completion(publication);
    ITF_REQUIRE(outcome.ok());
    ITF_CHECK(outcome.value().committed());
    driver.value().close();
  }

  // Abrupt death: no graceful shutdown, no final snapshot.
  cluster.kill();
  ITF_CHECK(!cluster.running());

  ITF_REQUIRE_STATUS_OK(cluster.start());
  auto driver = cluster.connect("driver-after-restart");
  ITF_REQUIRE(driver.ok());

  // The epoch advanced, so every decision minted before the restart is fenced.
  ITF_CHECK(driver.value().hello().epoch > first_epoch);
  const auto status = driver.value().status();
  ITF_REQUIRE(status.ok());
  ITF_CHECK(status.value().epoch > first_epoch);

  // Terminal history survived and is still reported.
  const auto snapshot = driver.value().accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().requests_completed, 1ULL);
  ITF_CHECK_EQ(snapshot.value().active_flows_total, 0U);

  // Dynamic evidence from the previous incarnation is not current.
  const auto model = driver.value().advance_model_generation(
      kModelHash, std::optional<ModelGeneration>(ModelGeneration::from(1)), false);
  ITF_CHECK(!model.ok());

  // The previously live request is interrupted and needs fresh authority.
  const auto revalidated = driver.value().register_attempt(
      live_request, itf::test::make_attempt_id(111, 3), kModelHash);
  ITF_CHECK(!revalidated.ok());
  ITF_CHECK_EQ(static_cast<int>(revalidated.code()),
               static_cast<int>(StatusCode::RevalidationRequired));

  // Traffic for the interrupted request is refused rather than inherited.
  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = live_request;
  subject.attempt = live_attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::Control;
  message.declared_stage = ServingStage::Admitted;
  message.payload_bytes = 256;
  message.binding.coordinator_epoch = first_epoch;
  const auto decision = driver.value().evaluate(message);
  ITF_REQUIRE(decision.ok());
  ITF_CHECK_EQ(static_cast<int>(decision.value().outcome),
               static_cast<int>(DecisionOutcome::Rejected));
  ITF_CHECK_EQ(decision.value().reason, ReasonCode::RejectedStaleEpoch);
}

ITF_TEST(a_corrupt_snapshot_prevents_startup) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  {
    auto driver = cluster.connect("driver");
    ITF_REQUIRE(driver.ok());
    ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));
    ITF_REQUIRE(driver.value().register_request(itf::test::make_request_id(112, 1), 0).ok());
    driver.value().close();
  }
  cluster.stop();
  ITF_CHECK(itf::test::file_exists(cluster.snapshot_path()));

  // Corrupt a byte in the middle of the durable document. The coordinator must
  // refuse to start rather than silently beginning a fresh epoch.
  const std::uint64_t size = itf::test::file_size(cluster.snapshot_path());
  ITF_REQUIRE(size > SnapshotStore::kHeaderBytes + 8);
  itf::test::flip_byte(cluster.snapshot_path(), SnapshotStore::kHeaderBytes + 4);

  const std::uint16_t port = itf::test::find_free_port();
  std::vector<std::string> arguments = {"--port",   std::to_string(port), "--token", cluster.token(),
                                        "--state-dir", cluster.directory()};
  auto child = itf::test::ChildProcess::spawn(itf::test::tool_path("itf_coordinator"), arguments);
  ITF_REQUIRE(child.ok());
  const int code = child.value().wait();
  std::printf("  corrupt snapshot exit code: %d\n", code);
  ITF_CHECK(code != 0);
}

ITF_TEST(a_truncated_snapshot_prevents_startup) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  {
    auto driver = cluster.connect("driver");
    ITF_REQUIRE(driver.ok());
    ITF_REQUIRE_STATUS_OK(itf::test::prepare_fabric(driver.value(), kModelHash, kStateHash));
    ITF_REQUIRE(driver.value().register_request(itf::test::make_request_id(113, 1), 0).ok());
    driver.value().close();
  }
  cluster.stop();
  const std::uint64_t size = itf::test::file_size(cluster.snapshot_path());
  ITF_REQUIRE(size > SnapshotStore::kHeaderBytes);
  itf::test::truncate_file(cluster.snapshot_path(), size / 2);

  const std::uint16_t port = itf::test::find_free_port();
  std::vector<std::string> arguments = {"--port",   std::to_string(port), "--token", cluster.token(),
                                        "--state-dir", cluster.directory()};
  auto child = itf::test::ChildProcess::spawn(itf::test::tool_path("itf_coordinator"), arguments);
  ITF_REQUIRE(child.ok());
  const int code = child.value().wait();
  std::printf("  truncated snapshot exit code: %d\n", code);
  ITF_CHECK(code != 0);
}

ITF_TEST(a_removed_snapshot_starts_a_fresh_epoch) {
  itf::test::Cluster cluster;
  ITF_REQUIRE_STATUS_OK(cluster.start());
  {
    auto driver = cluster.connect("driver");
    ITF_REQUIRE(driver.ok());
    ITF_REQUIRE(driver.value().register_request(itf::test::make_request_id(114, 1), 0).ok());
    driver.value().close();
  }
  cluster.stop();
  ITF_REQUIRE(itf::test::file_exists(cluster.snapshot_path()));
  (void)std::remove(cluster.snapshot_path().c_str());

  ITF_REQUIRE_STATUS_OK(cluster.start());
  auto driver = cluster.connect("driver-fresh");
  ITF_REQUIRE(driver.ok());
  const auto status = driver.value().status();
  ITF_REQUIRE(status.ok());
  ITF_CHECK_EQ(status.value().epoch.value(), 1ULL);
  const auto snapshot = driver.value().accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().requests_registered, 0ULL);
  ITF_CHECK_EQ(snapshot.value().active_flows_total, 0U);
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_multiprocess_restart", argc, argv);
}
