// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "itf/policy.hpp"
#include "support/test.hpp"

namespace {

using namespace itf;

ITF_TEST(canonical_defaults_are_valid) {
  const PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ITF_CHECK_EQ(static_cast<int>(policy.validate()), static_cast<int>(PolicyVerdict::Valid));
  ITF_CHECK(policy.reserved_weight_total() <= kReservedWeightScale);
  ITF_CHECK(policy.rule(TrafficClass::DecodeStream).starvation_protected);
  ITF_CHECK(policy.rule(TrafficClass::ModelTransfer).yields_to_latency);
  ITF_CHECK_EQ(policy.rule(TrafficClass::ModelTransfer).reserved_weight, 0U);
  ITF_CHECK(!policy.rule(TrafficClass::Unknown).enabled);
  ITF_CHECK(!policy.allow_unknown_stage());
}

ITF_TEST(canonical_defaults_are_deterministic) {
  const PolicyConfig first = PolicyConfig::canonical_defaults(PolicyGeneration::from(7));
  const PolicyConfig second = PolicyConfig::canonical_defaults(PolicyGeneration::from(7));
  Writer first_writer;
  Writer second_writer;
  first.encode(first_writer);
  second.encode(second_writer);
  ITF_REQUIRE(first_writer.ok());
  ITF_REQUIRE(second_writer.ok());
  ITF_CHECK(first_writer.buffer() == second_writer.buffer());
  ITF_CHECK_EQ(first.to_string(), second.to_string());
}

ITF_TEST(policy_rejects_reserved_weight_overflow) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  for (std::size_t index = 1; index < kTrafficClassCount; ++index) {
    const auto traffic = static_cast<TrafficClass>(index);
    ClassPolicyRule rule = policy.rule(traffic);
    // Bulk classes may never hold reserved weight, so they are left alone.
    if (rule.yields_to_latency) continue;
    rule.reserved_weight = 900;
    (void)policy.set_rule(traffic, rule);
  }
  ITF_CHECK_EQ(static_cast<int>(policy.validate()),
               static_cast<int>(PolicyVerdict::ReservedWeightOverflow));
  ITF_CHECK_EQ(static_cast<int>(policy.validate_status().code()),
               static_cast<int>(StatusCode::PolicyInvalid));
}

ITF_TEST(policy_rejects_reserved_weight_on_bulk_classes) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule rule = policy.rule(TrafficClass::ModelTransfer);
  rule.reserved_weight = 10;
  (void)policy.set_rule(TrafficClass::ModelTransfer, rule);
  ITF_CHECK_EQ(static_cast<int>(policy.validate()), static_cast<int>(PolicyVerdict::BulkReserved));
}

ITF_TEST(policy_requires_a_conservative_rule_when_unknown_is_allowed) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  policy.set_allow_unknown_stage(true);
  ITF_CHECK_EQ(static_cast<int>(policy.validate()), static_cast<int>(PolicyVerdict::MissingRule));
  ClassPolicyRule conservative;
  conservative.enabled = true;
  conservative.priority = PriorityClass::Background;
  conservative.pacing = PacingClass::BestEffort;
  conservative.integrity = IntegrityClass::ChecksumAndVerify;
  conservative.isolation = IsolationDomain::Background;
  ITF_REQUIRE_STATUS_OK(policy.set_rule(TrafficClass::Unknown, conservative));
  ITF_CHECK_EQ(static_cast<int>(policy.validate()), static_cast<int>(PolicyVerdict::Valid));
}

ITF_TEST(policy_rejects_enabled_rules_without_treatment) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule rule = policy.rule(TrafficClass::Control);
  rule.priority = PriorityClass::Unknown;
  (void)policy.set_rule(TrafficClass::Control, rule);
  ITF_CHECK_EQ(static_cast<int>(policy.validate()),
               static_cast<int>(PolicyVerdict::UnsupportedValue));
}

ITF_TEST(policy_rejects_starvation_guard_without_bounds) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule rule = policy.rule(TrafficClass::DecodeStream);
  rule.starvation_protected = true;
  rule.max_defer_count = 0;
  (void)policy.set_rule(TrafficClass::DecodeStream, rule);
  ITF_CHECK_EQ(static_cast<int>(policy.validate()), static_cast<int>(PolicyVerdict::BoundsExceeded));
}

ITF_TEST(policy_round_trips_through_canonical_encoding) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(4));
  policy.set_allow_unknown_stage(true);
  policy.set_congestion_queue_depth(17);
  policy.set_defer_hint_nanos(1234567);
  ClassPolicyRule rule = policy.rule(TrafficClass::Unknown);
  rule.enabled = true;
  rule.priority = PriorityClass::Background;
  rule.pacing = PacingClass::BestEffort;
  rule.integrity = IntegrityClass::Checksum;
  rule.isolation = IsolationDomain::Background;
  (void)policy.set_rule(TrafficClass::Unknown, rule);

  Writer writer;
  policy.encode(writer);
  ITF_REQUIRE(writer.ok());
  Reader reader(ByteSpan(writer.buffer().data(), writer.buffer().size()));
  const auto decoded = PolicyConfig::decode(reader);
  ITF_REQUIRE(decoded.ok());
  ITF_REQUIRE_STATUS_OK(reader.finish());
  ITF_CHECK_EQ(decoded.value().generation().value(), 4ULL);
  ITF_CHECK(decoded.value().allow_unknown_stage());
  ITF_CHECK_EQ(decoded.value().congestion_queue_depth(), 17U);
  ITF_CHECK_EQ(decoded.value().rule(TrafficClass::Unknown).enabled, true);
  ITF_CHECK_EQ(decoded.value().rule(TrafficClass::DecodeStream).reserved_weight, 200U);
}

ITF_TEST(policy_decoding_refuses_trailing_and_wrong_vocabulary) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Writer writer;
  policy.encode(writer);
  ITF_REQUIRE(writer.ok());
  std::vector<std::uint8_t> bytes = writer.buffer();
  bytes.push_back(0x00);
  Reader reader(ByteSpan(bytes.data(), bytes.size()));
  const auto decoded = PolicyConfig::decode(reader);
  ITF_REQUIRE(decoded.ok());
  ITF_CHECK_EQ(static_cast<int>(reader.finish().code()), static_cast<int>(StatusCode::TrailingGarbage));
}

ITF_TEST(policy_decoding_refuses_a_foreign_rule_count) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  Writer writer;
  policy.encode(writer);
  std::vector<std::uint8_t> bytes = writer.buffer();
  // The rule count follows the generation, three booleans, three scalars,
  // two booleans and two scalars; patch the two-byte count in place.
  const std::size_t offset = 8 + 1 + 1 + 1 + 4 + 8 + 4 + 1 + 8 + 8;
  bytes[offset] = 3;
  bytes[offset + 1] = 0;
  Reader reader(ByteSpan(bytes.data(), bytes.size()));
  const auto decoded = PolicyConfig::decode(reader);
  ITF_CHECK(!decoded.ok());
  ITF_CHECK_EQ(static_cast<int>(decoded.code()), static_cast<int>(StatusCode::MalformedInput));
}

ITF_TEST(policy_set_rule_rejects_out_of_range_classes) {
  PolicyConfig policy = PolicyConfig::canonical_defaults(PolicyGeneration::from(1));
  ClassPolicyRule rule;
  ITF_CHECK_EQ(static_cast<int>(policy.set_rule(TrafficClass::Count, rule).code()),
               static_cast<int>(StatusCode::InvalidArgument));
}

}  // namespace

int main(int argc, char** argv) { return itf::test::run_all("test_policy_engine", argc, argv); }
