// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/traffic.hpp"

#include <array>

namespace itf {
namespace {

constexpr std::array<std::string_view, kTrafficClassCount> kTrafficLabels = {
    "UNKNOWN",   "REQUEST_INGRESS",   "PREFILL_INPUT",     "PREFILL_DECODE_HANDOFF",
    "KV_TRANSFER", "STATE_TRANSFER",  "MODEL_TRANSFER",    "ADAPTER_TRANSFER",
    "DECODE_STREAM", "SPECULATIVE_BRANCH", "RESPONSE_EGRESS", "CONTROL"};

constexpr std::array<std::string_view, kServingStageCount> kStageLabels = {
    "UNKNOWN",         "ADMITTED",      "ROUTING",       "PREFILL_QUEUED", "PREFILL_RUNNING",
    "HANDOFF_PENDING", "DECODE_QUEUED", "DECODE_RUNNING", "STREAMING",     "COMPLETING",
    "COMPLETED",       "CANCELLED",     "FAILED"};

}  // namespace

std::string_view label(TrafficClass value) noexcept {
  const std::size_t index = static_cast<std::size_t>(value);
  return index < kTrafficClassCount ? kTrafficLabels[index] : kTrafficLabels[0];
}

std::string_view to_string(TrafficClass value) noexcept { return label(value); }

std::optional<TrafficClass> traffic_class_from_label(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    if (kTrafficLabels[index] == text) return static_cast<TrafficClass>(index);
  }
  return std::nullopt;
}

std::string_view label(ServingStage value) noexcept {
  const std::size_t index = static_cast<std::size_t>(value);
  return index < kServingStageCount ? kStageLabels[index] : kStageLabels[0];
}

std::string_view to_string(ServingStage value) noexcept { return label(value); }

std::optional<ServingStage> serving_stage_from_label(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kServingStageCount; ++index) {
    if (kStageLabels[index] == text) return static_cast<ServingStage>(index);
  }
  return std::nullopt;
}

bool is_terminal(ServingStage value) noexcept {
  return value == ServingStage::Completed || value == ServingStage::Cancelled ||
         value == ServingStage::Failed;
}

bool is_prefill_stage(ServingStage value) noexcept {
  return value == ServingStage::PrefillQueued || value == ServingStage::PrefillRunning;
}

bool is_decode_stage(ServingStage value) noexcept {
  return value == ServingStage::DecodeQueued || value == ServingStage::DecodeRunning ||
         value == ServingStage::Streaming;
}

bool is_serving_active(ServingStage value) noexcept {
  return value != ServingStage::Unknown && !is_terminal(value);
}

bool is_legal_stage_transition(ServingStage from, ServingStage to) noexcept {
  if (to == ServingStage::Unknown) return false;
  if (from == to) return true;
  if (is_terminal(from)) return false;
  switch (from) {
    case ServingStage::Unknown:
      return to == ServingStage::Admitted;
    case ServingStage::Admitted:
      return to == ServingStage::Routing || to == ServingStage::PrefillQueued ||
             to == ServingStage::DecodeQueued || to == ServingStage::Cancelled ||
             to == ServingStage::Failed;
    case ServingStage::Routing:
      return to == ServingStage::PrefillQueued || to == ServingStage::DecodeQueued ||
             to == ServingStage::Cancelled || to == ServingStage::Failed;
    case ServingStage::PrefillQueued:
      return to == ServingStage::PrefillRunning || to == ServingStage::Cancelled ||
             to == ServingStage::Failed;
    case ServingStage::PrefillRunning:
      return to == ServingStage::HandoffPending || to == ServingStage::DecodeQueued ||
             to == ServingStage::Cancelled || to == ServingStage::Failed;
    case ServingStage::HandoffPending:
      return to == ServingStage::DecodeQueued || to == ServingStage::DecodeRunning ||
             to == ServingStage::Cancelled || to == ServingStage::Failed;
    case ServingStage::DecodeQueued:
      return to == ServingStage::DecodeRunning || to == ServingStage::Cancelled ||
             to == ServingStage::Failed;
    case ServingStage::DecodeRunning:
      return to == ServingStage::Streaming || to == ServingStage::Completing ||
             to == ServingStage::Cancelled || to == ServingStage::Failed;
    case ServingStage::Streaming:
      return to == ServingStage::Completing || to == ServingStage::Cancelled ||
             to == ServingStage::Failed;
    case ServingStage::Completing:
      return to == ServingStage::Completed || to == ServingStage::Cancelled ||
             to == ServingStage::Failed;
    case ServingStage::Completed:
    case ServingStage::Cancelled:
    case ServingStage::Failed:
    case ServingStage::Count:
      return false;
  }
  return false;
}

TrafficClass canonical_class_for_stage(ServingStage stage) noexcept {
  switch (stage) {
    case ServingStage::Admitted:
    case ServingStage::Routing:
      return TrafficClass::RequestIngress;
    case ServingStage::PrefillQueued:
    case ServingStage::PrefillRunning:
      return TrafficClass::PrefillInput;
    case ServingStage::HandoffPending:
    case ServingStage::DecodeQueued:
      return TrafficClass::PrefillDecodeHandoff;
    case ServingStage::DecodeRunning:
    case ServingStage::Streaming:
      return TrafficClass::DecodeStream;
    case ServingStage::Completing:
      return TrafficClass::ResponseEgress;
    case ServingStage::Unknown:
    case ServingStage::Completed:
    case ServingStage::Cancelled:
    case ServingStage::Failed:
    case ServingStage::Count:
      return TrafficClass::Unknown;
  }
  return TrafficClass::Unknown;
}

bool is_class_legal_for_stage(TrafficClass traffic, ServingStage stage) noexcept {
  if (traffic == TrafficClass::Control) return is_serving_active(stage);
  switch (stage) {
    case ServingStage::Admitted:
    case ServingStage::Routing:
      return traffic == TrafficClass::RequestIngress;
    case ServingStage::PrefillQueued:
    case ServingStage::PrefillRunning:
      return traffic == TrafficClass::PrefillInput;
    case ServingStage::HandoffPending:
      return traffic == TrafficClass::PrefillDecodeHandoff || traffic == TrafficClass::KvTransfer ||
             traffic == TrafficClass::StateTransfer;
    case ServingStage::DecodeQueued:
      return traffic == TrafficClass::PrefillDecodeHandoff || traffic == TrafficClass::KvTransfer;
    case ServingStage::DecodeRunning:
      return traffic == TrafficClass::KvTransfer || traffic == TrafficClass::DecodeStream ||
             traffic == TrafficClass::SpeculativeBranch;
    case ServingStage::Streaming:
      return traffic == TrafficClass::DecodeStream || traffic == TrafficClass::SpeculativeBranch ||
             traffic == TrafficClass::ResponseEgress;
    case ServingStage::Completing:
      return traffic == TrafficClass::ResponseEgress;
    case ServingStage::Unknown:
    case ServingStage::Completed:
    case ServingStage::Cancelled:
    case ServingStage::Failed:
    case ServingStage::Count:
      return false;
  }
  return false;
}

std::string_view to_string(FlowDirection value) noexcept {
  switch (value) {
    case FlowDirection::Unknown: return "UNKNOWN";
    case FlowDirection::Ingress: return "INGRESS";
    case FlowDirection::Egress: return "EGRESS";
    case FlowDirection::Lateral: return "LATERAL";
    case FlowDirection::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(PriorityClass value) noexcept {
  switch (value) {
    case PriorityClass::Unknown: return "UNKNOWN";
    case PriorityClass::LatencyCritical: return "LATENCY_CRITICAL";
    case PriorityClass::Interactive: return "INTERACTIVE";
    case PriorityClass::Standard: return "STANDARD";
    case PriorityClass::Background: return "BACKGROUND";
    case PriorityClass::Bulk: return "BULK";
    case PriorityClass::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(PacingClass value) noexcept {
  switch (value) {
    case PacingClass::Unknown: return "UNKNOWN";
    case PacingClass::Immediate: return "IMMEDIATE";
    case PacingClass::Paced: return "PACED";
    case PacingClass::RateLimited: return "RATE_LIMITED";
    case PacingClass::BestEffort: return "BEST_EFFORT";
    case PacingClass::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(IntegrityClass value) noexcept {
  switch (value) {
    case IntegrityClass::Unknown: return "UNKNOWN";
    case IntegrityClass::None: return "NONE";
    case IntegrityClass::Checksum: return "CHECKSUM";
    case IntegrityClass::ChecksumAndVerify: return "CHECKSUM_AND_VERIFY";
    case IntegrityClass::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(IsolationDomain value) noexcept {
  switch (value) {
    case IsolationDomain::Unknown: return "UNKNOWN";
    case IsolationDomain::LatencyCritical: return "LATENCY_CRITICAL";
    case IsolationDomain::Interactive: return "INTERACTIVE";
    case IsolationDomain::Background: return "BACKGROUND";
    case IsolationDomain::Bulk: return "BULK";
    case IsolationDomain::Control: return "CONTROL";
    case IsolationDomain::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(PriorityHint value) noexcept {
  switch (value) {
    case PriorityHint::None: return "NONE";
    case PriorityHint::LatencyCritical: return "LATENCY_CRITICAL";
    case PriorityHint::Interactive: return "INTERACTIVE";
    case PriorityHint::Batch: return "BATCH";
    case PriorityHint::Background: return "BACKGROUND";
    case PriorityHint::Count: return "COUNT";
  }
  return "NONE";
}

std::string_view to_string(SubjectKind value) noexcept {
  switch (value) {
    case SubjectKind::Unknown: return "UNKNOWN";
    case SubjectKind::Serving: return "SERVING";
    case SubjectKind::Transfer: return "TRANSFER";
    case SubjectKind::Count: return "COUNT";
  }
  return "UNKNOWN";
}

bool TrafficSubject::is_valid() const noexcept {
  switch (kind) {
    case SubjectKind::Serving:
      return !request.is_nil() && !attempt.is_nil() && !request_generation.is_unset();
    case SubjectKind::Transfer:
      return !transfer.is_nil();
    case SubjectKind::Unknown:
    case SubjectKind::Count:
      return false;
  }
  return false;
}

std::string TrafficSubject::to_string() const {
  std::string out;
  out.append(itf::to_string(kind));
  switch (kind) {
    case SubjectKind::Serving:
      out.append(" request=");
      out.append(request.to_string());
      out.append(" attempt=");
      out.append(attempt.to_string());
      out.append(" generation=");
      out.append(std::to_string(request_generation.value()));
      break;
    case SubjectKind::Transfer:
      out.append(" transfer=");
      out.append(transfer.to_string());
      break;
    case SubjectKind::Unknown:
    case SubjectKind::Count:
      break;
  }
  return out;
}

std::string GenerationBinding::to_string() const {
  std::string out;
  const auto append = [&out](std::string_view name, const auto& value) {
    if (!value.has_value()) return;
    out.append(name);
    out.push_back('=');
    out.append(std::to_string(value->value()));
    out.push_back(' ');
  };
  append("request", request_generation);
  append("model", model_generation);
  append("state", state_generation);
  append("route", route_decision);
  append("slo", slo_contract);
  append("topology", topology);
  append("policy", policy);
  append("epoch", coordinator_epoch);
  if (out.empty()) out = "none";
  return out;
}

bool TrafficRequest::structurally_valid() const noexcept {
  if (subject.has_value() && !subject->is_valid()) return false;
  if (!subject.has_value() && declared_class != TrafficClass::ModelTransfer &&
      declared_class != TrafficClass::AdapterTransfer && declared_class != TrafficClass::Control) {
    return false;
  }
  if (declared_class >= TrafficClass::Count) return false;
  if (declared_stage >= ServingStage::Count) return false;
  if (direction >= FlowDirection::Count) return false;
  if (priority_hint >= PriorityHint::Count) return false;
  if (total_chunks != 0 && chunk_index >= total_chunks) return false;
  if (deadline.is_set() && deadline_after_nanos != 0) return false;
  return true;
}

std::string TrafficTreatment::to_string() const {
  std::string out;
  out.append("class=");
  out.append(label(traffic_class));
  out.append(" priority=");
  out.append(itf::to_string(priority));
  out.append(" pacing=");
  out.append(itf::to_string(pacing));
  out.append(" integrity=");
  out.append(itf::to_string(integrity));
  out.append(" isolation=");
  out.append(itf::to_string(isolation));
  out.append(" reserved=");
  out.append(std::to_string(reserved_weight));
  if (rate_limit_bytes_per_sec != 0) {
    out.append(" rate=");
    out.append(std::to_string(rate_limit_bytes_per_sec));
  }
  if (!flow.is_nil()) {
    out.append(" flow=");
    out.append(flow.to_string());
  }
  return out;
}

}  // namespace itf
