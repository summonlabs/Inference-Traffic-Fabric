// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <string>
#include <string_view>

#include "itf/traffic.hpp"
#include "support/test.hpp"

namespace {

using namespace itf;

ITF_TEST(every_traffic_class_has_a_stable_label) {
  const std::array<std::string_view, kTrafficClassCount> expected = {
      "UNKNOWN",       "REQUEST_INGRESS",    "PREFILL_INPUT",  "PREFILL_DECODE_HANDOFF",
      "KV_TRANSFER",   "STATE_TRANSFER",     "MODEL_TRANSFER", "ADAPTER_TRANSFER",
      "DECODE_STREAM", "SPECULATIVE_BRANCH", "RESPONSE_EGRESS", "CONTROL"};
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    const auto traffic = static_cast<TrafficClass>(index);
    ITF_CHECK_EQ(std::string(label(traffic)), std::string(expected[index]));
    const auto parsed = traffic_class_from_label(label(traffic));
    ITF_REQUIRE(parsed.has_value());
    ITF_CHECK(*parsed == traffic);
  }
  ITF_CHECK(!traffic_class_from_label("NOT_A_CLASS").has_value());
}

ITF_TEST(every_stage_has_a_stable_label) {
  for (std::size_t index = 0; index < kServingStageCount; ++index) {
    const auto stage = static_cast<ServingStage>(index);
    const auto parsed = serving_stage_from_label(label(stage));
    ITF_REQUIRE(parsed.has_value());
    ITF_CHECK(*parsed == stage);
  }
  ITF_CHECK(!serving_stage_from_label("NOT_A_STAGE").has_value());
}

ITF_TEST(stage_terminality_and_phase_classification) {
  ITF_CHECK(is_terminal(ServingStage::Completed));
  ITF_CHECK(is_terminal(ServingStage::Cancelled));
  ITF_CHECK(is_terminal(ServingStage::Failed));
  ITF_CHECK(!is_terminal(ServingStage::Streaming));
  ITF_CHECK(!is_terminal(ServingStage::Unknown));
  ITF_CHECK(is_prefill_stage(ServingStage::PrefillRunning));
  ITF_CHECK(is_decode_stage(ServingStage::DecodeRunning));
  ITF_CHECK(is_decode_stage(ServingStage::Streaming));
  ITF_CHECK(is_serving_active(ServingStage::Admitted));
  ITF_CHECK(!is_serving_active(ServingStage::Unknown));
  ITF_CHECK(!is_serving_active(ServingStage::Completed));
}

ITF_TEST(stage_transitions_are_ordered) {
  ITF_CHECK(is_legal_stage_transition(ServingStage::Unknown, ServingStage::Admitted));
  ITF_CHECK(is_legal_stage_transition(ServingStage::Admitted, ServingStage::Routing));
  ITF_CHECK(is_legal_stage_transition(ServingStage::PrefillRunning, ServingStage::HandoffPending));
  ITF_CHECK(is_legal_stage_transition(ServingStage::HandoffPending, ServingStage::DecodeQueued));
  ITF_CHECK(is_legal_stage_transition(ServingStage::DecodeRunning, ServingStage::Streaming));
  ITF_CHECK(is_legal_stage_transition(ServingStage::Streaming, ServingStage::Completing));
  ITF_CHECK(is_legal_stage_transition(ServingStage::Completing, ServingStage::Completed));
  ITF_CHECK(is_legal_stage_transition(ServingStage::PrefillRunning, ServingStage::PrefillRunning));

  ITF_CHECK(!is_legal_stage_transition(ServingStage::Admitted, ServingStage::Streaming));
  ITF_CHECK(!is_legal_stage_transition(ServingStage::Completed, ServingStage::Streaming));
  ITF_CHECK(!is_legal_stage_transition(ServingStage::Cancelled, ServingStage::Admitted));
  ITF_CHECK(!is_legal_stage_transition(ServingStage::Admitted, ServingStage::Unknown));
  ITF_CHECK(!is_legal_stage_transition(ServingStage::Streaming, ServingStage::PrefillQueued));
}

ITF_TEST(canonical_class_for_stage) {
  ITF_CHECK(canonical_class_for_stage(ServingStage::Admitted) == TrafficClass::RequestIngress);
  ITF_CHECK(canonical_class_for_stage(ServingStage::PrefillRunning) == TrafficClass::PrefillInput);
  ITF_CHECK(canonical_class_for_stage(ServingStage::HandoffPending) ==
            TrafficClass::PrefillDecodeHandoff);
  ITF_CHECK(canonical_class_for_stage(ServingStage::DecodeRunning) == TrafficClass::DecodeStream);
  ITF_CHECK(canonical_class_for_stage(ServingStage::Completing) == TrafficClass::ResponseEgress);
  ITF_CHECK(canonical_class_for_stage(ServingStage::Unknown) == TrafficClass::Unknown);
  ITF_CHECK(canonical_class_for_stage(ServingStage::Completed) == TrafficClass::Unknown);
}

ITF_TEST(class_legality_is_stage_dependent) {
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::RequestIngress, ServingStage::Admitted));
  ITF_CHECK(!is_class_legal_for_stage(TrafficClass::DecodeStream, ServingStage::Admitted));
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::PrefillInput, ServingStage::PrefillQueued));
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::KvTransfer, ServingStage::HandoffPending));
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::StateTransfer, ServingStage::HandoffPending));
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::DecodeStream, ServingStage::Streaming));
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::ResponseEgress, ServingStage::Completing));
  ITF_CHECK(is_class_legal_for_stage(TrafficClass::Control, ServingStage::Streaming));
  ITF_CHECK(!is_class_legal_for_stage(TrafficClass::Control, ServingStage::Unknown));
  ITF_CHECK(!is_class_legal_for_stage(TrafficClass::RequestIngress, ServingStage::Completed));
  ITF_CHECK(!is_class_legal_for_stage(TrafficClass::ModelTransfer, ServingStage::Streaming));
}

ITF_TEST(subject_validity) {
  TrafficSubject serving;
  serving.kind = SubjectKind::Serving;
  serving.request = RequestId(1, 2);
  serving.attempt = AttemptId(3, 4);
  serving.request_generation = RequestGeneration::from(1);
  ITF_CHECK(serving.is_valid());

  TrafficSubject missing_generation = serving;
  missing_generation.request_generation = RequestGeneration();
  ITF_CHECK(!missing_generation.is_valid());

  TrafficSubject transfer;
  transfer.kind = SubjectKind::Transfer;
  transfer.transfer = StateTransferId(5, 6);
  ITF_CHECK(transfer.is_valid());

  TrafficSubject nil_transfer;
  nil_transfer.kind = SubjectKind::Transfer;
  ITF_CHECK(!nil_transfer.is_valid());

  ITF_CHECK(!TrafficSubject().is_valid());
}

ITF_TEST(request_structural_validation) {
  TrafficRequest request;
  request.declared_class = TrafficClass::ModelTransfer;
  ITF_CHECK(request.structurally_valid());

  TrafficRequest subjectless_serving;
  subjectless_serving.declared_class = TrafficClass::DecodeStream;
  ITF_CHECK(!subjectless_serving.structurally_valid());

  TrafficRequest inconsistent;
  inconsistent.declared_class = TrafficClass::ModelTransfer;
  inconsistent.total_chunks = 4;
  inconsistent.chunk_index = 4;
  ITF_CHECK(!inconsistent.structurally_valid());

  TrafficRequest out_of_range;
  out_of_range.declared_class = TrafficClass::ModelTransfer;
  out_of_range.declared_stage = ServingStage::Count;
  ITF_CHECK(!out_of_range.structurally_valid());
}

ITF_TEST(generation_binding_rendering_is_stable) {
  GenerationBinding binding;
  ITF_CHECK_EQ(binding.to_string(), std::string("none"));
  binding.model_generation = ModelGeneration::from(3);
  binding.policy = PolicyGeneration::from(2);
  ITF_CHECK_EQ(binding.to_string(), std::string("model=3 policy=2 "));
}

}  // namespace

int main(int argc, char** argv) {
  return itf::test::run_all("test_traffic_classification", argc, argv);
}
