// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "itf/accounting.hpp"

namespace itf {

std::string AccountingSnapshot::to_string() const {
  std::string out;
  out.reserve(512);
  out.append("active_flows=");
  out.append(std::to_string(active_flows_total));
  out.append(" peak_active_flows=");
  out.append(std::to_string(peak_active_flows_total));
  out.append(" pending_deferrals=");
  out.append(std::to_string(pending_deferrals));
  out.append(" live_requests=");
  out.append(std::to_string(live_requests));
  out.append(" retained_requests=");
  out.append(std::to_string(retained_requests));
  out.append(" requests{registered=");
  out.append(std::to_string(requests_registered));
  out.append(" completed=");
  out.append(std::to_string(requests_completed));
  out.append(" cancelled=");
  out.append(std::to_string(requests_cancelled));
  out.append(" failed=");
  out.append(std::to_string(requests_failed));
  out.append(" interrupted=");
  out.append(std::to_string(requests_interrupted));
  out.append(" evicted=");
  out.append(std::to_string(requests_evicted));
  out.append("} completions{committed=");
  out.append(std::to_string(completions_committed));
  out.append(" suppressed=");
  out.append(std::to_string(completions_suppressed));
  out.append(" refused=");
  out.append(std::to_string(completions_refused));
  out.append("}");
  for (std::size_t index = 0; index < kTrafficClassCount; ++index) {
    const ClassAccounting& entry = per_class[index];
    if (entry.allowed == 0 && entry.degraded == 0 && entry.deferred == 0 && entry.rejected == 0 &&
        entry.active_flows == 0) {
      continue;
    }
    out.append(" ");
    out.append(label(static_cast<TrafficClass>(index)));
    out.append("{allowed=");
    out.append(std::to_string(entry.allowed));
    out.append(" degraded=");
    out.append(std::to_string(entry.degraded));
    out.append(" deferred=");
    out.append(std::to_string(entry.deferred));
    out.append(" rejected=");
    out.append(std::to_string(entry.rejected));
    out.append(" active=");
    out.append(std::to_string(entry.active_flows));
    out.append("}");
  }
  return out;
}

}  // namespace itf
