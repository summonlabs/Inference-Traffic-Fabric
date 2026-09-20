// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <memory>
#include <string>

#include "itf/client.hpp"
#include "itf/persistence.hpp"
#include "service.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0xA0A0A0A0A0A0A0A0ULL;
constexpr std::uint64_t kStateHash = 0xB0B0B0B0B0B0B0B0ULL;
const std::string kToken = "integration-secret";

struct ServiceFixture {
  std::string directory;
  std::unique_ptr<service::CoordinatorService> service;

  explicit ServiceFixture(bool persist) {
    directory = itf::test::make_temp_dir("integration");
    service::ServiceConfig config;
    config.server.bind_host = "127.0.0.1";
    config.server.port = 0;
    config.server.auth_token = kToken;
    config.persist = persist;
    if (persist) config.snapshot_path = directory + "/coordinator.bin";
    config.coordinator.boot_seed = 7;
    service = std::make_unique<service::CoordinatorService>(config, &steady_clock_singleton());
  }

  ~ServiceFixture() {
    if (service) {
      service->stop();
      service.reset();
    }
    itf::test::remove_dir(directory);
  }

  Status start() { return service->start(); }

  std::uint16_t port() const { return service->server().bound_port(); }

  Result<net::Client> connect(const std::string& token = kToken) const {
    net::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port();
    config.peer_name = "integration-client";
    config.auth_token = token;
    return net::Client::connect(config);
  }
};

void publish_topology(net::Client& client) {
  TopologyEvidence evidence;
  evidence.provenance = EvidenceProvenance::Measured;
  evidence.node_count = 1;
  evidence.accelerator_count = 1;
  evidence.link_count = 0;
  evidence.fabric_bytes_per_sec = 25000000000ULL;
  evidence.max_age_nanos = 30000000000ULL;
  const auto published = client.publish_topology(evidence);
  ITF_CHECK(published.ok());
}

ITF_TEST(handshake_requires_the_shared_secret) {
  ServiceFixture fixture(false);
  ITF_REQUIRE_STATUS_OK(fixture.start());
  const auto rejected = fixture.connect("wrong-secret");
  ITF_CHECK(!rejected.ok());
  ITF_CHECK_EQ(static_cast<int>(rejected.code()), static_cast<int>(StatusCode::NotAuthorized));
  const auto accepted = fixture.connect();
  ITF_REQUIRE(accepted.ok());
  ITF_CHECK(!accepted.value().hello().epoch.is_unset());
  ITF_CHECK_EQ(accepted.value().hello().policy_generation.value(), 1ULL);
  ITF_CHECK(accepted.value().max_frame_payload() > 0);
}

ITF_TEST(full_serving_lifecycle_over_the_transport) {
  ServiceFixture fixture(false);
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect();
  ITF_REQUIRE(connected.ok());
  net::Client& client = connected.value();
  publish_topology(client);

  const auto model = client.advance_model_generation(kModelHash, std::nullopt, false);
  ITF_REQUIRE(model.ok());
  const auto state = client.advance_state_generation(kStateHash, std::nullopt, false);
  ITF_REQUIRE(state.ok());

  const RequestId request = itf::test::make_request_id(31, 1);
  const AttemptId attempt = itf::test::make_attempt_id(31, 1);
  ITF_REQUIRE(client.register_request(request, 5000000000ULL).ok());
  const auto attempt_registration = client.register_attempt(request, attempt, kModelHash);
  ITF_REQUIRE(attempt_registration.ok());
  const auto route = client.issue_route_decision(request, attempt, 0, false, false);
  ITF_REQUIRE(route.ok());
  const auto slo = client.publish_slo_contract(request, 5000000000ULL, 5000000000ULL, 200);
  ITF_REQUIRE(slo.ok());
  ITF_REQUIRE(client.advance_stage(request, attempt, ServingStage::Admitted).ok());

  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::RequestIngress;
  message.declared_stage = ServingStage::Admitted;
  message.payload_bytes = 16384;
  message.binding.policy = client.hello().policy_generation;
  message.binding.coordinator_epoch = client.hello().epoch;
  message.binding.model_generation = model.value().generation;
  message.binding.route_decision = route.value().decision.generation;
  message.binding.slo_contract = slo.value().contract.generation;
  message.deadline_after_nanos = 5000000000ULL;
  const auto decision = client.evaluate(message);
  ITF_REQUIRE(decision.ok());
  ITF_CHECK_EQ(static_cast<int>(decision.value().outcome),
               static_cast<int>(DecisionOutcome::Allowed));

  const auto flow = client.open_flow(decision.value(), message.payload_bytes);
  ITF_REQUIRE(flow.ok());
  const auto mid = client.accounting();
  ITF_REQUIRE(mid.ok());
  ITF_CHECK_EQ(mid.value().active_flows_total, 1U);

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = attempt_registration.value().generation;
  publication.bytes_in = message.payload_bytes;
  const auto outcome = client.publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().committed());

  const auto duplicate = client.publish_completion(publication);
  ITF_REQUIRE(duplicate.ok());
  ITF_CHECK_EQ(static_cast<int>(duplicate.value().disposition),
               static_cast<int>(PublishOutcome::Disposition::DuplicateSuppressed));

  const auto final_snapshot = client.accounting();
  ITF_REQUIRE(final_snapshot.ok());
  ITF_CHECK_EQ(final_snapshot.value().active_flows_total, 0U);
  ITF_CHECK_EQ(final_snapshot.value().requests_completed, 1ULL);
  ITF_CHECK_EQ(final_snapshot.value().completions_suppressed, 1ULL);
  ITF_CHECK(final_snapshot.value().closed());

  const auto status = client.status();
  ITF_REQUIRE(status.ok());
  ITF_CHECK_EQ(status.value().epoch.value(), 1ULL);
  ITF_CHECK_EQ(status.value().topology_state, EvidenceState::Current);
  ITF_REQUIRE_STATUS_OK(fixture.service->coordinator().verify_invariants());
}

ITF_TEST(cancelled_requests_cannot_regain_authority) {
  ServiceFixture fixture(false);
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect();
  ITF_REQUIRE(connected.ok());
  net::Client& client = connected.value();
  publish_topology(client);
  ITF_REQUIRE(client.advance_model_generation(kModelHash, std::nullopt, false).ok());
  ITF_REQUIRE(client.advance_state_generation(kStateHash, std::nullopt, false).ok());

  const RequestId request = itf::test::make_request_id(32, 1);
  const AttemptId attempt = itf::test::make_attempt_id(32, 1);
  ITF_REQUIRE(client.register_request(request, 0).ok());
  const auto registration = client.register_attempt(request, attempt, kModelHash);
  ITF_REQUIRE(registration.ok());
  ITF_REQUIRE(client.issue_route_decision(request, attempt, 0, false, false).ok());
  ITF_REQUIRE(client.publish_slo_contract(request, 5000000000ULL, 5000000000ULL, 200).ok());
  ITF_REQUIRE(client.advance_stage(request, attempt, ServingStage::Admitted).ok());

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
  message.binding.policy = client.hello().policy_generation;
  message.binding.coordinator_epoch = client.hello().epoch;
  message.binding.model_generation = ModelGeneration::from(1);
  message.binding.route_decision = RouteDecisionGeneration::from(1);
  message.binding.slo_contract = SloContractGeneration::from(1);
  message.deadline_after_nanos = 5000000000ULL;

  const auto before_cancel = client.evaluate(message);
  ITF_REQUIRE(before_cancel.ok());
  ITF_CHECK(before_cancel.value().permits_traffic());
  ITF_REQUIRE(client.open_flow(before_cancel.value(), message.payload_bytes).ok());

  ITF_REQUIRE(client.cancel_request(request, ReasonCode::Cancelled).ok());

  // The decision minted before the cancellation can no longer open a flow.
  const auto reused = client.open_flow(before_cancel.value(), message.payload_bytes);
  ITF_CHECK(!reused.ok());

  const auto after = client.evaluate(message);
  ITF_REQUIRE(after.ok());
  ITF_CHECK_EQ(after.value().reason, ReasonCode::RejectedRequestCancelled);

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = registration.value().generation;
  const auto outcome = client.publish_completion(publication);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().refused());
  ITF_CHECK_EQ(outcome.value().reason, ReasonCode::RejectedRequestCancelled);

  const auto snapshot = client.accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().active_flows_total, 0U);
  ITF_CHECK_EQ(snapshot.value().requests_cancelled, 1ULL);
  ITF_REQUIRE_STATUS_OK(fixture.service->coordinator().verify_invariants());
}

ITF_TEST(transfer_generation_fencing_over_the_transport) {
  ServiceFixture fixture(false);
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect();
  ITF_REQUIRE(connected.ok());
  net::Client& client = connected.value();
  publish_topology(client);
  ITF_REQUIRE(client.advance_model_generation(kModelHash, std::nullopt, false).ok());
  ITF_REQUIRE(client.advance_state_generation(kStateHash, std::nullopt, false).ok());

  const RequestId owner = itf::test::make_request_id(33, 2);
  ITF_REQUIRE(client.register_request(owner, 0).ok());
  ITF_REQUIRE(client.publish_slo_contract(owner, 5000000000ULL, 5000000000ULL, 200).ok());

  TransferRegistration registration;
  registration.id = itf::test::make_transfer_id(33, 1);
  registration.state_hash = kStateHash;
  registration.model_hash = kModelHash;
  registration.payload_bytes = 1U << 20;
  registration.declared_class = TrafficClass::KvTransfer;
  registration.direction = FlowDirection::Lateral;
  registration.request = owner;
  const auto registered = client.register_transfer(registration);
  ITF_REQUIRE(registered.ok());

  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Transfer;
  subject.transfer = registration.id;
  message.subject = subject;
  message.declared_class = TrafficClass::KvTransfer;
  message.direction = FlowDirection::Lateral;
  message.payload_bytes = registration.payload_bytes;
  message.binding.policy = client.hello().policy_generation;
  message.binding.coordinator_epoch = client.hello().epoch;
  message.binding.model_generation = registered.value().model_generation;
  message.binding.state_generation = registered.value().state_generation;
  message.binding.slo_contract = SloContractGeneration::from(1);

  const auto authorized = client.authorize_transfer(message);
  ITF_REQUIRE(authorized.ok());
  if (!authorized.value().authorization.authorized) {
    std::printf("  transfer refused: %s\n",
                std::string(to_string(authorized.value().authorization.reason)).c_str());
  }
  ITF_CHECK(authorized.value().authorization.authorized);

  // Advancing the state generation fences the transfer that was bound to the
  // previous generation.
  const auto advanced = client.advance_state_generation(kStateHash, std::nullopt, false);
  ITF_REQUIRE(advanced.ok());
  const auto stale = client.authorize_transfer(message);
  ITF_REQUIRE(stale.ok());
  ITF_CHECK(!stale.value().authorization.authorized);
  ITF_CHECK_EQ(stale.value().authorization.reason, ReasonCode::RejectedStaleStateGeneration);

  const auto snapshot = client.accounting();
  ITF_REQUIRE(snapshot.ok());
  ITF_CHECK_EQ(snapshot.value().transfers_started, 1ULL);
  ITF_CHECK_EQ(snapshot.value().transfers_completed, 0ULL);
  ITF_REQUIRE_STATUS_OK(fixture.service->coordinator().verify_invariants());
}

ITF_TEST(shutdown_request_is_honoured) {
  ServiceFixture fixture(false);
  ITF_REQUIRE_STATUS_OK(fixture.start());
  auto connected = fixture.connect();
  ITF_REQUIRE(connected.ok());
  Writer writer;
  wire::HeartbeatMessage heartbeat;
  heartbeat.encode(writer);
  const auto ack = connected.value().heartbeat();
  ITF_REQUIRE(ack.ok());
  ITF_CHECK(!fixture.service->shutdown_requested());
  fixture.service->request_shutdown();
  ITF_CHECK(fixture.service->shutdown_requested());
  fixture.service->stop();
  ITF_CHECK(!fixture.service->server().running());
}

ITF_TEST(restart_advances_the_epoch_and_keeps_terminal_history) {
  const std::string directory = itf::test::make_temp_dir("integration-restart");
  const auto make_config = [&]() {
    service::ServiceConfig config;
    config.server.bind_host = "127.0.0.1";
    config.server.port = 0;
    config.server.auth_token = kToken;
    config.persist = true;
    config.snapshot_path = directory + "/coordinator.bin";
    config.coordinator.boot_seed = 11;
    return config;
  };

  const RequestId request = itf::test::make_request_id(34, 1);
  const AttemptId attempt = itf::test::make_attempt_id(34, 1);
  CoordinatorEpoch first_epoch;

  {
    auto service = std::make_unique<service::CoordinatorService>(make_config(),
                                                                 &steady_clock_singleton());
    ITF_REQUIRE_STATUS_OK(service->start());
    net::ClientConfig client_config;
    client_config.port = service->server().bound_port();
    client_config.peer_name = "restart-client";
    client_config.auth_token = kToken;
    auto client = net::Client::connect(client_config);
    ITF_REQUIRE(client.ok());
    ITF_REQUIRE(client.value().advance_model_generation(kModelHash, std::nullopt, false).ok());
    ITF_REQUIRE(client.value().register_request(request, 0).ok());
    const auto registration = client.value().register_attempt(request, attempt, kModelHash);
    ITF_REQUIRE(registration.ok());
    CompletionPublication publication;
    publication.request = request;
    publication.attempt = attempt;
    publication.attempt_generation = registration.value().generation;
    publication.bytes_in = 1024;
    const auto outcome = client.value().publish_completion(publication);
    ITF_REQUIRE(outcome.ok());
    ITF_CHECK(outcome.value().committed());
    first_epoch = service->coordinator().epoch();
    service->stop();
  }

  auto restored = std::make_unique<service::CoordinatorService>(make_config(),
                                                                &steady_clock_singleton());
  ITF_REQUIRE_STATUS_OK(restored->start());
  ITF_CHECK(restored->coordinator().epoch() > first_epoch);
  const auto record = restored->coordinator().request_record(request);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK_EQ(static_cast<int>(record->lifecycle), static_cast<int>(RequestLifecycle::Completed));
  ITF_CHECK_EQ(record->bytes_in, 1024ULL);
  const auto snapshot = restored->coordinator().accounting();
  ITF_CHECK_EQ(snapshot.retained_requests, static_cast<std::size_t>(1));
  restored->stop();
  restored.reset();
  itf::test::remove_dir(directory);
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_integration_lifecycle", argc, argv);
}
