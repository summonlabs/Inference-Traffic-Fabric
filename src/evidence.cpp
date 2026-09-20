// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/evidence.hpp"

namespace itf {

std::string_view to_string(EvidenceState value) noexcept {
  switch (value) {
    case EvidenceState::Unknown: return "UNKNOWN";
    case EvidenceState::Current: return "CURRENT";
    case EvidenceState::Stale: return "STALE";
    case EvidenceState::RevalidationRequired: return "REVALIDATION_REQUIRED";
    case EvidenceState::Count: return "COUNT";
  }
  return "UNKNOWN";
}

std::string_view to_string(EvidenceProvenance value) noexcept {
  switch (value) {
    case EvidenceProvenance::Unknown: return "UNKNOWN";
    case EvidenceProvenance::Synthetic: return "SYNTHETIC";
    case EvidenceProvenance::Measured: return "MEASURED";
    case EvidenceProvenance::Count: return "COUNT";
  }
  return "UNKNOWN";
}

bool TopologyEvidence::is_valid() const noexcept {
  if (provenance == EvidenceProvenance::Unknown) return false;
  if (node_count == 0) return false;
  if (link_count > node_count * node_count) return false;
  if (accelerator_count > node_count * 64U) return false;
  return true;
}

bool TopologyEvidence::is_authoritative() const noexcept {
  return is_valid() && !generation.is_unset() && !observed_boot.is_nil();
}

std::string TopologyEvidence::to_string() const {
  std::string out;
  out.reserve(160);
  out.append("generation=");
  out.append(std::to_string(generation.value()));
  out.append(" provenance=");
  out.append(itf::to_string(provenance));
  out.append(" nodes=");
  out.append(std::to_string(node_count));
  out.append(" accelerators=");
  out.append(std::to_string(accelerator_count));
  out.append(" links=");
  out.append(std::to_string(link_count));
  out.append(" fabric_bytes_per_sec=");
  out.append(std::to_string(fabric_bytes_per_sec));
  out.append(" queue_depth=");
  out.append(std::to_string(queue_depth));
  out.append(" congested=");
  out.append(congestion_signalled ? "true" : "false");
  out.append(" disaggregated=");
  out.append(disaggregated ? "true" : "false");
  return out;
}

bool SloContract::is_valid() const noexcept {
  return !generation.is_unset() && !request.is_nil() && tail_latency_budget_nanos > 0 &&
         min_stream_share_per_mille <= 1000U;
}

bool RouteDecision::is_valid() const noexcept {
  return !generation.is_unset() && !request.is_nil() && !attempt.is_nil();
}

bool ModelState::is_valid() const noexcept {
  return model_hash != 0 && !generation.is_unset();
}

bool StateGenerationRecord::is_valid() const noexcept {
  return state_hash != 0 && !generation.is_unset();
}

}  // namespace itf
