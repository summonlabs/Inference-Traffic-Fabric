// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <set>
#include <string>

#include "itf/coordinator.hpp"
#include "itf/serialize.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

constexpr std::uint64_t kModelHash = 0x1234567890ABCDEFULL;
constexpr std::uint64_t kStateHash = 0xFEDCBA0987654321ULL;

struct Fixture {
  VirtualClock clock;
  Coordinator coordinator;

  Fixture() : coordinator(make_config(), &clock) {
    (void)coordinator.boot_fresh();
    TopologyEvidence topology;
    topology.provenance = EvidenceProvenance::Measured;
    topology.node_count = 1;
    topology.accelerator_count = 1;
    topology.link_count = 0;
    topology.fabric_bytes_per_sec = 25000000000ULL;
    topology.max_age_nanos = 1000000000ULL;
    (void)coordinator.publish_topology(topology);
    (void)coordinator.set_model_generation(kModelHash, std::nullopt);
    (void)coordinator.advance_state_generation(kStateHash, std::nullopt);
  }

  static CoordinatorConfig make_config() {
    CoordinatorConfig config;
    config.policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
    config.boot_seed = 4242;
    return config;
  }

  RequestId request = itf::test::make_request_id(11, 1);
  AttemptId attempt = itf::test::make_attempt_id(11, 1);

  void prepare() {
    (void)coordinator.register_request(request);
    (void)coordinator.register_attempt(request, attempt, kModelHash);
    (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
    (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
    (void)coordinator.advance_stage(request, attempt, ServingStage::Admitted);
  }

  TrafficRequest message() const {
    TrafficRequest request_message;
    TrafficSubject subject;
    subject.kind = SubjectKind::Serving;
    subject.request = request;
    subject.attempt = attempt;
    subject.request_generation = RequestGeneration::from(1);
    request_message.subject = subject;
    request_message.declared_class = TrafficClass::RequestIngress;
    request_message.declared_stage = ServingStage::Admitted;
    request_message.payload_bytes = 4096;
    request_message.binding.policy = coordinator.policy().generation();
    request_message.binding.coordinator_epoch = coordinator.epoch();
    request_message.binding.model_generation = ModelGeneration::from(1);
    request_message.binding.route_decision = RouteDecisionGeneration::from(1);
    request_message.binding.slo_contract = SloContractGeneration::from(1);
    request_message.deadline = Instant::from_now(clock.now_nanos(), 20000000);
    return request_message;
  }
};

ITF_TEST(every_reason_code_has_stable_text) {
  std::set<std::string> labels;
  for (std::uint16_t value = 0; value < static_cast<std::uint16_t>(ReasonCode::Count); ++value) {
    const std::string text(to_string(static_cast<ReasonCode>(value)));
    ITF_CHECK(!text.empty());
    ITF_CHECK(text != "UNKNOWN" || value == 0);
    labels.insert(text);
  }
  ITF_CHECK_EQ(labels.size(), static_cast<std::size_t>(ReasonCode::Count));
}

ITF_TEST(reason_code_classification_is_total) {
  for (std::uint16_t value = 0; value < static_cast<std::uint16_t>(ReasonCode::Count); ++value) {
    const ReasonCode reason = static_cast<ReasonCode>(value);
    const int categories = (is_rejection(reason) ? 1 : 0) + (is_deferral(reason) ? 1 : 0) +
                           (is_admission(reason) ? 1 : 0);
    ITF_CHECK(categories <= 1);
  }
  ITF_CHECK(is_rejection(ReasonCode::RejectedRequestCancelled));
  ITF_CHECK(is_deferral(ReasonCode::DeferredBulkYield));
  ITF_CHECK(is_admission(ReasonCode::AdmittedStarvationGuard));
}

ITF_TEST(admitted_decision_records_explanations) {
  Fixture fixture;
  fixture.prepare();
  const Decision decision = fixture.coordinator.evaluate(fixture.message());
  ITF_CHECK_EQ(static_cast<int>(decision.outcome), static_cast<int>(DecisionOutcome::Allowed));
  ITF_CHECK_EQ(decision.reason, ReasonCode::Admitted);
  ITF_CHECK(decision.permits_traffic());
  ITF_CHECK(!decision.is_authority_denial());
  ITF_CHECK(decision.explanation_count > 0);
  ITF_CHECK(decision.explanation_count <= kMaxExplanationSteps);
  ITF_CHECK(decision.authority.is_valid());
  ITF_CHECK(decision.subject.is_valid());
  const std::string rendered = decision.to_string();
  ITF_CHECK(rendered.find("ALLOWED") != std::string::npos);
  ITF_CHECK(rendered.find(fixture.request.to_string()) != std::string::npos);
  const std::string explained = decision.explain();
  ITF_CHECK(explained.find("AUTHORITY_BINDING") != std::string::npos);
}

ITF_TEST(explanation_steps_respect_the_policy_limit) {
  CoordinatorConfig config = Fixture::make_config();
  PolicyConfig policy = config.policy;
  policy.set_max_explanation_steps(2);
  config.policy = policy;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  (void)coordinator.register_request(itf::test::make_request_id(12, 1));
  const Decision decision = coordinator.evaluate(TrafficRequest{});
  ITF_CHECK(decision.explanation_count <= 2);
}

ITF_TEST(rejections_are_authority_denials_where_they_should_be) {
  Fixture fixture;
  fixture.prepare();
  auto message = fixture.message();
  message.binding.coordinator_epoch = CoordinatorEpoch::from(9);
  const Decision stale = fixture.coordinator.evaluate(message);
  ITF_CHECK_EQ(stale.outcome, DecisionOutcome::Rejected);
  ITF_CHECK(stale.is_authority_denial());

  auto unknown = fixture.message();
  unknown.subject = TrafficSubject();
  unknown.subject->kind = SubjectKind::Serving;
  unknown.subject->request = itf::test::make_request_id(99, 1);
  unknown.subject->attempt = itf::test::make_attempt_id(99, 1);
  unknown.subject->request_generation = RequestGeneration::from(1);
  const Decision missing = fixture.coordinator.evaluate(unknown);
  ITF_CHECK_EQ(missing.reason, ReasonCode::RejectedUnknownRequest);
  ITF_CHECK(missing.is_authority_denial());
}

ITF_TEST(structurally_invalid_requests_are_rejected_without_a_subject) {
  Fixture fixture;
  TrafficRequest invalid;
  const Decision decision = fixture.coordinator.evaluate(invalid);
  ITF_CHECK_EQ(decision.outcome, DecisionOutcome::Rejected);
  ITF_CHECK_EQ(decision.reason, ReasonCode::RejectedSubjectKindMismatch);
  ITF_CHECK(!decision.subject.is_valid());

  // A decision without a resolvable subject must still round-trip on the wire.
  Writer writer;
  encode(writer, decision);
  ITF_REQUIRE(writer.ok());
  Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
  const auto decoded = decode_decision(reader);
  ITF_REQUIRE(decoded.ok());
  ITF_REQUIRE_STATUS_OK(reader.finish());
  ITF_CHECK_EQ(static_cast<int>(decoded.value().outcome), static_cast<int>(DecisionOutcome::Rejected));
  ITF_CHECK_EQ(decoded.value().reason, ReasonCode::RejectedSubjectKindMismatch);
}

ITF_TEST(decisions_round_trip_through_the_canonical_encoding) {
  Fixture fixture;
  fixture.prepare();
  const Decision decision = fixture.coordinator.evaluate(fixture.message());
  Writer writer;
  encode(writer, decision);
  ITF_REQUIRE(writer.ok());
  Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
  const auto decoded = decode_decision(reader);
  ITF_REQUIRE(decoded.ok());
  ITF_REQUIRE_STATUS_OK(reader.finish());
  ITF_CHECK_EQ(decoded.value().to_string(), decision.to_string());
  ITF_CHECK_EQ(decoded.value().explanation_count, decision.explanation_count);
}

ITF_TEST(deferral_carries_a_hint_and_a_hold_time) {
  CoordinatorConfig config = Fixture::make_config();
  PolicyConfig policy = config.policy;
  ClassPolicyRule streaming = policy.rule(TrafficClass::DecodeStream);
  streaming.reserved_weight = 0;
  streaming.max_defer_count = 100;
  streaming.max_defer_horizon_nanos = 1000000000ULL;
  (void)policy.set_rule(TrafficClass::DecodeStream, streaming);
  policy.set_congestion_queue_depth(1);
  config.policy = policy;
  VirtualClock clock;
  Coordinator coordinator(config, &clock);
  (void)coordinator.boot_fresh();
  TopologyEvidence topology;
  topology.provenance = EvidenceProvenance::Synthetic;
  topology.node_count = 2;
  topology.link_count = 1;
  topology.queue_depth = 32;
  topology.congestion_signalled = true;
  topology.fabric_bytes_per_sec = 1000000000ULL;
  topology.max_age_nanos = 1000000000ULL;
  (void)coordinator.publish_topology(topology);
  (void)coordinator.set_model_generation(kModelHash, std::nullopt);
  const RequestId request = itf::test::make_request_id(13, 1);
  const AttemptId attempt = itf::test::make_attempt_id(13, 1);
  (void)coordinator.register_request(request);
  (void)coordinator.register_attempt(request, attempt, kModelHash);
  (void)coordinator.issue_route_decision(request, attempt, 0, false, false);
  (void)coordinator.register_slo_contract(request, 20000000ULL, 20000000ULL, 200);
  (void)coordinator.advance_stage(request, attempt, ServingStage::Admitted);
  (void)coordinator.advance_stage(request, attempt, ServingStage::PrefillQueued);
  (void)coordinator.advance_stage(request, attempt, ServingStage::PrefillRunning);
  (void)coordinator.advance_stage(request, attempt, ServingStage::HandoffPending);
  (void)coordinator.advance_stage(request, attempt, ServingStage::DecodeQueued);
  (void)coordinator.advance_stage(request, attempt, ServingStage::DecodeRunning);
  (void)coordinator.advance_stage(request, attempt, ServingStage::Streaming);

  TrafficRequest message;
  TrafficSubject subject;
  subject.kind = SubjectKind::Serving;
  subject.request = request;
  subject.attempt = attempt;
  subject.request_generation = RequestGeneration::from(1);
  message.subject = subject;
  message.declared_class = TrafficClass::DecodeStream;
  message.declared_stage = ServingStage::Streaming;
  message.payload_bytes = 4096;
  message.binding.policy = coordinator.policy().generation();
  message.binding.coordinator_epoch = coordinator.epoch();
  message.binding.slo_contract = SloContractGeneration::from(1);
  message.binding.route_decision = RouteDecisionGeneration::from(1);
  message.binding.model_generation = ModelGeneration::from(1);
  message.deadline = Instant::from_now(clock.now_nanos(), 20000000);

  const Decision decision = coordinator.evaluate(message);
  ITF_CHECK_EQ(decision.outcome, DecisionOutcome::Deferred);
  ITF_CHECK_EQ(decision.reason, ReasonCode::DeferredCongestion);
  ITF_CHECK(decision.defer_hint_nanos > 0);
  ITF_CHECK(decision.treatment.hold_until.is_set());
  ITF_CHECK_EQ(decision.treatment.defer_count, 1U);
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_decision_explain", argc, argv); }
