// Inference Traffic Fabric - example: coordinator over real loopback TCP.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "itf/client.hpp"
#include "itf/coordinator.hpp"
#include "itf/serialize.hpp"
#include "itf/session.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x2222333344445555ULL;

/// Minimal hosting of a coordinator behind the framed transport. The tools
/// ship a fuller service; this example keeps the wiring visible.
class ExampleHandler final : public net::MessageHandler {
 public:
  ExampleHandler(Coordinator& coordinator, std::string token)
      : coordinator_(coordinator), token_(std::move(token)) {}

  Result<std::vector<std::uint8_t>> handle(Context& context, wire::MessageType type,
                                           ByteSpan payload) override {
    (void)context;
    switch (type) {
      case wire::MessageType::RegisterRequest: {
        Reader reader(payload);
        auto body = wire::RegisterRequestMessage::decode(reader);
        if (!body.ok()) return fail(body.status());
        auto generation = coordinator_.register_request(body.value().request);
        if (!generation.ok()) return fail(generation.status());
        wire::RegisterRequestResponse response;
        response.request = body.value().request;
        response.generation = generation.value();
        response.authority = coordinator_.authority();
        return encode([&](Writer& writer) { response.encode(writer); });
      }
      case wire::MessageType::RegisterAttempt: {
        Reader reader(payload);
        auto body = wire::RegisterAttemptMessage::decode(reader);
        if (!body.ok()) return fail(body.status());
        auto generation = coordinator_.register_attempt(body.value().request, body.value().attempt,
                                                        body.value().model_hash);
        if (!generation.ok()) return fail(generation.status());
        wire::RegisterAttemptResponse response;
        response.request = body.value().request;
        response.attempt = body.value().attempt;
        response.generation = generation.value();
        response.authority = coordinator_.authority();
        return encode([&](Writer& writer) { response.encode(writer); });
      }
      case wire::MessageType::StageUpdate: {
        Reader reader(payload);
        auto body = wire::StageUpdateMessage::decode(reader);
        if (!body.ok()) return fail(body.status());
        const Status advanced = coordinator_.advance_stage(body.value().request,
                                                           body.value().attempt,
                                                           body.value().stage);
        if (!advanced.ok()) return fail(advanced);
        wire::SimpleAck ack;
        return encode([&](Writer& writer) { ack.encode(writer); });
      }
      case wire::MessageType::EvaluateTraffic: {
        Reader reader(payload);
        auto body = wire::EvaluateTrafficMessage::decode(reader);
        if (!body.ok()) return fail(body.status());
        wire::DecisionResponseMessage response;
        response.decision = coordinator_.evaluate(body.value().request);
        return encode([&](Writer& writer) { response.encode(writer); });
      }
      case wire::MessageType::CompletionPublish: {
        Reader reader(payload);
        auto body = wire::CompletionPublishMessage::decode(reader);
        if (!body.ok()) return fail(body.status());
        auto outcome = coordinator_.publish_completion(body.value().publication);
        if (!outcome.ok()) return fail(outcome.status());
        wire::CompletionResponse response;
        response.outcome = outcome.value();
        return encode([&](Writer& writer) { response.encode(writer); });
      }
      case wire::MessageType::AccountingQuery: {
        wire::AccountingResponseMessage response;
        response.snapshot = coordinator_.accounting();
        return encode([&](Writer& writer) { response.encode(writer); });
      }
      default:
        return fail(Status(StatusCode::Unsupported, "example endpoint does not handle this type"));
    }
  }

  void describe_authority(wire::HelloResponse& response) override {
    const AuthorityStamp stamp = coordinator_.authority();
    response.coordinator_boot = stamp.incarnation;
    response.epoch = stamp.epoch;
    response.policy_generation = stamp.policy;
    response.authority_sequence = stamp.sequence.value();
  }

 private:
  static Result<std::vector<std::uint8_t>> fail(const Status& status) {
    return Result<std::vector<std::uint8_t>>::failure(status);
  }

  template <class Fn>
  static Result<std::vector<std::uint8_t>> encode(Fn&& fn) {
    Writer writer;
    fn(writer);
    if (!writer.ok()) {
      return Result<std::vector<std::uint8_t>>::failure(writer.fail_code(), "response too large");
    }
    return Result<std::vector<std::uint8_t>>::success(writer.take());
  }

  Coordinator& coordinator_;
  std::string token_;
};

}  // namespace

int main() {
  const std::string secret = "example-shared-secret";
  VirtualClock clock;
  CoordinatorConfig config;
  config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);

  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Measured;
  topology.node_count = 1;
  topology.link_count = 0;
  topology.accelerator_count = 1;
  topology.fabric_bytes_per_sec = 25000000000ULL;
  topology.max_age_nanos = 5000000000ULL;
  (void)coordinator.publish_topology(topology);

  ExampleHandler handler(coordinator, secret);
  net::ServerConfig server_config;
  server_config.bind_host = "127.0.0.1";
  server_config.port = 0;
  server_config.auth_token = secret;
  net::Server server(server_config, &handler);
  const Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "server start failed: %s\n", started.to_string().c_str());
    return 1;
  }
  std::printf("example coordinator listening on 127.0.0.1:%u\n",
              static_cast<unsigned>(server.bound_port()));

  net::ClientConfig client_config;
  client_config.port = server.bound_port();
  client_config.peer_name = "example-client";
  client_config.auth_token = secret;
  auto client = net::Client::connect(client_config);
  if (!client.ok()) {
    std::fprintf(stderr, "connect failed: %s\n", client.status().to_string().c_str());
    server.stop();
    return 1;
  }
  std::printf("handshake: epoch=%llu policy=%llu\n",
              static_cast<unsigned long long>(client.value().hello().epoch.value()),
              static_cast<unsigned long long>(client.value().hello().policy_generation.value()));

  const RequestId request(0x777788889999AAAAULL, 0xBBBBCCCCDDDDEEEEULL);
  const AttemptId attempt(0x1234123412341234ULL, 0x5678567856785678ULL);
  (void)client.value().register_request(request, 5000000000ULL);
  const auto registered = client.value().register_attempt(request, attempt, kModelHash);
  if (!registered.ok()) {
    std::fprintf(stderr, "register_attempt failed: %s\n", registered.status().to_string().c_str());
    server.stop();
    return 1;
  }
  (void)client.value().advance_stage(request, attempt, ServingStage::Admitted);

  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::RequestIngress;
  message.declared_stage = ServingStage::Admitted;
  message.payload_bytes = 8192;
  message.binding.policy = client.value().hello().policy_generation;
  message.binding.coordinator_epoch = client.value().hello().epoch;

  // The route decision and the SLO contract are owned by adjacent systems; the
  // example binds them through the local coordinator for brevity.
  (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
  (void)coordinator.register_slo_contract(request, 30000000ULL, 30000000ULL, 200);
  message.binding.route_decision = coordinator.request_record(request).value().current_attempt.is_nil()
                                       ? std::optional<RouteDecisionGeneration>()
                                       : coordinator.issue_route_decision(request, attempt, 0, false, false)
                                             .value()
                                             .generation;
  message.binding.slo_contract =
      coordinator.register_slo_contract(request, 30000000ULL, 30000000ULL, 200).value().generation;

  const auto decision = client.value().evaluate(message);
  if (!decision.ok()) {
    std::fprintf(stderr, "evaluate failed: %s\n", decision.status().to_string().c_str());
    server.stop();
    return 1;
  }
  std::printf("remote decision: %s\n", decision.value().to_string().c_str());

  CompletionPublication publication;
  publication.request = request;
  publication.attempt = attempt;
  publication.attempt_generation = registered.value().generation;
  publication.bytes_in = 8192;
  const auto outcome = client.value().publish_completion(publication);
  std::printf("remote completion: disposition=%u\n",
              static_cast<unsigned>(outcome.value().disposition));

  const auto snapshot = client.value().accounting();
  std::printf("remote accounting: %s\n", snapshot.value().to_string().c_str());
  client.value().close();
  server.stop();
  std::printf("server stats: %s\n", server.stats().to_string().c_str());
  return 0;
}
