// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/policy.hpp"

#include <string>

namespace itf {
namespace {

constexpr std::uint64_t kMicrosecond = 1000ULL;
constexpr std::uint64_t kMillisecond = 1000ULL * kMicrosecond;
constexpr std::uint64_t kSecond = 1000ULL * kMillisecond;
constexpr std::uint64_t kKib = 1024ULL;
constexpr std::uint64_t kMib = 1024ULL * kKib;

[[nodiscard]] bool is_bulk_rule(const ClassPolicyRule& rule) noexcept {
  return rule.yields_to_latency || rule.priority == PriorityClass::Bulk;
}

}  // namespace

std::string_view to_string(PolicyVerdict value) noexcept {
  switch (value) {
    case PolicyVerdict::Unknown: return "UNKNOWN";
    case PolicyVerdict::Valid: return "VALID";
    case PolicyVerdict::MissingRule: return "MISSING_RULE";
    case PolicyVerdict::ReservedWeightOverflow: return "RESERVED_WEIGHT_OVERFLOW";
    case PolicyVerdict::BulkReserved: return "BULK_RESERVED";
    case PolicyVerdict::UnsupportedValue: return "UNSUPPORTED_VALUE";
    case PolicyVerdict::BoundsExceeded: return "BOUNDS_EXCEEDED";
    case PolicyVerdict::Count: return "COUNT";
  }
  return "UNKNOWN";
}

Status PolicyConfig::set_rule(TrafficClass traffic, const ClassPolicyRule& rule) {
  const std::size_t index = static_cast<std::size_t>(traffic);
  if (index >= kTrafficClassCount) {
    return Status(StatusCode::InvalidArgument, "traffic class out of range for policy rule");
  }
  rules_[index] = rule;
  return Status::success();
}

std::uint32_t PolicyConfig::reserved_weight_total() const noexcept {
  std::uint64_t total = 0;
  for (std::size_t index = 1; index < kTrafficClassCount; ++index) {
    const ClassPolicyRule& rule = rules_[index];
    if (!rule.enabled) continue;
    if (is_bulk_rule(rule)) continue;
    total += rule.reserved_weight;
  }
  return total > kReservedWeightScale ? kReservedWeightScale : static_cast<std::uint32_t>(total);
}

PolicyVerdict PolicyConfig::validate() const noexcept {
  if (max_explanation_steps_ > kMaxExplanationHardLimit) return PolicyVerdict::BoundsExceeded;
  if ((allow_unknown_traffic_class_ || allow_unknown_stage_) && !rules_[0].enabled) {
    return PolicyVerdict::MissingRule;
  }

  std::uint64_t reserved_total = 0;
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    const ClassPolicyRule& rule = rules_[index];
    if (index != 0 && !rule.enabled) continue;
    if (index == 0 && !rule.enabled) continue;
    if (rule.priority == PriorityClass::Unknown || rule.pacing == PacingClass::Unknown ||
        rule.integrity == IntegrityClass::Unknown || rule.isolation == IsolationDomain::Unknown) {
      return PolicyVerdict::UnsupportedValue;
    }
    if (rule.reserved_weight > kReservedWeightScale) return PolicyVerdict::BoundsExceeded;
    if (is_bulk_rule(rule) && rule.reserved_weight != 0) return PolicyVerdict::BulkReserved;
    if (rule.starvation_protected &&
        (rule.max_defer_count == 0 || rule.max_defer_horizon_nanos == 0)) {
      return PolicyVerdict::BoundsExceeded;
    }
    if (rule.requires_deadline && rule.max_defer_horizon_nanos > 0 &&
        rule.max_defer_horizon_nanos > kSecond * 60ULL) {
      return PolicyVerdict::BoundsExceeded;
    }
    if (is_bulk_rule(rule)) continue;
    reserved_total += rule.reserved_weight;
  }
  if (reserved_total > kReservedWeightScale) return PolicyVerdict::ReservedWeightOverflow;
  return PolicyVerdict::Valid;
}

Status PolicyConfig::validate_status() const {
  const PolicyVerdict verdict = validate();
  if (verdict == PolicyVerdict::Valid) return Status::success();
  return Status(StatusCode::PolicyInvalid, std::string(itf::to_string(verdict)));
}

PolicyConfig PolicyConfig::canonical_defaults(PolicyGeneration generation) {
  PolicyConfig config;
  config.set_generation(generation);

  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Interactive;
    rule.pacing = PacingClass::Immediate;
    rule.integrity = IntegrityClass::Checksum;
    rule.isolation = IsolationDomain::Interactive;
    rule.reserved_weight = 100;
    rule.max_burst_bytes = 256 * kKib;
    rule.max_in_flight_bytes = 16 * kMib;
    rule.requires_slo_contract = true;
    rule.requires_current_route = true;
    rule.requires_deadline = true;
    rule.starvation_protected = true;
    rule.max_defer_count = 8;
    rule.max_defer_horizon_nanos = 50 * kMillisecond;
    (void)config.set_rule(TrafficClass::RequestIngress, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Interactive;
    rule.pacing = PacingClass::Paced;
    rule.integrity = IntegrityClass::Checksum;
    rule.isolation = IsolationDomain::LatencyCritical;
    rule.reserved_weight = 120;
    rule.max_burst_bytes = 1 * kMib;
    rule.max_in_flight_bytes = 128 * kMib;
    rule.requires_current_topology = true;
    rule.requires_slo_contract = true;
    rule.requires_current_route = true;
    rule.requires_model_generation = true;
    rule.requires_deadline = true;
    rule.starvation_protected = true;
    rule.max_defer_count = 8;
    rule.max_defer_horizon_nanos = 50 * kMillisecond;
    (void)config.set_rule(TrafficClass::PrefillInput, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::LatencyCritical;
    rule.pacing = PacingClass::Immediate;
    rule.integrity = IntegrityClass::ChecksumAndVerify;
    rule.isolation = IsolationDomain::LatencyCritical;
    rule.reserved_weight = 150;
    rule.max_burst_bytes = 2 * kMib;
    rule.max_in_flight_bytes = 256 * kMib;
    rule.requires_current_topology = true;
    rule.requires_slo_contract = true;
    rule.requires_current_route = true;
    rule.requires_model_generation = true;
    rule.requires_state_generation = true;
    rule.requires_deadline = true;
    rule.starvation_protected = true;
    rule.max_defer_count = 8;
    rule.max_defer_horizon_nanos = 50 * kMillisecond;
    (void)config.set_rule(TrafficClass::PrefillDecodeHandoff, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::LatencyCritical;
    rule.pacing = PacingClass::Paced;
    rule.integrity = IntegrityClass::ChecksumAndVerify;
    rule.isolation = IsolationDomain::LatencyCritical;
    rule.reserved_weight = 150;
    rule.max_burst_bytes = 4 * kMib;
    rule.max_in_flight_bytes = 512 * kMib;
    rule.requires_current_topology = true;
    rule.requires_slo_contract = true;
    rule.requires_current_route = true;
    rule.requires_model_generation = true;
    rule.requires_state_generation = true;
    rule.requires_deadline = true;
    rule.starvation_protected = true;
    rule.max_defer_count = 8;
    rule.max_defer_horizon_nanos = 50 * kMillisecond;
    (void)config.set_rule(TrafficClass::KvTransfer, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Standard;
    rule.pacing = PacingClass::Paced;
    rule.integrity = IntegrityClass::ChecksumAndVerify;
    rule.isolation = IsolationDomain::Background;
    rule.reserved_weight = 60;
    rule.max_burst_bytes = 8 * kMib;
    rule.max_in_flight_bytes = 512 * kMib;
    rule.requires_current_topology = true;
    rule.requires_current_route = true;
    rule.requires_model_generation = true;
    rule.requires_state_generation = true;
    rule.starvation_protected = false;
    rule.max_defer_count = 64;
    rule.max_defer_horizon_nanos = 2 * kSecond;
    (void)config.set_rule(TrafficClass::StateTransfer, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Bulk;
    rule.pacing = PacingClass::RateLimited;
    rule.integrity = IntegrityClass::ChecksumAndVerify;
    rule.isolation = IsolationDomain::Bulk;
    rule.reserved_weight = 0;
    rule.max_burst_bytes = 16 * kMib;
    rule.rate_limit_bytes_per_sec = 512 * kMib;
    rule.requires_current_topology = true;
    rule.requires_model_generation = true;
    rule.yields_to_latency = true;
    rule.starvation_protected = false;
    rule.max_defer_count = 256;
    rule.max_defer_horizon_nanos = 30 * kSecond;
    (void)config.set_rule(TrafficClass::ModelTransfer, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Bulk;
    rule.pacing = PacingClass::RateLimited;
    rule.integrity = IntegrityClass::Checksum;
    rule.isolation = IsolationDomain::Bulk;
    rule.reserved_weight = 0;
    rule.max_burst_bytes = 8 * kMib;
    rule.rate_limit_bytes_per_sec = 256 * kMib;
    rule.requires_current_topology = true;
    rule.requires_model_generation = true;
    rule.yields_to_latency = true;
    rule.starvation_protected = false;
    rule.max_defer_count = 256;
    rule.max_defer_horizon_nanos = 30 * kSecond;
    (void)config.set_rule(TrafficClass::AdapterTransfer, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::LatencyCritical;
    rule.pacing = PacingClass::Paced;
    rule.integrity = IntegrityClass::Checksum;
    rule.isolation = IsolationDomain::LatencyCritical;
    rule.reserved_weight = 200;
    rule.max_burst_bytes = 512 * kKib;
    rule.max_in_flight_bytes = 256 * kMib;
    rule.requires_slo_contract = true;
    rule.requires_current_route = true;
    rule.requires_model_generation = true;
    rule.requires_deadline = true;
    rule.starvation_protected = true;
    rule.max_defer_count = 8;
    rule.max_defer_horizon_nanos = 50 * kMillisecond;
    (void)config.set_rule(TrafficClass::DecodeStream, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Background;
    rule.pacing = PacingClass::BestEffort;
    rule.integrity = IntegrityClass::Checksum;
    rule.isolation = IsolationDomain::Background;
    rule.reserved_weight = 0;
    rule.max_burst_bytes = 1 * kMib;
    rule.max_in_flight_bytes = 64 * kMib;
    rule.requires_current_route = true;
    rule.requires_model_generation = true;
    rule.yields_to_latency = true;
    rule.starvation_protected = false;
    rule.max_defer_count = 64;
    rule.max_defer_horizon_nanos = 5 * kSecond;
    (void)config.set_rule(TrafficClass::SpeculativeBranch, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::Interactive;
    rule.pacing = PacingClass::Immediate;
    rule.integrity = IntegrityClass::Checksum;
    rule.isolation = IsolationDomain::Interactive;
    rule.reserved_weight = 100;
    rule.max_burst_bytes = 256 * kKib;
    rule.max_in_flight_bytes = 64 * kMib;
    rule.requires_slo_contract = true;
    rule.requires_current_route = true;
    rule.requires_deadline = true;
    rule.starvation_protected = true;
    rule.max_defer_count = 8;
    rule.max_defer_horizon_nanos = 50 * kMillisecond;
    (void)config.set_rule(TrafficClass::ResponseEgress, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = true;
    rule.priority = PriorityClass::LatencyCritical;
    rule.pacing = PacingClass::Immediate;
    rule.integrity = IntegrityClass::ChecksumAndVerify;
    rule.isolation = IsolationDomain::Control;
    rule.reserved_weight = 20;
    rule.max_burst_bytes = 64 * kKib;
    rule.max_in_flight_bytes = 16 * kMib;
    rule.starvation_protected = true;
    rule.max_defer_count = 32;
    rule.max_defer_horizon_nanos = 1 * kSecond;
    (void)config.set_rule(TrafficClass::Control, rule);
  }
  {
    ClassPolicyRule rule;
    rule.enabled = false;
    (void)config.set_rule(TrafficClass::Unknown, rule);
  }
  return config;
}

void PolicyConfig::encode(Writer& writer) const {
  writer.generation(generation_);
  writer.boolean(allow_unknown_stage_);
  writer.boolean(allow_unknown_traffic_class_);
  writer.u32(max_explanation_steps_);
  writer.u64(bulk_in_flight_cap_bytes_);
  writer.u32(congestion_queue_depth_);
  writer.boolean(congestion_yields_bulk_);
  writer.u64(defer_hint_nanos_);
  writer.u64(stale_evidence_grace_nanos_);
  writer.u16(static_cast<std::uint16_t>(kTrafficClassCount));
  for (const ClassPolicyRule& rule : rules_) {
    writer.boolean(rule.enabled);
    writer.enumeration(rule.priority);
    writer.enumeration(rule.pacing);
    writer.enumeration(rule.integrity);
    writer.enumeration(rule.isolation);
    writer.u32(rule.reserved_weight);
    writer.u64(rule.max_burst_bytes);
    writer.u64(rule.rate_limit_bytes_per_sec);
    writer.u64(rule.max_in_flight_bytes);
    writer.boolean(rule.requires_current_topology);
    writer.boolean(rule.requires_slo_contract);
    writer.boolean(rule.requires_current_route);
    writer.boolean(rule.requires_model_generation);
    writer.boolean(rule.requires_state_generation);
    writer.boolean(rule.requires_deadline);
    writer.boolean(rule.starvation_protected);
    writer.u32(rule.max_defer_count);
    writer.u64(rule.max_defer_horizon_nanos);
    writer.boolean(rule.yields_to_latency);
  }
}

Result<PolicyConfig> PolicyConfig::decode(Reader& reader) {
  PolicyConfig config;
  config.set_generation(reader.generation<PolicyGenerationTag>());
  config.allow_unknown_stage_ = reader.boolean();
  config.allow_unknown_traffic_class_ = reader.boolean();
  config.max_explanation_steps_ = reader.u32();
  config.bulk_in_flight_cap_bytes_ = reader.u64();
  config.congestion_queue_depth_ = reader.u32();
  config.congestion_yields_bulk_ = reader.boolean();
  config.defer_hint_nanos_ = reader.u64();
  config.stale_evidence_grace_nanos_ = reader.u64();
  const std::uint16_t rule_count = reader.u16();
  if (!reader.ok()) return Result<PolicyConfig>::failure(reader.fail_code(), "policy header");
  if (rule_count != static_cast<std::uint16_t>(kTrafficClassCount)) {
    return Result<PolicyConfig>::failure(StatusCode::MalformedInput,
                                         "policy rule count does not match the fabric vocabulary");
  }
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    ClassPolicyRule rule;
    rule.enabled = reader.boolean();
    rule.priority = reader.enumeration(PriorityClass::Count);
    rule.pacing = reader.enumeration(PacingClass::Count);
    rule.integrity = reader.enumeration(IntegrityClass::Count);
    rule.isolation = reader.enumeration(IsolationDomain::Count);
    rule.reserved_weight = reader.u32();
    rule.max_burst_bytes = reader.u64();
    rule.rate_limit_bytes_per_sec = reader.u64();
    rule.max_in_flight_bytes = reader.u64();
    rule.requires_current_topology = reader.boolean();
    rule.requires_slo_contract = reader.boolean();
    rule.requires_current_route = reader.boolean();
    rule.requires_model_generation = reader.boolean();
    rule.requires_state_generation = reader.boolean();
    rule.requires_deadline = reader.boolean();
    rule.starvation_protected = reader.boolean();
    rule.max_defer_count = reader.u32();
    rule.max_defer_horizon_nanos = reader.u64();
    rule.yields_to_latency = reader.boolean();
    if (!reader.ok()) {
      return Result<PolicyConfig>::failure(reader.fail_code(), "policy rule body");
    }
    config.rules_[index] = rule;
  }
  if (config.max_explanation_steps_ == 0 ||
      config.max_explanation_steps_ > kMaxExplanationHardLimit) {
    config.max_explanation_steps_ = kMaxExplanationHardLimit;
  }
  const PolicyVerdict verdict = config.validate();
  if (verdict != PolicyVerdict::Valid) {
    return Result<PolicyConfig>::failure(StatusCode::PolicyInvalid,
                                         std::string("decoded policy rejected: ") +
                                             std::string(itf::to_string(verdict)));
  }
  return Result<PolicyConfig>::success(config);
}

std::string PolicyConfig::to_string() const {
  std::string out;
  out.reserve(512);
  out.append("policy generation=");
  out.append(std::to_string(generation_.value()));
  out.append(" reserved_weight_total=");
  out.append(std::to_string(reserved_weight_total()));
  out.append(" allow_unknown_stage=");
  out.append(allow_unknown_stage_ ? "true" : "false");
  out.append(" allow_unknown_class=");
  out.append(allow_unknown_traffic_class_ ? "true" : "false");
  out.append(" bulk_in_flight_cap_bytes=");
  out.append(std::to_string(bulk_in_flight_cap_bytes_));
  out.append(" defer_hint_nanos=");
  out.append(std::to_string(defer_hint_nanos_));
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    const ClassPolicyRule& rule = rules_[index];
    out.push_back('\n');
    out.push_back(' ');
    out.push_back(' ');
    out.append(label(static_cast<TrafficClass>(index)));
    out.append(rule.enabled ? " enabled" : " disabled");
    if (!rule.enabled) continue;
    out.append(" priority=");
    out.append(itf::to_string(rule.priority));
    out.append(" pacing=");
    out.append(itf::to_string(rule.pacing));
    out.append(" integrity=");
    out.append(itf::to_string(rule.integrity));
    out.append(" isolation=");
    out.append(itf::to_string(rule.isolation));
    out.append(" reserved=");
    out.append(std::to_string(rule.reserved_weight));
    if (rule.starvation_protected) {
      out.append(" starvation_guard=");
      out.append(std::to_string(rule.max_defer_count));
      out.push_back('/');
      out.append(std::to_string(rule.max_defer_horizon_nanos));
    }
    if (rule.yields_to_latency) out.append(" yields_to_latency");
  }
  return out;
}

}  // namespace itf
