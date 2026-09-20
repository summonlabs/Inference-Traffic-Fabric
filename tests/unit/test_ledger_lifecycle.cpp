// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "itf/ledger.hpp"
#include "support/test.hpp"
#include "support/test_support.hpp"

namespace {

using namespace itf;

const RequestId kRequest = itf::test::make_request_id(1, 1);
const AttemptId kAttempt = itf::test::make_attempt_id(1, 1);
const AttemptId kSecondAttempt = itf::test::make_attempt_id(1, 2);

ITF_TEST(register_and_attempt_binding) {
  RequestLedger ledger;
  const auto generation = ledger.register_request(kRequest, 100);
  ITF_REQUIRE(generation.ok());
  ITF_CHECK_EQ(generation.value().value(), 1ULL);
  ITF_CHECK(ledger.contains(kRequest));
  ITF_CHECK_EQ(static_cast<int>(ledger.register_request(kRequest, 100).code()),
               static_cast<int>(StatusCode::AlreadyExists));

  const auto attempt = ledger.register_attempt(kRequest, kAttempt, 0xAAULL, ModelGeneration::from(1),
                                               RouteDecisionGeneration::from(1), 110);
  ITF_REQUIRE(attempt.ok());
  ITF_CHECK_EQ(attempt.value().value(), 1ULL);
  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK_EQ(static_cast<int>(record->stage), static_cast<int>(ServingStage::Unknown));
  ITF_CHECK_EQ(static_cast<int>(record->lifecycle), static_cast<int>(RequestLifecycle::Registered));
  ITF_CHECK(record->is_current_attempt(kAttempt));
}

ITF_TEST(registering_without_a_request_is_refused) {
  RequestLedger ledger;
  const auto attempt = ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                               RouteDecisionGeneration::from(1), 0);
  ITF_CHECK_EQ(static_cast<int>(attempt.code()), static_cast<int>(StatusCode::NotFound));
  ITF_CHECK_EQ(static_cast<int>(ledger.register_request(RequestId(), 0).code()),
               static_cast<int>(StatusCode::InvalidArgument));
}

ITF_TEST(stage_progression_and_illegal_transitions) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  ITF_REQUIRE_STATUS_OK(ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 1));
  ITF_REQUIRE_STATUS_OK(ledger.advance_stage(kRequest, kAttempt, ServingStage::PrefillQueued, 2));
  ITF_REQUIRE_STATUS_OK(ledger.advance_stage(kRequest, kAttempt, ServingStage::PrefillRunning, 3));
  ITF_CHECK_EQ(
      static_cast<int>(
          ledger.advance_stage(kRequest, kAttempt, ServingStage::Streaming, 4).code()),
      static_cast<int>(StatusCode::InvalidTransition));
  ITF_CHECK_EQ(static_cast<int>(
                   ledger.advance_stage(kRequest, kAttempt, ServingStage::Completed, 5).code()),
               static_cast<int>(StatusCode::InvalidArgument));
  ITF_CHECK_EQ(static_cast<int>(
                   ledger.advance_stage(kRequest, kAttempt, ServingStage::Unknown, 5).code()),
               static_cast<int>(StatusCode::InvalidArgument));
  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK_EQ(static_cast<int>(record->stage), static_cast<int>(ServingStage::PrefillRunning));
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());
}

ITF_TEST(attempt_identity_isolation) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  (void)ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 1);
  const auto replaced = ledger.supersede_attempt(kRequest, kAttempt, kSecondAttempt, 2);
  ITF_REQUIRE(replaced.ok());
  ITF_CHECK_EQ(replaced.value().value(), 2ULL);

  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK(record->is_current_attempt(kSecondAttempt));
  ITF_CHECK(!record->is_current_attempt(kAttempt));
  ITF_CHECK_EQ(record->generation.value(), 2ULL);
  ITF_CHECK_EQ(static_cast<int>(ledger.advance_stage(kRequest, kAttempt, ServingStage::Routing, 3)
                                    .code()),
               static_cast<int>(StatusCode::StaleGeneration));
  // A replacement attempt starts with no established serving stage.
  ITF_CHECK_EQ(static_cast<int>(
                   ledger.advance_stage(kRequest, kSecondAttempt, ServingStage::Routing, 3).code()),
               static_cast<int>(StatusCode::InvalidTransition));
  ITF_CHECK_EQ(static_cast<int>(
                   ledger.advance_stage(kRequest, kSecondAttempt, ServingStage::Admitted, 3).code()),
               static_cast<int>(StatusCode::Ok));
}

ITF_TEST(cancellation_is_terminal_and_idempotent) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  ITF_REQUIRE_STATUS_OK(
      ledger.cancel_request(kRequest, ReasonCode::Cancelled, AuthoritySequence::from(5), 1));
  ITF_CHECK(ledger.is_cancelled(kRequest));
  ITF_REQUIRE_STATUS_OK(
      ledger.cancel_request(kRequest, ReasonCode::Cancelled, AuthoritySequence::from(5), 2));
  ITF_CHECK_EQ(static_cast<int>(ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 3)
                                    .code()),
               static_cast<int>(StatusCode::Cancelled));
  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK_EQ(record->authority_floor.value(), 5ULL);
  ITF_CHECK(record->accounting_closed);
}

ITF_TEST(completion_publication_dispositions) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  (void)ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 1);

  CompletionPublication publication;
  publication.request = kRequest;
  publication.attempt = kAttempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.bytes_in = 100;
  publication.bytes_out = 50;

  const auto committed = ledger.publish_completion(publication, 2);
  ITF_REQUIRE(committed.ok());
  ITF_CHECK(committed.value().committed());
  ITF_CHECK_EQ(committed.value().reason, ReasonCode::CommittedCompletion);

  const auto duplicate = ledger.publish_completion(publication, 3);
  ITF_REQUIRE(duplicate.ok());
  ITF_CHECK_EQ(static_cast<int>(duplicate.value().disposition),
               static_cast<int>(PublishOutcome::Disposition::DuplicateSuppressed));
  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK_EQ(record->bytes_in, 100ULL);
  ITF_CHECK_EQ(record->bytes_out, 50ULL);
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());
}

ITF_TEST(completion_for_a_cancelled_request_is_refused) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  (void)ledger.cancel_request(kRequest, ReasonCode::Cancelled, AuthoritySequence::from(9), 1);
  CompletionPublication publication;
  publication.request = kRequest;
  publication.attempt = kAttempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  const auto outcome = ledger.publish_completion(publication, 2);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().refused());
  ITF_CHECK_EQ(outcome.value().reason, ReasonCode::RejectedRequestCancelled);
  ITF_CHECK_EQ(static_cast<int>(outcome.value().refusal), static_cast<int>(StatusCode::Cancelled));
}

ITF_TEST(completion_generation_mismatch_is_refused) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  CompletionPublication publication;
  publication.request = kRequest;
  publication.attempt = kAttempt;
  publication.attempt_generation = AttemptGeneration::from(7);
  const auto outcome = ledger.publish_completion(publication, 1);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().refused());
  ITF_CHECK_EQ(outcome.value().reason, ReasonCode::RejectedStaleAttempt);
}

ITF_TEST(completion_for_a_superseded_attempt_is_refused) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  (void)ledger.supersede_attempt(kRequest, kAttempt, kSecondAttempt, 1);
  CompletionPublication publication;
  publication.request = kRequest;
  publication.attempt = kAttempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  const auto outcome = ledger.publish_completion(publication, 2);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().refused());
  ITF_CHECK_EQ(outcome.value().reason, ReasonCode::RejectedStaleAttempt);
}

ITF_TEST(flow_accounting_closes_on_completion_and_cancellation) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  (void)ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 1);
  const auto flow = ledger.open_flow(kRequest, kAttempt, TrafficClass::RequestIngress, 4096, 2);
  ITF_REQUIRE(flow.ok());
  ITF_CHECK_EQ(ledger.accounting().active_flows_total, 1U);
  ITF_REQUIRE_STATUS_OK(ledger.record_flow_bytes(flow.value(), 4096, true));
  const auto mid = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(mid.has_value());
  ITF_CHECK_EQ(mid->bytes_in, 4096ULL);

  CompletionPublication publication;
  publication.request = kRequest;
  publication.attempt = kAttempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  const auto outcome = ledger.publish_completion(publication, 3);
  ITF_REQUIRE(outcome.ok());
  ITF_CHECK(outcome.value().committed());
  ITF_CHECK_EQ(ledger.accounting().active_flows_total, 0U);
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());

  const auto cancelled = ledger.register_request(itf::test::make_request_id(2, 1), 4);
  ITF_REQUIRE(cancelled.ok());
  const RequestId second = itf::test::make_request_id(2, 1);
  (void)ledger.register_attempt(second, itf::test::make_attempt_id(2, 1), 0,
                                ModelGeneration::from(1), RouteDecisionGeneration::from(1), 4);
  (void)ledger.advance_stage(second, itf::test::make_attempt_id(2, 1), ServingStage::Admitted, 5);
  ITF_REQUIRE(
      ledger.open_flow(second, itf::test::make_attempt_id(2, 1), TrafficClass::RequestIngress, 1, 6)
          .ok());
  ITF_CHECK_EQ(ledger.accounting().active_flows_total, 1U);
  ITF_REQUIRE_STATUS_OK(
      ledger.cancel_request(second, ReasonCode::Cancelled, AuthoritySequence::from(2), 7));
  ITF_CHECK_EQ(ledger.accounting().active_flows_total, 0U);
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());
}

ITF_TEST(flow_limits_are_enforced) {
  LedgerLimits limits;
  limits.max_active_flows_per_request = 2;
  RequestLedger ledger(limits);
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  (void)ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 1);
  ITF_REQUIRE(ledger.open_flow(kRequest, kAttempt, TrafficClass::RequestIngress, 1, 2).ok());
  ITF_REQUIRE(ledger.open_flow(kRequest, kAttempt, TrafficClass::RequestIngress, 1, 3).ok());
  const auto third = ledger.open_flow(kRequest, kAttempt, TrafficClass::RequestIngress, 1, 4);
  ITF_CHECK_EQ(static_cast<int>(third.code()), static_cast<int>(StatusCode::CapacityExhausted));
}

ITF_TEST(releasing_an_unknown_flow_is_a_deterministic_error) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  ITF_CHECK_EQ(static_cast<int>(ledger.release_flow(FlowId(1, 1), 0).code()),
               static_cast<int>(StatusCode::NotFound));
  ITF_CHECK_EQ(static_cast<int>(ledger.record_flow_bytes(FlowId(1, 1), 1, true).code()),
               static_cast<int>(StatusCode::NotFound));
}

ITF_TEST(live_request_limit_is_enforced) {
  LedgerLimits limits;
  limits.max_live_requests = 3;
  RequestLedger ledger(limits);
  ITF_REQUIRE(ledger.register_request(itf::test::make_request_id(3, 1), 0).ok());
  ITF_REQUIRE(ledger.register_request(itf::test::make_request_id(3, 2), 0).ok());
  ITF_REQUIRE(ledger.register_request(itf::test::make_request_id(3, 3), 0).ok());
  ITF_CHECK_EQ(
      static_cast<int>(ledger.register_request(itf::test::make_request_id(3, 4), 0).code()),
      static_cast<int>(StatusCode::CapacityExhausted));
}

ITF_TEST(attempt_ceiling_is_enforced) {
  LedgerLimits limits;
  limits.max_attempts_per_request = 2;
  RequestLedger ledger(limits);
  (void)ledger.register_request(kRequest, 0);
  ITF_REQUIRE(ledger
                  .register_attempt(kRequest, itf::test::make_attempt_id(4, 1), 0,
                                    ModelGeneration::from(1), RouteDecisionGeneration::from(1), 0)
                  .ok());
  ITF_REQUIRE(ledger
                  .register_attempt(kRequest, itf::test::make_attempt_id(4, 2), 0,
                                    ModelGeneration::from(1), RouteDecisionGeneration::from(1), 0)
                  .ok());
  ITF_CHECK_EQ(static_cast<int>(ledger
                                    .register_attempt(kRequest, itf::test::make_attempt_id(4, 3), 0,
                                                      ModelGeneration::from(1),
                                                      RouteDecisionGeneration::from(1), 0)
                                    .code()),
               static_cast<int>(StatusCode::CapacityExhausted));
}

ITF_TEST(retention_evicts_the_oldest_terminal_records) {
  LedgerLimits limits;
  limits.max_retained_terminal_requests = 2;
  RequestLedger ledger(limits);
  for (std::uint64_t index = 0; index < 5; ++index) {
    const RequestId request = itf::test::make_request_id(5, index);
    const AttemptId attempt = itf::test::make_attempt_id(5, index);
    ITF_REQUIRE(ledger.register_request(request, 0).ok());
    ITF_REQUIRE(ledger
                    .register_attempt(request, attempt, 0, ModelGeneration::from(1),
                                      RouteDecisionGeneration::from(1), 0)
                    .ok());
    CompletionPublication publication;
    publication.request = request;
    publication.attempt = attempt;
    publication.attempt_generation = AttemptGeneration::from(1);
    const auto outcome = ledger.publish_completion(publication, 0);
    ITF_REQUIRE(outcome.ok());
    ITF_CHECK(outcome.value().committed());
  }
  ITF_CHECK_EQ(ledger.accounting().requests_evicted, 3ULL);
  ITF_CHECK_EQ(ledger.accounting().retained_requests, static_cast<std::size_t>(2));
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());
}

ITF_TEST(interrupted_requests_require_revalidation) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  ITF_REQUIRE_STATUS_OK(ledger.mark_request_interrupted(kRequest, 1));
  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK(record->requires_revalidation);
  ITF_CHECK_EQ(static_cast<int>(record->lifecycle), static_cast<int>(RequestLifecycle::Interrupted));
  ITF_CHECK_EQ(static_cast<int>(ledger.advance_stage(kRequest, kAttempt, ServingStage::Admitted, 2)
                                    .code()),
               static_cast<int>(StatusCode::RevalidationRequired));
  const auto revalidated = ledger.revalidate_request(kRequest, kSecondAttempt, 3);
  ITF_REQUIRE(revalidated.ok());
  ITF_REQUIRE_STATUS_OK(ledger.advance_stage(kRequest, kSecondAttempt, ServingStage::Admitted, 4));
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());
}

ITF_TEST(failure_publication_is_recorded_once) {
  RequestLedger ledger;
  (void)ledger.register_request(kRequest, 0);
  (void)ledger.register_attempt(kRequest, kAttempt, 0, ModelGeneration::from(1),
                                RouteDecisionGeneration::from(1), 0);
  CompletionPublication publication;
  publication.request = kRequest;
  publication.attempt = kAttempt;
  publication.attempt_generation = AttemptGeneration::from(1);
  publication.success = false;
  publication.reason = ReasonCode::AttemptFailed;
  const auto first = ledger.publish_completion(publication, 1);
  ITF_REQUIRE(first.ok());
  ITF_CHECK(first.value().committed());
  ITF_CHECK_EQ(first.value().reason, ReasonCode::AttemptFailed);
  const auto second = ledger.publish_completion(publication, 2);
  ITF_REQUIRE(second.ok());
  ITF_CHECK_EQ(static_cast<int>(second.value().disposition),
               static_cast<int>(PublishOutcome::Disposition::Refused));
  const auto record = ledger.snapshot_request(kRequest);
  ITF_REQUIRE(record.has_value());
  ITF_CHECK_EQ(static_cast<int>(record->lifecycle), static_cast<int>(RequestLifecycle::Failed));
  ITF_REQUIRE_STATUS_OK(ledger.verify_closure());
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_ledger_lifecycle", argc, argv); }
